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
#include <limits>
#include <new>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
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
    int64_t fileSize;
    TTDBHeader header;
    vector<int64_t> index;

    TdbgReader() : file_index(-1), fileSize(0), header(), index() {}
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

bool readExactAt(TdbgReader &r, int64_t offset, int64_t limit,
                 void *buffer, size_t length)
{
    if (r.file_index < 0 || offset < 0 || limit < offset ||
        limit > r.fileSize ||
        static_cast<uint64_t>(length) > static_cast<uint64_t>(limit - offset))
    {
        return false;
    }

    size_t totalRead = 0;
    char *bytes = static_cast<char *>(buffer);
    while (totalRead < length)
    {
        ssize_t result = pread(static_cast<int>(r.file_index), bytes + totalRead,
                               length - totalRead,
                               static_cast<off_t>(offset + totalRead));
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
        totalRead += static_cast<size_t>(result);
    }
    return true;
}

uint32_t decodeUInt32LE(const uint8_t bytes[4])
{
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t decodeUInt64LE(const uint8_t bytes[8])
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
    {
        value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
    }
    return value;
}

bool readUInt32LE(TdbgReader &r, int64_t &position, int64_t limit,
                  uint32_t &value)
{
    uint8_t bytes[4];
    if (!readExactAt(r, position, limit, bytes, sizeof(bytes)))
    {
        return false;
    }
    position += sizeof(bytes);
    value = decodeUInt32LE(bytes);
    return true;
}

bool readInt32LE(TdbgReader &r, int64_t &position, int64_t limit,
                 int32_t &value)
{
    uint32_t bits = 0;
    if (!readUInt32LE(r, position, limit, bits))
    {
        return false;
    }
    if (bits <= static_cast<uint32_t>(numeric_limits<int32_t>::max()))
    {
        value = static_cast<int32_t>(bits);
    }
    else
    {
        value = -1 - static_cast<int32_t>(UINT32_MAX - bits);
    }
    return true;
}

bool readSerializedString(TdbgReader &r, int64_t &position, int64_t limit,
                          string &value)
{
    uint32_t length = 0;
    if (!readUInt32LE(r, position, limit, length) ||
        static_cast<uint64_t>(length) >
            static_cast<uint64_t>(limit - position))
    {
        return false;
    }

    string decoded;
    if (static_cast<size_t>(length) > decoded.max_size())
    {
        return false;
    }
    try
    {
        decoded.resize(length);
    }
    catch (const bad_alloc &)
    {
        return false;
    }
    if (length > 0 &&
        !readExactAt(r, position, limit, &decoded[0], length))
    {
        return false;
    }
    position += length;
    value.swap(decoded);
    return true;
}

Snapshot *readSnapshotAtInternal(TdbgReader &r, int32_t step)
{
    if (r.file_index < 0 || step < 0 || step >= r.header.stepCount ||
        r.index.size() != static_cast<size_t>(r.header.stepCount))
    {
        return NULL;
    }

    int64_t position = r.index[step];
    int64_t limit = step + 1 < r.header.stepCount
        ? r.index[step + 1]
        : r.header.indexOffset;
    if (position < 20 || limit <= position ||
        limit > r.header.indexOffset)
    {
        return NULL;
    }

    Snapshot *snapshot = new (nothrow) Snapshot();
    if (snapshot == NULL)
    {
        return NULL;
    }

    try
    {
        if (!readInt32LE(r, position, limit, snapshot->stackDepth) ||
            snapshot->stackDepth < 0 ||
            snapshot->stackDepth > MAX_STACK_DEPTH)
        {
            delete snapshot;
            return NULL;
        }

        for (int32_t i = 0; i < snapshot->stackDepth; ++i)
        {
            Frame &frame = snapshot->callStack[i];
            if (!readSerializedString(r, position, limit, frame.func_name) ||
                !readInt32LE(r, position, limit, frame.argc) ||
                frame.argc < 0 || frame.argc > MAX_VARS_PER_FRAME)
            {
                delete snapshot;
                return NULL;
            }
            for (int32_t j = 0; j < frame.argc; ++j)
            {
                if (!readSerializedString(r, position, limit, frame.argv[j].name) ||
                    !readInt32LE(r, position, limit, frame.argv[j].value))
                {
                    delete snapshot;
                    return NULL;
                }
            }

            if (!readInt32LE(r, position, limit, frame.returnLine) ||
                !readInt32LE(r, position, limit, frame.localCount) ||
                frame.localCount < 0 ||
                frame.localCount > MAX_VARS_PER_FRAME)
            {
                delete snapshot;
                return NULL;
            }
            for (int32_t j = 0; j < frame.localCount; ++j)
            {
                if (!readSerializedString(r, position, limit, frame.locals[j].name) ||
                    !readInt32LE(r, position, limit, frame.locals[j].value))
                {
                    delete snapshot;
                    return NULL;
                }
            }
        }
    }
    catch (const bad_alloc &)
    {
        delete snapshot;
        return NULL;
    }

    if (position != limit)
    {
        delete snapshot;
        return NULL;
    }
    return snapshot;
}

void closeTdbg(TdbgReader &r)
{
    if (r.file_index >= 0)
    {
        if (close(static_cast<int>(r.file_index)) != 0)
        {
            perror("close .tdbg file");
        }
    }
    r.file_index = -1;
    r.fileSize = 0;
    r.header = TTDBHeader();
    r.index.clear();
}

bool openTdbg(const char *path, TdbgReader &r)
{
    if (path == NULL || path[0] == '\0')
    {
        return false;
    }

    TdbgReader candidate;
    candidate.file_index = open(path, O_RDONLY | O_CLOEXEC);
    if (candidate.file_index < 0)
    {
        return false;
    }
    const auto failCandidate = [&candidate]() -> bool
    {
        closeTdbg(candidate);
        return false;
    };

    struct stat fileInfo;
    if (fstat(static_cast<int>(candidate.file_index), &fileInfo) != 0 ||
        fileInfo.st_size < 20 ||
        static_cast<uint64_t>(fileInfo.st_size) >
            static_cast<uint64_t>(numeric_limits<int64_t>::max()))
    {
        return failCandidate();
    }
    candidate.fileSize = static_cast<int64_t>(fileInfo.st_size);

    uint8_t headerBytes[20];
    if (!readExactAt(candidate, 0, candidate.fileSize,
                     headerBytes, sizeof(headerBytes)) ||
        memcmp(headerBytes, "TTDB", 4) != 0)
    {
        return failCandidate();
    }

    uint32_t version = decodeUInt32LE(headerBytes + 4);
    uint32_t stepCount = decodeUInt32LE(headerBytes + 8);
    uint64_t indexOffsetBits = decodeUInt64LE(headerBytes + 12);
    if (version != static_cast<uint32_t>(TTDB_VERSION) ||
        stepCount == 0 ||
        stepCount > static_cast<uint32_t>(numeric_limits<int32_t>::max()) ||
        indexOffsetBits > static_cast<uint64_t>(numeric_limits<int64_t>::max()))
    {
        return failCandidate();
    }

    candidate.header.magic[0] = 'T';
    candidate.header.magic[1] = 'T';
    candidate.header.magic[2] = 'D';
    candidate.header.magic[3] = 'B';
    candidate.header.version = static_cast<int32_t>(version);
    candidate.header.stepCount = static_cast<int32_t>(stepCount);
    candidate.header.indexOffset = static_cast<int64_t>(indexOffsetBits);
    uint64_t indexBytes = static_cast<uint64_t>(stepCount) * 8;
    if (candidate.header.indexOffset < 20 ||
        candidate.header.indexOffset > candidate.fileSize ||
        indexBytes != static_cast<uint64_t>(
            candidate.fileSize - candidate.header.indexOffset) ||
        static_cast<size_t>(stepCount) > candidate.index.max_size())
    {
        return failCandidate();
    }

    try
    {
        candidate.index.resize(stepCount);
    }
    catch (const bad_alloc &)
    {
        return failCandidate();
    }

    for (int32_t i = 0; i < candidate.header.stepCount; ++i)
    {
        uint8_t offsetBytes[8];
        int64_t indexPosition =
            candidate.header.indexOffset + static_cast<int64_t>(i) * 8;
        if (!readExactAt(candidate, indexPosition, candidate.fileSize,
                         offsetBytes, sizeof(offsetBytes)))
        {
            return failCandidate();
        }
        uint64_t offset = decodeUInt64LE(offsetBytes);
        if (offset > static_cast<uint64_t>(numeric_limits<int64_t>::max()))
        {
            return failCandidate();
        }
        candidate.index[i] = static_cast<int64_t>(offset);
        if ((i == 0 && candidate.index[i] != 20) ||
            candidate.index[i] < 20 ||
            candidate.index[i] >= candidate.header.indexOffset ||
            (i > 0 && candidate.index[i] <= candidate.index[i - 1]))
        {
            return failCandidate();
        }
    }

    for (int32_t i = 0; i < candidate.header.stepCount; ++i)
    {
        Snapshot *snapshot = readSnapshotAtInternal(candidate, i);
        if (snapshot == NULL)
        {
            return failCandidate();
        }
        delete snapshot;
    }

    closeTdbg(r);
    r.file_index = candidate.file_index;
    r.fileSize = candidate.fileSize;
    r.header = candidate.header;
    r.index.swap(candidate.index);
    candidate.file_index = -1;
    candidate.fileSize = 0;
    return true;
}

Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
    return readSnapshotAtInternal(r, step);
}

// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
    closeTdbg(s.reader);
    s.active_flag = 0;
    s.currentStep = 0;
}
void closeSession(DebugSession &s)
{
    closeTdbg(s.reader);
    s.active_flag = 0;
    s.currentStep = 0;
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    if (!s.active_flag || s.currentStep < 0 ||
        s.currentStep >= s.reader.header.stepCount)
    {
        cerr << "no active debug session" << endl;
        return false;
    }

    Snapshot *snapshot = readSnapshotAt(
        s.reader, static_cast<int32_t>(s.currentStep));
    if (snapshot == NULL)
    {
        cerr << "damaged .tdbg file" << endl;
        return false;
    }
    printSnapshot(static_cast<int32_t>(s.currentStep + 1),
                  s.reader.header.stepCount, *snapshot);
    delete snapshot;
    return true;
}
bool moveBy(DebugSession &s, int32_t delta)
{
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    const string candidatePath =
        string(TDBG_FILE) + ".incoming." + to_string(getpid());
    string error;
    if (!requestDebugFile(LB_HOST.c_str(), LB_PORT, sourcePath.c_str(),
                          candidatePath.c_str(), error))
    {
        cerr << error << endl;
        std::remove(candidatePath.c_str());
        return false;
    }

    TdbgReader newReader;
    if (!openTdbg(candidatePath.c_str(), newReader))
    {
        cerr << "server sent a damaged .tdbg file" << endl;
        std::remove(candidatePath.c_str());
        return false;
    }
    if (std::rename(candidatePath.c_str(), TDBG_FILE) != 0)
    {
        cerr << "could not replace the local .tdbg file" << endl;
        closeTdbg(newReader);
        std::remove(candidatePath.c_str());
        return false;
    }

    closeTdbg(s.reader);
    s.reader.file_index = newReader.file_index;
    s.reader.fileSize = newReader.fileSize;
    s.reader.header = newReader.header;
    s.reader.index.swap(newReader.index);
    newReader.file_index = -1;
    newReader.fileSize = 0;
    s.currentStep = 0;
    s.active_flag = 1;
    return showCurrent(s);
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
