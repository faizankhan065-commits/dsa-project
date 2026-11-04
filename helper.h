#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

const std::int32_t MAX_VARS_PER_FRAME = 16;
const std::int32_t MAX_STACK_DEPTH = 64;
const std::int32_t IO_BUFFER_SIZE = 64 * 1024;
const std::int32_t TTDB_VERSION = 1;
const std::uint8_t TTDB_STATUS_OK = 0;
const std::uint8_t TTDB_STATUS_ERROR = 1;
const std::size_t TTDB_CHECKSUM_TRAILER_SIZE = 4;

struct Variable
{
    std::string name;
    std::int32_t value;
};

struct Frame
{
    std::string func_name;
    std::int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    std::string callerArgNames[MAX_VARS_PER_FRAME];
    bool callerArgIsVariable[MAX_VARS_PER_FRAME];
    std::int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    std::int32_t localCount;
};

struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    std::int32_t stackDepth;
};

struct TTDBHeader
{
    char magic[4];
    std::int32_t version;
    std::int32_t stepCount;
    std::int64_t indexOffset;
};

struct ClientInfo
{
    int sock;
    std::string address;
    int port;
    std::time_t arrivedAt;
};

// CRC-32/ISO-HDLC; callers feed the status byte before the payload.
class Checksum32
{
    std::uint32_t state;

public:
    Checksum32() : state(0xFFFFFFFFu) {}

    void update(const void *data, std::size_t length)
    {
        const unsigned char *bytes = static_cast<const unsigned char *>(data);
        for (std::size_t i = 0; i < length; ++i)
        {
            state ^= bytes[i];
            for (int bit = 0; bit < 8; ++bit)
            {
                state = (state >> 1) ^ ((state & 1u) ? 0xEDB88320u : 0u);
            }
        }
    }

    std::uint32_t value() const
    {
        return state ^ 0xFFFFFFFFu;
    }
};

inline void writeChecksumLittleEndian(
    std::uint32_t checksum,
    std::uint8_t output[TTDB_CHECKSUM_TRAILER_SIZE])
{
    for (std::size_t i = 0; i < TTDB_CHECKSUM_TRAILER_SIZE; ++i)
    {
        output[i] = static_cast<std::uint8_t>((checksum >> (i * 8)) & 0xFFu);
    }
}

inline std::uint32_t readChecksumLittleEndian(
    const std::uint8_t input[TTDB_CHECKSUM_TRAILER_SIZE])
{
    std::uint32_t checksum = 0;
    for (std::size_t i = 0; i < TTDB_CHECKSUM_TRAILER_SIZE; ++i)
    {
        checksum |= static_cast<std::uint32_t>(input[i]) << (i * 8);
    }
    return checksum;
}

struct FuncEntry
{
    std::string funcName;
    std::int64_t byteOffsetInResolveBin;
};

struct PendingPatch
{
    std::int64_t byteOffsetOfOffsetField;
    std::string targetFuncName;
};
