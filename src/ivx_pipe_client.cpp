// The 64-bit half of the bitness bridge: a SynthBackend that talks to Infovox330Server.exe.
//
// The helper is started on demand and shared by every client on the desktop. Stopping an
// utterance goes through a named event rather than the pipe, so a stop is acted on
// immediately instead of after the queued audio has drained.

#include <windows.h>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "ivx_log.hpp"
#include "ivx_paths.hpp"
#include "ivx_pipe.hpp"
#include "ivx_synth.hpp"

namespace ivx {
namespace {

using namespace ivx::pipe;

// How long to keep trying while a freshly launched helper gets its pipe up.
constexpr DWORD kConnectTimeoutMs = 20000;

class RemoteBackend : public SynthBackend
{
public:
    RemoteBackend()
    {
        fill_expected_format(format_);

        wchar_t name[96];
        _snwprintf_s(name, _TRUNCATE, L"Local\\Infovox330Abort_%lu_%p", GetCurrentProcessId(),
                     static_cast<void*>(this));
        abort_name_ = name;
        abort_event_ = CreateEventW(nullptr, TRUE, FALSE, abort_name_.c_str());
        if (!abort_event_) {
            IVX_LOG_E("could not create the stop event (%lu)", GetLastError());
        }
    }

    ~RemoteBackend() override
    {
        disconnect();
        if (abort_event_) {
            CloseHandle(abort_event_);
        }
    }

    HRESULT ensure_ready() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return ensure_connected();
    }

    void output_format(WAVEFORMATEX& out) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out = format_;
    }

    HRESULT get_ranges(const GUID& mode, VoiceRanges& out) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Idempotent, so a lost connection is worth one silent retry.
        for (int attempt = 0; attempt < 2; ++attempt) {
            const HRESULT hr = get_ranges_once(mode, out);
            if (hr != HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE) || attempt == 1) {
                return hr;
            }
            IVX_LOG_W("helper connection lost reading voice ranges; retrying");
        }
        return E_FAIL;
    }

private:
    HRESULT get_ranges_once(const GUID& mode, VoiceRanges& out)
    {
        HRESULT hr = ensure_connected();
        if (FAILED(hr)) {
            disconnect();
            return HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE);
        }
        if (!write_message(pipe_, MsgType::ReqGetRanges, &mode, sizeof(mode))) {
            drop("writing a ranges request");
            return HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE);
        }

        MsgHeader header{};
        std::vector<char> payload;
        if (!read_message(pipe_, header, payload)) {
            drop("reading a ranges reply");
            return HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE);
        }
        if (static_cast<MsgType>(header.type) == MsgType::RespError &&
            payload.size() >= sizeof(ErrorWire)) {
            ErrorWire err{};
            std::memcpy(&err, payload.data(), sizeof(err));
            return static_cast<HRESULT>(err.hr);
        }
        if (static_cast<MsgType>(header.type) != MsgType::RespRanges ||
            payload.size() < sizeof(RangesWire)) {
            return E_UNEXPECTED;
        }

        RangesWire wire{};
        std::memcpy(&wire, payload.data(), sizeof(wire));
        auto unpack = [](const AttrWire& a) {
            AttrRange r;
            r.supported = a.supported != 0;
            r.min_value = a.min_value;
            r.max_value = a.max_value;
            r.default_value = a.default_value;
            return r;
        };
        out.speed = unpack(wire.speed);
        out.pitch = unpack(wire.pitch);
        out.volume = unpack(wire.volume);
        return S_OK;
    }

public:
    HRESULT speak(const SpeakParams& params, SynthSink& sink) override
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // The helper can go away between utterances - it exits when idle, and it can be
        // killed. Losing the connection must cost at most a reconnect, never an utterance:
        // for a screen reader a dropped announcement is the failure that matters.
        for (int attempt = 0; attempt < 2; ++attempt) {
            bool emitted = false;
            HRESULT hr = S_OK;
            const Outcome outcome = speak_once(params, sink, emitted, hr);

            if (outcome == Outcome::Completed) {
                return hr;
            }
            if (emitted) {
                // Audio has already reached SAPI, so re-sending would repeat it. Report
                // success with what was delivered and leave the reason in the log.
                IVX_LOG_E("helper connection lost mid-utterance; audio was truncated");
                return S_OK;
            }
            if (attempt == 0) {
                IVX_LOG_W("helper connection lost before any audio; reconnecting and retrying");
                continue;
            }
            IVX_LOG_E("helper connection could not be re-established");
            return E_FAIL;
        }
        return E_FAIL;
    }

private:
    enum class Outcome { Completed, Broken };

    // One attempt at an utterance. Sets emitted as soon as anything has been handed to the
    // sink, because from that point a retry would duplicate audio.
    Outcome speak_once(const SpeakParams& params, SynthSink& sink, bool& emitted, HRESULT& result)
    {
        result = ensure_connected();
        if (FAILED(result)) {
            disconnect();
            return Outcome::Broken;
        }

        if (abort_event_) {
            ResetEvent(abort_event_);
        }

        SpeakWire request{};
        request.mode = params.mode;
        request.speed = params.speed;
        request.pitch = params.pitch;
        request.volume = params.volume;
        request.realtime = params.realtime;
        request.text_chars = static_cast<uint32_t>(params.text.size());
        wcsncpy_s(request.abort_event, abort_name_.c_str(), _TRUNCATE);

        if (!write_message2(pipe_, MsgType::ReqSpeak, &request, sizeof(request),
                            params.text.data(),
                            static_cast<uint32_t>(params.text.size() * sizeof(wchar_t)))) {
            drop("writing a speak request");
            return Outcome::Broken;
        }

        bool stopping = false;
        for (;;) {
            MsgHeader header{};
            std::vector<char> payload;
            if (!read_message(pipe_, header, payload)) {
                drop("reading the audio stream");
                return Outcome::Broken;
            }

            switch (static_cast<MsgType>(header.type)) {
                case MsgType::EvtAudio: {
                    if (payload.size() < sizeof(AudioWire)) {
                        result = E_UNEXPECTED;
                        return Outcome::Completed;
                    }
                    AudioWire audio{};
                    std::memcpy(&audio, payload.data(), sizeof(audio));
                    if (payload.size() < sizeof(AudioWire) + audio.bytes) {
                        result = E_UNEXPECTED;
                        return Outcome::Completed;
                    }
                    if (!stopping) {
                        emitted = true;
                        if (!sink.on_audio(payload.data() + sizeof(AudioWire), audio.bytes)) {
                            stopping = true;
                            abort_current();
                        }
                    }
                    break;
                }

                case MsgType::EvtBookmark: {
                    if (payload.size() < sizeof(MarkWire)) {
                        result = E_UNEXPECTED;
                        return Outcome::Completed;
                    }
                    MarkWire mark{};
                    std::memcpy(&mark, payload.data(), sizeof(mark));
                    if (!stopping) {
                        emitted = true;
                        sink.on_bookmark(mark.offset, mark.id);
                    }
                    break;
                }

                case MsgType::EvtWord: {
                    if (payload.size() < sizeof(MarkWire)) {
                        result = E_UNEXPECTED;
                        return Outcome::Completed;
                    }
                    MarkWire mark{};
                    std::memcpy(&mark, payload.data(), sizeof(mark));
                    if (!stopping) {
                        emitted = true;
                        sink.on_word_boundary(mark.offset);
                    }
                    break;
                }

                case MsgType::EvtEnd: {
                    EndWire end{};
                    if (payload.size() >= sizeof(end)) {
                        std::memcpy(&end, payload.data(), sizeof(end));
                    }
                    result = stopping ? S_OK : static_cast<HRESULT>(end.hr);
                    return Outcome::Completed;
                }

                case MsgType::RespError: {
                    ErrorWire err{};
                    if (payload.size() >= sizeof(err)) {
                        std::memcpy(&err, payload.data(), sizeof(err));
                    }
                    IVX_LOG_E("helper reported %s", hresult_string(err.hr).c_str());
                    result = static_cast<HRESULT>(err.hr);
                    return Outcome::Completed;
                }

                default:
                    IVX_LOG_W("unexpected frame type %lu during speak", header.type);
                    break;
            }

            if (!stopping && sink.should_abort()) {
                stopping = true;
                abort_current();
            }
        }
    }

public:
    void abort_current() override
    {
        if (abort_event_) {
            SetEvent(abort_event_);
        }
    }

private:
    void drop(const char* what)
    {
        IVX_LOG_E("helper connection lost while %s (%lu)", what, GetLastError());
        disconnect();
    }

    void disconnect()
    {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
    }

    HRESULT ensure_connected()
    {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            return S_OK;
        }

        const DWORD deadline = GetTickCount() + kConnectTimeoutMs;
        bool launched = false;

        for (;;) {
            HANDLE pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                pipe_ = pipe;
                const HRESULT hr = handshake();
                if (FAILED(hr)) {
                    disconnect();
                    return hr;
                }
                return S_OK;
            }

            const DWORD err = GetLastError();
            if (err == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(kPipeName, 2000);
            } else if (!launched) {
                const HRESULT hr = launch_helper();
                if (FAILED(hr)) {
                    return hr;
                }
                launched = true;
            } else {
                Sleep(100);
            }

            if (static_cast<LONG>(GetTickCount() - deadline) >= 0) {
                IVX_LOG_E("timed out waiting for the helper (last error %lu)", err);
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
        }
    }

    HRESULT launch_helper()
    {
        const std::wstring exe = server_exe_path();
        if (exe.empty() || !file_exists(exe)) {
            IVX_LOG_E("Infovox330Server.exe not found (looked near %s)",
                      log_narrow(own_module_directory().c_str()).c_str());
            return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }

        // Wait on the helper's ready event so the first connection attempt does not race
        // its first CreateNamedPipe.
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, kServerReadyEventName);

        std::wstring command = L"\"" + exe + L"\"";
        std::wstring directory = exe.substr(0, exe.find_last_of(L'\\'));

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};

        IVX_LOG_I("starting helper: %s", log_narrow(exe.c_str()).c_str());
        const BOOL ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                                       CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup,
                                       &process);
        if (!ok) {
            const DWORD err = GetLastError();
            IVX_LOG_E("CreateProcess failed (%lu)", err);
            if (ready) {
                CloseHandle(ready);
            }
            return HRESULT_FROM_WIN32(err);
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        if (ready) {
            WaitForSingleObject(ready, 10000);
            CloseHandle(ready);
        }
        return S_OK;
    }

    HRESULT handshake()
    {
        HelloWire hello{kProtocolVersion, 0};
        if (!write_message(pipe_, MsgType::ReqHello, &hello, sizeof(hello))) {
            return E_FAIL;
        }

        MsgHeader header{};
        std::vector<char> payload;
        if (!read_message(pipe_, header, payload)) {
            return E_FAIL;
        }
        if (static_cast<MsgType>(header.type) == MsgType::RespError &&
            payload.size() >= sizeof(ErrorWire)) {
            ErrorWire err{};
            std::memcpy(&err, payload.data(), sizeof(err));
            IVX_LOG_E("helper could not start the engine: %s", hresult_string(err.hr).c_str());
            return static_cast<HRESULT>(err.hr);
        }
        if (static_cast<MsgType>(header.type) != MsgType::RespHello ||
            payload.size() < sizeof(HelloWire)) {
            return E_UNEXPECTED;
        }
        std::memcpy(&hello, payload.data(), sizeof(hello));
        if (hello.version != kProtocolVersion) {
            IVX_LOG_E("helper speaks protocol %lu, this build speaks %lu", hello.version,
                      kProtocolVersion);
            return E_FAIL;
        }
        IVX_LOG_I("connected to the helper; it reports %lu voices", hello.voice_count);

        // Ask what the engine actually produces rather than assuming.
        if (write_message(pipe_, MsgType::ReqGetFormat, nullptr, 0) &&
            read_message(pipe_, header, payload) &&
            static_cast<MsgType>(header.type) == MsgType::RespFormat &&
            payload.size() >= sizeof(WfxWire)) {
            WfxWire wire{};
            std::memcpy(&wire, payload.data(), sizeof(wire));
            format_.wFormatTag = wire.format_tag;
            format_.nChannels = wire.channels;
            format_.nSamplesPerSec = wire.samples_per_sec;
            format_.nAvgBytesPerSec = wire.avg_bytes_per_sec;
            format_.nBlockAlign = wire.block_align;
            format_.wBitsPerSample = wire.bits_per_sample;
            format_.cbSize = 0;
        }
        return S_OK;
    }

    std::mutex mutex_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE abort_event_ = nullptr;
    std::wstring abort_name_;
    WAVEFORMATEX format_{};
};

}  // namespace

SynthBackend* create_backend()
{
    return new RemoteBackend();
}

}  // namespace ivx
