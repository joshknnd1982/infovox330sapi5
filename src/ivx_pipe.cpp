#include "ivx_pipe.hpp"

#include "ivx_log.hpp"

namespace ivx {
namespace pipe {
namespace {

bool write_exact(HANDLE pipe, const void* buffer, uint32_t size)
{
    const char* p = static_cast<const char*>(buffer);
    uint32_t remaining = size;
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(pipe, p, remaining, &written, nullptr) || written == 0) {
            return false;
        }
        p += written;
        remaining -= written;
    }
    return true;
}

}  // namespace

bool read_exact(HANDLE pipe, void* buffer, uint32_t size)
{
    char* p = static_cast<char*>(buffer);
    uint32_t remaining = size;
    while (remaining > 0) {
        DWORD got = 0;
        if (!ReadFile(pipe, p, remaining, &got, nullptr) || got == 0) {
            return false;
        }
        p += got;
        remaining -= got;
    }
    return true;
}

bool write_message(HANDLE pipe, MsgType type, const void* payload, uint32_t size)
{
    return write_message2(pipe, type, payload, size, nullptr, 0);
}

bool write_message2(HANDLE pipe, MsgType type, const void* head, uint32_t head_size,
                    const void* body, uint32_t body_size)
{
    MsgHeader header{static_cast<uint32_t>(type), head_size + body_size};
    if (!write_exact(pipe, &header, sizeof(header))) {
        return false;
    }
    if (head_size > 0 && !write_exact(pipe, head, head_size)) {
        return false;
    }
    if (body_size > 0 && !write_exact(pipe, body, body_size)) {
        return false;
    }
    return true;
}

bool read_message(HANDLE pipe, MsgHeader& header, std::vector<char>& payload)
{
    if (!read_exact(pipe, &header, sizeof(header))) {
        return false;
    }
    // A frame larger than this is a desynchronised stream, not a legitimate message.
    if (header.size > kMaxFramePayload + sizeof(SpeakWire) + 1024) {
        IVX_LOG_E("refusing a %lu byte frame of type %lu", header.size, header.type);
        return false;
    }
    payload.resize(header.size);
    if (header.size > 0 && !read_exact(pipe, payload.data(), header.size)) {
        return false;
    }
    return true;
}

}  // namespace pipe
}  // namespace ivx
