// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <cstring>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    { // initialize the stack
        top = NULL;
        count = 0;
    }
    void push(const T &val)
    {
        // pushes the value on the stack if max limit is not reached yet.
        if (count >= MAX_STACK_DEPTH)
        {
            return;
        }
        Node *newNode = new Node();
        newNode->data = val;
        newNode->next = top;
        top = newNode;
        count++;
    }
    T pop()
    {
        // pop the top value on the stack
        if (isEmpty())
        {
            T empty;
            return empty;
        }
        Node *temp = top;
        T val = top->data;
        top = top->next;
        delete temp;
        count--;
        return val;
    }
    T &peek()
    {
        // returns the top value on the stack
        if (isEmpty())
        {
            static T empty;
            return empty;
        }
        return top->data;
    }
    bool isEmpty()
    {
        return top == NULL;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
        if (isEmpty() || maxLen <= 0)
        {
            return 0;
        }
        Node *current = top;
        int32_t written = 0;
        int32_t index = 0;

        // First count how many we can write
        Node *temp = top;
        while (temp != NULL && index < maxLen)
        {
            written++;
            temp = temp->next;
            index++;
        }

        // Copy top to bottom
        current = top;
        index = 0;
        while (current != NULL && index < maxLen)
        {
            out[index] = current->data;
            current = current->next;
            index++;
        }

        return written;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = NULL;
        tail = NULL;
        stepCount = 0;
    }
    void record(Snapshot *s)
    {
        // add record in the timeline
        TimelineNode *newNode = new TimelineNode();
        newNode->data = s;
        newNode->next = NULL;
        newNode->prev = tail;

        if (tail != NULL)
        {
            tail->next = newNode;
        }
        tail = newNode;

        if (head == NULL)
        {
            head = newNode;
        }
        stepCount++;
    }
    TimelineNode *begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

// Core structs
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



// STAGE 0: RECEIVE TRACE AND SAVE IT AS source.bin
bool receiveTrace(istream &client)
{
    ofstream source("source.bin", ios::binary | ios::trunc);
    if (!source.is_open())
    {
        return false;
    }

    char buffer[IO_BUFFER_SIZE];
    uint64_t totalBytes = 0;

    while (client)
    {
        client.read(buffer, sizeof(buffer));
        streamsize bytesRead = client.gcount();

        if (bytesRead > 0)
        {
            uint64_t chunkSize = static_cast<uint64_t>(bytesRead);
            if (totalBytes + chunkSize > MAX_SOURCE_BYTES)
            {
                source.close();
                std::remove("source.bin");
                return false;
            }

            source.write(buffer, bytesRead);
            if (!source)
            {
                source.close();
                std::remove("source.bin");
                return false;
            }

            totalBytes += chunkSize;
        }
    }

    if (client.bad() || (client.fail() && !client.eof()))
    {
        source.close();
        std::remove("source.bin");
        return false;
    }

    source.close();
    if (!source)
    {
        std::remove("source.bin");
        return false;
    }

    return true;
}

// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
    while (getline(in, out))
    {
        if (!out.empty() && out.back() == '\r')
        {
            out.pop_back();
        }

        bool hasText = false;
        for (size_t i = 0; i < out.length(); i++)
        {
            if (out[i] != ' ' && out[i] != '\t')
            {
                hasText = true;
                break;
            }
        }

        if (!hasText)
        {
            continue;
        }

        while (!out.empty() && (out.back() == ' ' || out.back() == '\t'))
        {
            out.pop_back();
        }
        return true;
    }

    return false;
}

bool writeResolveRecord(ofstream &out, int64_t offset, const string &line)
{
    if (offset < 0 || line.length() > UINT32_MAX)
    {
        return false;
    }

    uint32_t lineSize = static_cast<uint32_t>(line.length());
    out.write(reinterpret_cast<const char *>(&offset), sizeof(offset));
    out.write(reinterpret_cast<const char *>(&lineSize), sizeof(lineSize));
    out.write(line.data(), static_cast<streamsize>(lineSize));
    return static_cast<bool>(out);
}
string firstWord(const string &line)
{
    // returns first word from the input string
    string result = "";
    bool inWord = false;

    for (int i = 0; i < line.length(); i++)
    {
        char c = line[i];
        if (c != ' ' && c != '\t')
        {
            result += c;
            inWord = true;
        }
        else if (inWord)
        {
            break;
        }
    }

    return result;
}
string secondWord(const string &line)
{
    // returns the second word
    string result = "";
    int wordCount = 0;
    bool inWord = false;

    for (int i = 0; i < line.length(); i++)
    {
        char c = line[i];
        if (c != ' ' && c != '\t')
        {
            if (inWord)
            {
                result += c;
            }
            else
            {
                inWord = true;
                wordCount++;
                if (wordCount == 2)
                {
                    result = "";
                    result += c;
                }
            }
        }
        else
        {
            inWord = false;
        }
    }

    return result;
}

int64_t findFunctionOffset(FuncEntry functions[], int32_t functionCount, const string &name)
{
    for (int32_t i = 0; i < functionCount; i++)
    {
        if (functions[i].funcName == name)
        {
            return functions[i].byteOffsetInResolveBin;
        }
    }

    return -1;
}

bool patchCallTargets(ofstream &resolved, FuncEntry functions[], int32_t functionCount,
                      PendingPatch patches[], int32_t patchCount)
{
    for (int32_t i = 0; i < patchCount; i++)
    {
        int64_t targetOffset = findFunctionOffset(functions, functionCount, patches[i].targetFuncName);
        if (targetOffset < 0)
        {
            return false;
        }

        resolved.seekp(patches[i].byteOffsetOfOffsetField, ios::beg);
        resolved.write(reinterpret_cast<const char *>(&targetOffset), sizeof(targetOffset));
        if (!resolved)
        {
            return false;
        }
    }

    return true;
}

// PASS 0x1: WRITE resolve.bin AND PATCH CALL TARGET OFFSETS
int64_t resolve(const char *sourcePath, const char *resolvePath)
{
    if (sourcePath == NULL || resolvePath == NULL)
    {
        return -1;
    }

    ifstream source(sourcePath, ios::binary);
    if (!source.is_open())
    {
        return -1;
    }

    ofstream resolved(resolvePath, ios::binary | ios::trunc);
    if (!resolved.is_open())
    {
        return -1;
    }

    FuncEntry functions[MAX_FUNCS];
    PendingPatch patches[MAX_PATCHES];
    int32_t functionCount = 0;
    int32_t patchCount = 0;
    int64_t mainOffset = -1;
    int64_t nextOffset = 0;
    string line;

    while (readSourceLine(source, line))
    {
        if (line.length() > UINT32_MAX || nextOffset < 0 ||
            nextOffset > INT64_MAX - 12 - static_cast<int64_t>(line.length()))
        {
            resolved.close();
            std::remove(resolvePath);
            return -1;
        }

        int64_t recordOffset = nextOffset;
        uint32_t lineSize = static_cast<uint32_t>(line.length());
        string instruction = firstWord(line);
        string name = secondWord(line);

        if (instruction == "func")
        {
            if (name.empty() || functionCount >= MAX_FUNCS)
            {
                resolved.close();
                std::remove(resolvePath);
                return -1;
            }

            for (int32_t i = 0; i < functionCount; i++)
            {
                if (functions[i].funcName == name)
                {
                    resolved.close();
                    std::remove(resolvePath);
                    return -1;
                }
            }

            functions[functionCount].funcName = name;
            functions[functionCount].byteOffsetInResolveBin = recordOffset;
            if (name == "main")
            {
                mainOffset = recordOffset;
            }
            functionCount++;
        }
        else if (instruction == "call")
        {
            if (name.empty() || patchCount >= MAX_PATCHES)
            {
                resolved.close();
                std::remove(resolvePath);
                return -1;
            }

            patches[patchCount].byteOffsetOfOffsetField = recordOffset;
            patches[patchCount].targetFuncName = name;
            patchCount++;
        }

        if (!writeResolveRecord(resolved, recordOffset, line))
        {
            resolved.close();
            std::remove(resolvePath);
            return -1;
        }

        nextOffset += 12 + static_cast<int64_t>(lineSize);
    }

    if (source.bad())
    {
        resolved.close();
        std::remove(resolvePath);
        return -1;
    }

    if (mainOffset < 0)
    {
        resolved.close();
        std::remove(resolvePath);
        return -1;
    }

    if (!patchCallTargets(resolved, functions, functionCount, patches, patchCount))
    {
        resolved.close();
        std::remove(resolvePath);
        return -1;
    }

    resolved.close();
    if (!resolved)
    {
        std::remove(resolvePath);
        return -1;
    }

    return mainOffset;
}

enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};

struct Token
{
    TokenType type;
    string text;
};

bool validateProgram(const char *sourcePath)
{
    ifstream in(sourcePath);
    if (!in.is_open())
    {
        return false;
    }

    string line;
    bool inFunc = false;

    while (getline(in, line))
    {
        while (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }

        string instruction = firstWord(line);
        if (instruction == "func")
        {
            if (inFunc)
            {
                return false;
            }

            if (secondWord(line).empty())
            {
                return false;
            }

            inFunc = true;
        }
        else if (instruction == "func_end")
        {
            if (!inFunc || !secondWord(line).empty())
            {
                return false;
            }

            inFunc = false;
        }
    }

    return !in.bad() && !inFunc;
}