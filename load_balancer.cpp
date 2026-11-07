// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code). 
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection 


#include <iostream>
#include <string>
#include <cstdlib>
#include <cerrno>
#include <chrono>
#include <thread>
#include <mutex>
#include <poll.h>
#include <ctime>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "helper.h"
using namespace std;


// ---- Constants ----
const char *SERVER_PATH = "./server";      // the program we start when the server is down
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
int lbPort = 5000;
string serverHost = "127.0.0.0";
int serverPort = 5001;


// The bounded linked-list queue synchronizes each operation on queueMutex.
mutex queueMutex;

template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node *next;

        explicit Node(const T &value) : data(value), next(NULL) {}
    };
    Node *front, *rear;
    int count;
    const int capacity;
    mutex &queueMutexRef;

public:
    Queue(mutex &guard, int maxSize)
        : front(NULL), rear(NULL), count(0), capacity(maxSize), queueMutexRef(guard)
    {
    }

    Queue(const Queue &) = delete;
    Queue &operator=(const Queue &) = delete;

    ~Queue()
    {
        lock_guard<mutex> lock(queueMutexRef);
        while (front != NULL)
        {
            Node *node = front;
            front = front->next;
            delete node;
        }
        rear = NULL;
        count = 0;
    }

    bool enqueue(const T &val)
    {
        lock_guard<mutex> lock(queueMutexRef);
        if (count >= capacity)
        {
            return false;
        }

        Node *node = new Node(val);
        if (rear == NULL)
        {
            front = node;
        }
        else
        {
            rear->next = node;
        }
        rear = node;
        ++count;
        return true;
    }

    bool dequeue(T &value)
    {
        lock_guard<mutex> lock(queueMutexRef);
        if (front == NULL)
        {
            return false;
        }

        Node *node = front;
        value = node->data;
        front = node->next;
        if (front == NULL)
        {
            rear = NULL;
        }
        delete node;
        --count;
        return true;
    }

    bool isEmpty() const
    {
        lock_guard<mutex> lock(queueMutexRef);
        return front == NULL;
    }

    int size() const
    {
        lock_guard<mutex> lock(queueMutexRef);
        return count;
    }
};

Queue<ClientInfo> pendingClients(queueMutex, MAX_PENDING_CLIENTS);
volatile sig_atomic_t flag = 1;


// ---- Basic socket helpers ----
bool sendAll(int sock, const char *buf, size_t n) 
{
    size_t sent = 0;
    while (sent < n)
    {
        ssize_t result = send(sock, buf + sent, n - sent, MSG_NOSIGNAL);
        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (result == 0)
        {
            return false;
        }
        sent += static_cast<size_t>(result);
    }
    return true;
}
bool sendError(int sock, const string &message)
{
    const uint8_t status = TTDB_STATUS_ERROR;
    Checksum32 checksum;
    checksum.update(&status, sizeof(status));
    checksum.update(message.data(), message.size());

    uint8_t trailer[TTDB_CHECKSUM_TRAILER_SIZE];
    writeChecksumLittleEndian(checksum.value(), trailer);

    return sendAll(sock, reinterpret_cast<const char *>(&status), sizeof(status)) &&
           sendAll(sock, message.data(), message.size()) &&
           sendAll(sock, reinterpret_cast<const char *>(trailer), sizeof(trailer));
}
int createListenSocket(int port)
{ 
    if (port < 1 || port > 65535)
    {
        cerr << "Invalid load-balancer port." << endl;
        return -1;
    }

    int listenSocket = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listenSocket < 0)
    {
        perror("socket");
        return -1;
    }

    int reuseAddress = 1;
    if (setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR,
                   &reuseAddress, sizeof(reuseAddress)) < 0)
    {
        perror("setsockopt");
        close(listenSocket);
        return -1;
    }

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(listenSocket, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) < 0)
    {
        perror("bind");
        close(listenSocket);
        return -1;
    }
    if (listen(listenSocket, LISTEN_BACKLOG) < 0)
    {
        perror("listen");
        close(listenSocket);
        return -1;
    }
    return listenSocket;
}
int connectToServer(const char *host, int port)
{
    if (host == NULL || host[0] == '\0' || port < 1 || port > 65535)
    {
        return -1;
    }

    const string service = to_string(port);
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo *addresses = NULL;
    int lookupResult = getaddrinfo(host, service.c_str(), &hints, &addresses);
    if (lookupResult != 0)
    {
        cerr << "Could not resolve backend host: " << gai_strerror(lookupResult) << endl;
        return -1;
    }

    int connectedSocket = -1;
    for (addrinfo *address = addresses; address != NULL; address = address->ai_next)
    {
        int candidate = socket(address->ai_family,
                               address->ai_socktype | SOCK_CLOEXEC,
                               address->ai_protocol);
        if (candidate < 0)
        {
            continue;
        }

        if (connect(candidate, address->ai_addr, address->ai_addrlen) == 0)
        {
            connectedSocket = candidate;
            break;
        }
        close(candidate);
    }

    freeaddrinfo(addresses);
    return connectedSocket;
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================


bool startServer(int port)
{
    if (port < 1 || port > 65535)
    {
        return false;
    }

    string command = string(SERVER_PATH) + " " + to_string(port) + " &";
    return system(command.c_str()) == 0;
}
int connectToServerWithWakeup()
{
    int serverSocket = connectToServer(serverHost.c_str(), serverPort);
    if (serverSocket >= 0)
    {
        return serverSocket;
    }
    if (!startServer(serverPort))
    {
        return -1;
    }

    for (int attempt = 0; attempt < STARTUP_MAX_ATTEMPTS; ++attempt)
    {
        this_thread::sleep_for(
            chrono::milliseconds(STARTUP_POLL_INTERVAL_MS));
        serverSocket = connectToServer(serverHost.c_str(), serverPort);
        if (serverSocket >= 0)
        {
            return serverSocket;
        }
    }
    return -1;
}


// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================


void relayOneClient(ClientInfo &c)
{
    int serverSocket = connectToServerWithWakeup();
    if (serverSocket < 0)
    {
        if (!sendError(c.sock, "backend server failed to start"))
        {
            cerr << "Could not report backend startup failure to client." << endl;
        }
        close(c.sock);
        return;
    }

    char buffer[IO_BUFFER_SIZE];
    bool relayOk = true;
    while (true)
    {
        ssize_t received = recv(c.sock, buffer, sizeof(buffer), 0);
        if (received == 0)
        {
            break;
        }
        if (received < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("recv from client");
            relayOk = false;
            break;
        }
        if (!sendAll(serverSocket, buffer, static_cast<size_t>(received)))
        {
            cerr << "Could not relay client data to backend." << endl;
            relayOk = false;
            break;
        }
    }

    if (relayOk && shutdown(serverSocket, SHUT_WR) < 0)
    {
        perror("shutdown backend write side");
        relayOk = false;
    }

    if (relayOk)
    {
        while (true)
        {
            ssize_t received = recv(serverSocket, buffer, sizeof(buffer), 0);
            if (received == 0)
            {
                break;
            }
            if (received < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                perror("recv from backend");
                break;
            }
            if (!sendAll(c.sock, buffer, static_cast<size_t>(received)))
            {
                cerr << "Could not relay backend response to client." << endl;
                break;
            }
        }
    }

    close(serverSocket);
    close(c.sock);
}


// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================


void acceptLoop(int listenSock)
{
    while (flag)
    {
        pollfd listener = {};
        listener.fd = listenSock;
        listener.events = POLLIN;
        int ready = poll(&listener, 1, 100);
        if (ready == 0)
        {
            continue;
        }
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (flag)
            {
                perror("poll for client");
            }
            continue;
        }
        if ((listener.revents & POLLIN) == 0)
        {
            if (flag)
            {
                cerr << "Listening socket reported an unexpected event." << endl;
            }
            continue;
        }

        sockaddr_in peerAddress = {};
        socklen_t peerAddressLength = sizeof(peerAddress);
        int clientSocket = accept(
            listenSock, reinterpret_cast<sockaddr *>(&peerAddress), &peerAddressLength);
        if (clientSocket < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (flag)
            {
                perror("accept");
            }
            continue;
        }

        char addressText[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &peerAddress.sin_addr,
                      addressText, sizeof(addressText)) == NULL)
        {
            perror("inet_ntop");
            close(clientSocket);
            continue;
        }

        ClientInfo client;
        client.sock = clientSocket;
        client.address = addressText;
        client.port = ntohs(peerAddress.sin_port);
        client.arrivedAt = std::time(NULL);
        if (!pendingClients.enqueue(client))
        {
            if (!sendError(clientSocket, "server busy, try again later"))
            {
                cerr << "Could not report a full client queue." << endl;
            }
            close(clientSocket);
        }
    }
}
void dispatcherLoop()
{
    while (flag)
    {
        ClientInfo client;
        if (pendingClients.dequeue(client))
        {
            relayOneClient(client);
        }
        else
        {
            this_thread::sleep_for(chrono::milliseconds(10));
        }
    }

    ClientInfo waitingClient;
    while (pendingClients.dequeue(waitingClient))
    {
        close(waitingClient.sock);
    }
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}
int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    signal(SIGINT, handler); // whenever ctrl+c will be pressed to exit the program the flag wll be set so insead of using infinite loop you can loop on flag to safely close things and exit
    int listenSocket = createListenSocket(lbPort);
    if (listenSocket < 0)
    {
        return 1;
    }

    thread acceptor(acceptLoop, listenSocket);
    dispatcherLoop();
    acceptor.join();
    close(listenSocket);
    return 0;
}