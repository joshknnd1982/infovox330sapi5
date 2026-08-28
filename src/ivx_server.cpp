// Infovox330Server.exe - the 32-bit helper the 64-bit SAPI 5 engine speaks through.
//
// It never opens an audio device. It produces bytes and hands them back over a pipe, which
// is what lets the 64-bit engine write them into SAPI itself, in the host application's own
// process. For NVDA that is the difference between audio that ducks correctly and audio
// that does not.
//
// One process serves every client, because the Infovox engine keeps global state and only
// tolerates one live ITTSCentral at a time. Requests are serialised inside ivx::Engine.

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "ivx_engine.hpp"
#include "ivx_log.hpp"
#include "ivx_paths.hpp"
#include "ivx_pipe.hpp"
#include "ivx_voices.hpp"

using namespace ivx;
using namespace ivx::pipe;

namespace {

std::atomic<int> g_clients{0};
std::atomic<bool> g_shutdown{false};

void send_error(HANDLE client, HRESULT hr)
{
    ErrorWire wire{static_cast<int32_t>(hr)};
    write_message(client, MsgType::RespError, &wire, sizeof(wire));
}

// Streams one utterance back to the client and watches the client's stop event.
class PipeSink : public SynthSink
{
public:
    PipeSink(HANDLE client, HANDLE abort_event) : client_(client), abort_event_(abort_event) {}

    bool on_audio(const void* data, DWORD size) override
    {
        if (should_abort()) {
            return false;
        }
        const char* p = static_cast<const char*>(data);
        DWORD remaining = size;
        while (remaining > 0) {
            const uint32_t chunk = (remaining < kMaxFramePayload) ? remaining : kMaxFramePayload;
            AudioWire wire{offset_, chunk};
            if (!write_message2(client_, MsgType::EvtAudio, &wire, sizeof(wire), p, chunk)) {
                IVX_LOG_W("client went away mid-utterance");
                broken_ = true;
                return false;
            }
            offset_ += chunk;
            p += chunk;
            remaining -= chunk;
        }
        return true;
    }

    void on_bookmark(unsigned __int64 audio_offset, DWORD id) override
    {
        MarkWire wire{audio_offset, id};
        if (!write_message(client_, MsgType::EvtBookmark, &wire, sizeof(wire))) {
            broken_ = true;
        }
    }

    void on_word_boundary(unsigned __int64 audio_offset) override
    {
        MarkWire wire{audio_offset, 0};
        if (!write_message(client_, MsgType::EvtWord, &wire, sizeof(wire))) {
            broken_ = true;
        }
    }

    bool should_abort() override
    {
        if (broken_) {
            return true;
        }
        if (abort_event_ && WaitForSingleObject(abort_event_, 0) == WAIT_OBJECT_0) {
            IVX_LOG_I("client asked to stop");
            return true;
        }
        return false;
    }

    [[nodiscard]] bool broken() const { return broken_; }

private:
    HANDLE client_;
    HANDLE abort_event_;
    uint64_t offset_ = 0;
    bool broken_ = false;
};

void handle_speak(HANDLE client, const std::vector<char>& payload)
{
    if (payload.size() < sizeof(SpeakWire)) {
        send_error(client, E_INVALIDARG);
        return;
    }
    SpeakWire request{};
    memcpy(&request, payload.data(), sizeof(request));

    const std::size_t text_bytes = static_cast<std::size_t>(request.text_chars) * sizeof(wchar_t);
    if (payload.size() < sizeof(SpeakWire) + text_bytes) {
        send_error(client, E_INVALIDARG);
        return;
    }

    SpeakParams params;
    params.mode = request.mode;
    params.speed = request.speed;
    params.pitch = request.pitch;
    params.volume = request.volume;
    params.realtime = request.realtime;
    params.text.assign(reinterpret_cast<const wchar_t*>(payload.data() + sizeof(SpeakWire)),
                       request.text_chars);

    HANDLE abort_event = nullptr;
    if (request.abort_event[0] != L'\0') {
        request.abort_event[std::size(request.abort_event) - 1] = L'\0';
        abort_event = OpenEventW(SYNCHRONIZE, FALSE, request.abort_event);
        if (!abort_event) {
            IVX_LOG_W("could not open the client's stop event '%s' (%lu)",
                      log_narrow(request.abort_event).c_str(), GetLastError());
        }
    }

    PipeSink sink(client, abort_event);
    const HRESULT hr = Engine::instance().speak(params, sink);

    if (abort_event) {
        CloseHandle(abort_event);
    }
    if (!sink.broken()) {
        EndWire end{static_cast<int32_t>(hr)};
        write_message(client, MsgType::EvtEnd, &end, sizeof(end));
    }
}

void handle_get_format(HANDLE client)
{
    WAVEFORMATEX wfx{};
    Engine::instance().output_format(wfx);
    WfxWire wire{wfx.wFormatTag,      wfx.nChannels,   wfx.nSamplesPerSec,
                 wfx.nAvgBytesPerSec, wfx.nBlockAlign, wfx.wBitsPerSample};
    write_message(client, MsgType::RespFormat, &wire, sizeof(wire));
}

void handle_get_ranges(HANDLE client, const std::vector<char>& payload)
{
    if (payload.size() < sizeof(GUID)) {
        send_error(client, E_INVALIDARG);
        return;
    }
    GUID mode{};
    memcpy(&mode, payload.data(), sizeof(mode));

    VoiceRanges ranges;
    const HRESULT hr = Engine::instance().get_ranges(mode, ranges);
    if (FAILED(hr)) {
        send_error(client, hr);
        return;
    }

    auto pack = [](const AttrRange& a) {
        return AttrWire{a.supported ? 1u : 0u, a.min_value, a.max_value, a.default_value};
    };
    RangesWire wire{pack(ranges.speed), pack(ranges.pitch), pack(ranges.volume)};
    write_message(client, MsgType::RespRanges, &wire, sizeof(wire));
}

void handle_hello(HANDLE client)
{
    const HRESULT hr = Engine::instance().ensure_initialised();
    if (FAILED(hr)) {
        send_error(client, hr);
        return;
    }
    HelloWire wire{kProtocolVersion, static_cast<uint32_t>(voice_catalogue().size())};
    write_message(client, MsgType::RespHello, &wire, sizeof(wire));
}

void serve_client(HANDLE client)
{
    ++g_clients;
    IVX_LOG_I("client connected (%d active)", g_clients.load());

    for (;;) {
        MsgHeader header{};
        std::vector<char> payload;
        if (!read_message(client, header, payload)) {
            break;
        }
        switch (static_cast<MsgType>(header.type)) {
            case MsgType::ReqHello:
                handle_hello(client);
                break;
            case MsgType::ReqGetFormat:
                handle_get_format(client);
                break;
            case MsgType::ReqGetRanges:
                handle_get_ranges(client, payload);
                break;
            case MsgType::ReqSpeak:
                handle_speak(client, payload);
                break;
            case MsgType::ReqShutdown:
                write_message(client, MsgType::RespOk, nullptr, 0);
                g_shutdown.store(true);
                goto done;
            default:
                IVX_LOG_W("unknown request type %lu", header.type);
                send_error(client, E_NOTIMPL);
                break;
        }
    }

done:
    FlushFileBuffers(client);
    DisconnectNamedPipe(client);
    CloseHandle(client);
    --g_clients;
    IVX_LOG_I("client disconnected (%d active)", g_clients.load());
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    log_init(L"server");

    HANDLE singleton = CreateMutexW(nullptr, TRUE, kServerMutexName);
    if (!singleton) {
        IVX_LOG_E("cannot create the singleton mutex (%lu)", GetLastError());
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        IVX_LOG_I("another helper is already running; exiting");
        CloseHandle(singleton);
        return 0;
    }

    // Clients wait on this before trying to connect, so they never race the first
    // CreateNamedPipe.
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, kServerReadyEventName);

    IVX_LOG_I("helper starting, data root %s", log_narrow(data_root().c_str()).c_str());
    const HRESULT init = Engine::instance().ensure_initialised();
    if (FAILED(init)) {
        // Still serve: the client deserves a real error rather than a connection failure.
        IVX_LOG_E("engine initialisation failed %s", hresult_string(init).c_str());
    }

    HANDLE connected = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD idle_since = GetTickCount();

    while (!g_shutdown.load()) {
        HANDLE instance = CreateNamedPipeW(
            kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
            64 * 1024, 64 * 1024, 0, nullptr);
        if (instance == INVALID_HANDLE_VALUE) {
            IVX_LOG_E("CreateNamedPipe failed (%lu)", GetLastError());
            Sleep(500);
            continue;
        }

        if (ready) {
            SetEvent(ready);
        }

        OVERLAPPED overlapped{};
        ResetEvent(connected);
        overlapped.hEvent = connected;

        BOOL pending = ConnectNamedPipe(instance, &overlapped);
        DWORD err = GetLastError();
        if (!pending && err == ERROR_PIPE_CONNECTED) {
            SetEvent(connected);
        } else if (!pending && err != ERROR_IO_PENDING) {
            IVX_LOG_E("ConnectNamedPipe failed (%lu)", err);
            CloseHandle(instance);
            Sleep(500);
            continue;
        }

        const DWORD wait = WaitForSingleObject(connected, 1000);
        if (wait == WAIT_TIMEOUT) {
            CancelIo(instance);
            CloseHandle(instance);
            if (g_clients.load() > 0) {
                idle_since = GetTickCount();
            } else if (GetTickCount() - idle_since > kServerIdleExitMs) {
                IVX_LOG_I("no clients for %lu ms; exiting", kServerIdleExitMs);
                break;
            }
            continue;
        }
        if (wait != WAIT_OBJECT_0) {
            CloseHandle(instance);
            continue;
        }

        idle_since = GetTickCount();
        std::thread(serve_client, instance).detach();
    }

    if (connected) {
        CloseHandle(connected);
    }
    if (ready) {
        CloseHandle(ready);
    }
    ReleaseMutex(singleton);
    CloseHandle(singleton);
    IVX_LOG_I("helper exiting");
    log_shutdown();
    return 0;
}
