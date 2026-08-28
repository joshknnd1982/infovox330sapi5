// Wire protocol between the 64-bit SAPI 5 engine and the 32-bit helper process.
//
// SAPI's ISpTTSEngine and ISpTTSEngineSite are in-process interfaces with no proxy/stub, so
// a 64-bit engine cannot be a LocalServer32 and let COM bridge the bitness gap: the site
// would have to marshal back into SAPI. The 64-bit DLL is therefore a real in-process COM
// object that does its own IPC, and this is that IPC.
//
// One request produces one reply, except for Speak, which produces a stream of Audio /
// Bookmark / Word frames terminated by an End frame. Stopping mid-utterance does not travel
// over the pipe at all - it is a named event the client sets and the helper polls, so a stop
// is immediate even while the pipe is full of queued audio.

#pragma once

#include <windows.h>
#include <objbase.h>

#include <cstdint>
#include <vector>

namespace ivx {
namespace pipe {

inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\Infovox330TTS";
inline constexpr wchar_t kServerMutexName[] = L"Local\\Infovox330ServerSingleton";
inline constexpr wchar_t kServerReadyEventName[] = L"Local\\Infovox330ServerReady";
inline constexpr uint32_t kProtocolVersion = 2;

// Largest single audio frame; the helper splits anything bigger.
inline constexpr uint32_t kMaxFramePayload = 256 * 1024;

// The helper exits once it has had no clients for this long.
inline constexpr DWORD kServerIdleExitMs = 60000;

enum class MsgType : uint32_t {
    ReqHello = 1,
    ReqGetFormat = 2,
    ReqGetRanges = 3,
    ReqSpeak = 4,
    ReqShutdown = 5,

    RespOk = 100,
    RespError = 101,
    RespHello = 102,
    RespFormat = 103,
    RespRanges = 104,

    EvtAudio = 200,
    EvtBookmark = 201,
    EvtWord = 202,
    EvtEnd = 203,
};

#pragma pack(push, 1)

struct MsgHeader {
    uint32_t type;
    uint32_t size;  // payload bytes following this header
};

struct HelloWire {
    uint32_t version;
    uint32_t voice_count;
};

struct ErrorWire {
    int32_t hr;
};

struct WfxWire {
    uint16_t format_tag;
    uint16_t channels;
    uint32_t samples_per_sec;
    uint32_t avg_bytes_per_sec;
    uint16_t block_align;
    uint16_t bits_per_sample;
};

struct AttrWire {
    uint32_t supported;
    uint32_t min_value;
    uint32_t max_value;
    uint32_t default_value;
};

struct RangesWire {
    AttrWire speed;
    AttrWire pitch;
    AttrWire volume;
};

struct SpeakWire {
    GUID mode;
    int32_t speed;
    int32_t pitch;
    int32_t volume;
    int32_t realtime;         // SAPI 4's fourth attribute; -1 leaves it alone
    uint32_t text_chars;      // UTF-16 units following the struct, no terminator
    wchar_t abort_event[96];  // name of a manual-reset event; empty means no stop channel
};

struct AudioWire {
    uint64_t offset;  // bytes into this utterance
    uint32_t bytes;   // audio bytes following the struct
};

struct MarkWire {
    uint64_t offset;
    uint32_t id;  // meaningless for EvtWord
};

struct EndWire {
    int32_t hr;
};

#pragma pack(pop)

// Blocking framed I/O. All return false on any short read/write or pipe error; there is no
// partial-message recovery, because a broken frame means a broken connection.
bool write_message(HANDLE pipe, MsgType type, const void* payload, uint32_t size);
bool write_message2(HANDLE pipe, MsgType type, const void* head, uint32_t head_size,
                    const void* body, uint32_t body_size);
bool read_message(HANDLE pipe, MsgHeader& header, std::vector<char>& payload);
bool read_exact(HANDLE pipe, void* buffer, uint32_t size);

}  // namespace pipe
}  // namespace ivx
