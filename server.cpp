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



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
    // reads the next nonblank line
    char c;
    out = "";

    // Skip whitespace and newlines
    while (in.get(c))
    {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
        {
            break;
        }
    }

    if (!in.good())
    {
        return false;
    }

    // Put the character back and read the line
    in.putback(c);
    getline(in, out);

    // Trim trailing whitespace
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t' || out.back() == '\n' || out.back() == '\r'))
    {
        out.pop_back();
    }

    return !out.empty();
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

bool validateProgram(const char *sourcePath)
{
    // for each func defined there should be exactly one func_end and no nested funcs allowed -
    ifstream in(sourcePath);
    if (!in.is_open())
    {
        return false;
    }

    string line;
    int funcCount = 0;
    int funcEndCount = 0;
    bool inFunc = false;

    while (getline(in, line))
    {
        // Trim whitespace
        string trimmed = "";
        for (int i = 0; i < line.length(); i++)
        {
            if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
            {
                trimmed += line[i];
            }
        }

        if (trimmed.empty())
        {
            continue;
        }

        if (trimmed == "func")
        {
            if (inFunc)
            {
                // Nested func not allowed
                in.close();
                return false;
            }
            funcCount++;
            inFunc = true;
        }
        else if (trimmed == "func_end")
        {
            funcEndCount++;
            inFunc = false;
        }
    }

    in.close();

    // Each func should have exactly one func_end
    if (funcCount != funcEndCount)
    {
        return false;
    }

    // Must have a main function
    // Reset and check for main
    in.open(sourcePath);
    if (!in.is_open())
    {
        return false;
    }

    bool hasMain = false;
    while (getline(in, line))
    {
        string trimmed = "";
        for (int i = 0; i < line.length(); i++)
        {
            if (line[i] != ' ' && line[i] != '\t' && line[i] != '\r')
            {
                trimmed += line[i];
            }
        }

        if (trimmed == "func_main")
        {
            hasMain = true;
            break;
        }
    }

    in.close();
    return hasMain;
}