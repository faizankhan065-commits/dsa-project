// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <vector>
#include <sys/socket.h>
#include <netdb.h>
#include "helper.h"
using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.0";
int LB_PORT = 5000;

// ---- Structs only the client needs ----
struct TdbgReader
{
    ssize_t file_index;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep; 
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(int sock, char *buf, size_t n)
{
    size_t received = 0;
    while (received < n)
    {
        ssize_t result = recv(sock, buf + received, n - received, 0);
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
        received += static_cast<size_t>(result);
    }
    return true;
}
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

int connectToHost(const char *host, int port)
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
        cerr << "Could not resolve load-balancer host: "
             << gai_strerror(lookupResult) << endl;
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
bool uploadSource(int sock, const char *sourcePath)
{
    if (sourcePath == NULL)
    {
        return false;
    }

    ifstream source(sourcePath, ios::binary);
    if (!source.is_open())
    {
        return false;
    }

    Checksum32 checksum;
    char buffer[IO_BUFFER_SIZE];
    while (source)
    {
        source.read(buffer, sizeof(buffer));
        streamsize bytesRead = source.gcount();
        if (bytesRead > 0)
        {
            checksum.update(buffer, static_cast<size_t>(bytesRead));
            if (!sendAll(sock, buffer, static_cast<size_t>(bytesRead)))
            {
                return false;
            }
        }
    }
    if (source.bad() || (source.fail() && !source.eof()))
    {
        return false;
    }

    uint8_t trailer[TTDB_CHECKSUM_TRAILER_SIZE];
    writeChecksumLittleEndian(checksum.value(), trailer);
    if (!sendAll(sock, reinterpret_cast<const char *>(trailer), sizeof(trailer)))
    {
        return false;
    }
    return shutdown(sock, SHUT_WR) == 0;
}
bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    outError.clear();
    if (outTdbgPath == NULL || outTdbgPath[0] == '\0')
    {
        outError = "invalid output path";
        return false;
    }

    ofstream outputFile;
    bool temporaryFileCreated = false;
    string temporaryPath;
    bool statusKnown = false;
    uint8_t status = 0;
    TrailerChecker checker;
    char inputBuffer[IO_BUFFER_SIZE];
    uint8_t safeBuffer[IO_BUFFER_SIZE];

    const auto discardOutput = [&outputFile, &temporaryFileCreated, &temporaryPath]()
    {
        if (outputFile.is_open())
        {
            outputFile.close();
        }
        if (temporaryFileCreated)
        {
            std::remove(temporaryPath.c_str());
            temporaryFileCreated = false;
        }
    };

    const auto consumeSafeBytes = [&](size_t safeSize) -> bool
    {
        size_t payloadOffset = 0;
        if (!statusKnown && safeSize > 0)
        {
            status = safeBuffer[0];
            statusKnown = true;
            payloadOffset = 1;

            if (status == TTDB_STATUS_OK)
            {
                string pattern = string(outTdbgPath) + ".part.XXXXXX";
                vector<char> temporaryName(pattern.begin(), pattern.end());
                temporaryName.push_back('\0');
                int temporaryDescriptor = mkstemp(&temporaryName[0]);
                if (temporaryDescriptor < 0)
                {
                    outError = "could not create temporary output file";
                    return false;
                }
                temporaryPath.assign(&temporaryName[0]);
                temporaryFileCreated = true;
                if (close(temporaryDescriptor) != 0)
                {
                    outError = "could not close temporary output file";
                    return false;
                }

                outputFile.open(temporaryPath.c_str(), ios::binary | ios::trunc);
                if (!outputFile.is_open())
                {
                    outError = "could not open output file";
                    return false;
                }
            }
            else if (status != TTDB_STATUS_ERROR)
            {
                outError = "invalid response status";
                return false;
            }
        }

        if (payloadOffset == safeSize)
        {
            return true;
        }

        const char *payload =
            reinterpret_cast<const char *>(safeBuffer + payloadOffset);
        size_t payloadSize = safeSize - payloadOffset;
        if (status == TTDB_STATUS_OK)
        {
            outputFile.write(payload, static_cast<streamsize>(payloadSize));
            if (!outputFile)
            {
                outError = "could not write output file";
                return false;
            }
        }
        else
        {
            outError.append(payload, payloadSize);
        }
        return true;
    };

    while (true)
    {
        ssize_t received = recv(sock, inputBuffer, sizeof(inputBuffer), 0);
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
            outError = "failed receiving response";
            discardOutput();
            return false;
        }

        size_t safeSize = 0;
        if (!checker.consume(inputBuffer, static_cast<size_t>(received),
                             safeBuffer, sizeof(safeBuffer), safeSize))
        {
            outError = "invalid response stream";
            discardOutput();
            return false;
        }
        if (!consumeSafeBytes(safeSize))
        {
            discardOutput();
            return false;
        }
    }

    if (!checker.verify())
    {
        discardOutput();
        outError = "checksum mismatch, response corrupted";
        return false;
    }
    if (!statusKnown)
    {
        outError = "response is missing a status byte";
        return false;
    }
    if (status == TTDB_STATUS_ERROR)
    {
        if (outError.empty())
        {
            outError = "server returned an unspecified error";
        }
        return false;
    }

    outputFile.flush();
    if (!outputFile)
    {
        discardOutput();
        outError = "could not write output file";
        return false;
    }
    outputFile.close();
    if (outputFile.fail())
    {
        discardOutput();
        outError = "could not close output file";
        return false;
    }
    if (std::rename(temporaryPath.c_str(), outTdbgPath) != 0)
    {
        discardOutput();
        outError = "could not replace output file";
        return false;
    }
    temporaryFileCreated = false;
    return true;
}

bool requestDebugFile(const char *host, int port, const char *sourcePath,
                      const char *outTdbgPath, string &outError)
{
    int sock = connectToHost(host, port);
    if (sock < 0)
    {
        outError = "could not connect to load balancer";
        return false;
    }

    if (!uploadSource(sock, sourcePath))
    {
        close(sock);
        outError = "could not upload source";
        return false;
    }

    bool received = receiveResponse(sock, outTdbgPath, outError);
    close(sock);
    return received;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================

bool openTdbg(const char *path, TdbgReader &r)
{
}
void closeTdbg(TdbgReader &r)
{
    // Free r.index and close the file.
}
Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
   
}
void closeSession(DebugSession &s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
}
bool moveBy(DebugSession &s, int32_t delta)
{
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).
}
bool cmdNext(DebugSession &s) {}
bool cmdPrv(DebugSession &s) {}
bool cmdJmpNext(DebugSession &s, int32_t n) {}
bool cmdJmpPrv(DebugSession &s, int32_t n) { }

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
}
void printHelp()
{
    // Print the commands
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.
}
int main(int argc, char *argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.

}
