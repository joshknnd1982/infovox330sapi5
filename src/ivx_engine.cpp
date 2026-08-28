#include "ivx_engine.hpp"

#include "ivx_synth.hpp"

#include "ivx_log.hpp"
#include "ivx_paths.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

using namespace ivx::sapi4;

namespace ivx {
namespace {

// The engine is told there are always two seconds of room. Nothing here ever plays audio,
// so the buffer drains the instant it is written and the engine never has to wait.
constexpr DWORD kBufferSeconds = 2;

// Give up on an utterance that produces nothing at all for this long.
constexpr DWORD kStallTimeoutMs = 20000;

// Absolute ceiling on one utterance, however much audio it is producing.
constexpr DWORD kUtteranceTimeoutMs = 600000;

// After asking the engine to stop, wait this long for it to settle before moving on.
constexpr DWORD kAbortSettleMs = 3000;

[[nodiscard]] std::wstring bounded_wstring(const WCHAR* s, std::size_t capacity)
{
    if (!s) {
        return {};
    }
    std::size_t len = 0;
    while (len < capacity && s[len] != L'\0') {
        ++len;
    }
    return std::wstring(s, len);
}

#pragma pack(push, 1)
// SAPI 4 hands over exactly 18 bytes and expects exactly 18 back, whatever the compiler
// thinks sizeof(WAVEFORMATEX) is.
struct WfxRaw {
    WORD  wFormatTag;
    WORD  nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD  nBlockAlign;
    WORD  wBitsPerSample;
    WORD  cbSize;
};
#pragma pack(pop)
static_assert(sizeof(WfxRaw) == 18, "SAPI 4 wave format must be 18 bytes");

void wfx_from_raw(const WfxRaw& raw, WAVEFORMATEX& out)
{
    out.wFormatTag = raw.wFormatTag;
    out.nChannels = raw.nChannels;
    out.nSamplesPerSec = raw.nSamplesPerSec;
    out.nAvgBytesPerSec = raw.nAvgBytesPerSec;
    out.nBlockAlign = raw.nBlockAlign;
    out.wBitsPerSample = raw.wBitsPerSample;
    out.cbSize = 0;
}

void raw_from_wfx(const WAVEFORMATEX& wfx, WfxRaw& out)
{
    out.wFormatTag = wfx.wFormatTag;
    out.nChannels = wfx.nChannels;
    out.nSamplesPerSec = wfx.nSamplesPerSec;
    out.nAvgBytesPerSec = wfx.nAvgBytesPerSec;
    out.nBlockAlign = wfx.nBlockAlign;
    out.wBitsPerSample = wfx.wBitsPerSample;
    out.cbSize = 0;
}

// A thread that owns every SAPI 4 COM object and runs the message pump the engine needs.
// Tasks posted from other threads run between message dispatches, never inside a window
// procedure, which is what keeps the engine out of RPC_E_CANTCALLOUT_INEXTERNALCALL.
class EngineThread {
public:
    EngineThread()
    {
        ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        thread_ = std::thread([this] { run(); });
        WaitForSingleObject(ready_, INFINITE);
    }

    ~EngineThread()
    {
        if (thread_.joinable()) {
            PostThreadMessageW(tid_, WM_QUIT, 0, 0);
            thread_.join();
        }
        if (ready_) {
            CloseHandle(ready_);
        }
    }

    EngineThread(const EngineThread&) = delete;
    EngineThread& operator=(const EngineThread&) = delete;

    [[nodiscard]] bool on_thread() const { return GetCurrentThreadId() == tid_; }

    // Queues a task and returns immediately. Used by the audio sink so that notifications
    // back into the engine never happen inside one of the engine's own calls.
    void post(std::function<void()> fn)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push_back(std::move(fn));
        }
        PostThreadMessageW(tid_, WM_NULL, 0, 0);
    }

    HRESULT invoke(std::function<HRESULT()> fn)
    {
        if (on_thread()) {
            return fn();
        }

        HRESULT result = E_FAIL;
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!done) {
            return E_OUTOFMEMORY;
        }
        post([&result, &fn, done] {
            result = fn();
            SetEvent(done);
        });
        WaitForSingleObject(done, INFINITE);
        CloseHandle(done);
        return result;
    }

    void invoke_void(std::function<void()> fn)
    {
        invoke([&fn]() -> HRESULT {
            fn();
            return S_OK;
        });
    }

private:
    void drain()
    {
        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

    void run()
    {
        tid_ = GetCurrentThreadId();

        MSG msg;
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // force the queue into existence
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IVX_LOG_I("engine thread %lu started, CoInitializeEx=%s", tid_,
                  hresult_string(hr).c_str());
        SetEvent(ready_);

        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            drain();
        }
        drain();

        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
        IVX_LOG_I("engine thread %lu exiting", tid_);
    }

    std::thread thread_;
    DWORD tid_ = 0;
    HANDLE ready_ = nullptr;
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
};

// Receives audio and bookmarks; forwarded to whichever Engine::Impl owns it.
class StreamTarget {
public:
    virtual ~StreamTarget() = default;
    virtual void audio_written(const void* data, DWORD size, unsigned __int64 total_after) = 0;
    // The engine marks every word it emits. begin_mark/end_mark bracket one such mark; if
    // text_bookmark fires in between, the mark belonged to a "\mrk=N\" tag in the text
    // rather than to a plain word boundary.
    virtual void begin_mark(unsigned __int64 total_at) = 0;
    virtual void text_bookmark(DWORD mark_number, unsigned __int64 absolute_offset) = 0;
    virtual void end_mark() = 0;
    virtual void engine_audio_stopped() = 0;
    virtual void engine_text_done() = 0;
    virtual void engine_flushed() = 0;
    // Claim/UnClaim bracket a run of audio. Some engines unclaim between sentences, so an
    // unclaim only ends the utterance if nothing re-claims shortly afterwards.
    virtual void engine_claimed() = 0;
    virtual void engine_unclaimed() = 0;
};

// IAudio + IAudioDest. Instead of a sound card this drains straight into StreamTarget, so
// every write is "played" the moment it arrives and the engine is never throttled.
//
// Release never destroys the object: SAPI 4 engines are notorious for over-releasing the
// sinks handed to them, and a use-after-free inside a screen reader is the worst possible
// failure. Engine::Impl keeps the only owning pointer and deliberately leaks it at
// shutdown rather than risk the engine touching freed memory.
class AudioSink : public IAudio, public IAudioDest {
public:
    AudioSink(EngineThread* thread, StreamTarget* target) : thread_(thread), target_(target) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(IAudio))) {
            *ppv = static_cast<IAudio*>(this);
        } else if (IsEqualIID(riid, __uuidof(IAudioDest))) {
            *ppv = static_cast<IAudioDest*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(refs_.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        const long n = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n <= 0) {
            // Intentionally not deleted; see the class comment.
            refs_.store(1, std::memory_order_relaxed);
            IVX_LOG_D("AudioSink released past zero; keeping the object alive");
            return 1;
        }
        return static_cast<ULONG>(n);
    }

    // ---- IAudio ----

    STDMETHODIMP Flush() override
    {
        IVX_LOG_D("IAudio::Flush");
        if (!have_format_) {
            return AUDERR_NEEDWAVEFORMAT;
        }
        target_->engine_flushed();
        return S_OK;
    }

    STDMETHODIMP LevelGet(DWORD* pdwLevel) override
    {
        if (!pdwLevel) {
            return E_POINTER;
        }
        if (!have_format_) {
            return AUDERR_NEEDWAVEFORMAT;
        }
        *pdwLevel = level_;
        return S_OK;
    }

    STDMETHODIMP LevelSet(DWORD dwLevel) override
    {
        if (!have_format_) {
            return AUDERR_NEEDWAVEFORMAT;
        }
        level_ = dwLevel;
        return S_OK;
    }

    STDMETHODIMP PassNotify(void* pNotifyInterface, GUID iidNotify) override
    {
        if (!IsEqualGUID(iidNotify, __uuidof(IAudioDestNotifySink))) {
            IVX_LOG_W("IAudio::PassNotify with unexpected iid %s",
                      guid_string(iidNotify).c_str());
            return AUDERR_INVALIDNOTIFYSINK;
        }
        // Deliberately not AddRef'd: the engine owns this sink for as long as it owns us,
        // which is what the NVDA driver relies on and what this engine expects.
        notify_ = static_cast<IAudioDestNotifySink*>(pNotifyInterface);
        IVX_LOG_D("IAudio::PassNotify sink=%p", static_cast<void*>(notify_));
        return S_OK;
    }

    STDMETHODIMP PosnGet(QWORD* pqTimeStamp) override
    {
        if (!pqTimeStamp) {
            return E_POINTER;
        }
        if (!have_format_) {
            return AUDERR_NEEDWAVEFORMAT;
        }
        *pqTimeStamp = written_;
        return S_OK;
    }

    STDMETHODIMP Claim() override
    {
        if (!have_format_) {
            return AUDERR_NEEDWAVEFORMAT;
        }
        if (state_ != State::Unclaimed) {
            IVX_LOG_D("IAudio::Claim while already claimed");
            return AUDERR_ALREADYCLAIMED;
        }
        state_ = State::Claimed;
        IVX_LOG_D("IAudio::Claim");
        target_->engine_claimed();
        notify_later([](IAudioDestNotifySink* s) { s->AudioStart(); });
        return S_OK;
    }

    STDMETHODIMP UnClaim() override
    {
        if (!have_format_) {
            return AUDERR_NEEDWAVEFORMAT;
        }
        if (state_ == State::Unclaimed) {
            return AUDERR_NOTCLAIMED;
        }
        state_ = State::Unclaimed;
        IVX_LOG_D("IAudio::UnClaim (all audio already consumed)");
        target_->engine_unclaimed();
        notify_later([](IAudioDestNotifySink* s) { s->AudioStop(IANSRSN_NODATA); });
        return S_OK;
    }

    STDMETHODIMP Start() override
    {
        if (!have_format_) {
            return AUDERR_NEEDWAVEFORMAT;
        }
        if (state_ == State::Started) {
            return AUDERR_ALREADYSTARTED;
        }
        if (state_ != State::Claimed) {
            return AUDERR_NOTCLAIMED;
        }
        state_ = State::Started;
        IVX_LOG_D("IAudio::Start");
        return S_OK;
    }

    STDMETHODIMP Stop() override
    {
        if (!have_format_) {
            return AUDERR_NEEDWAVEFORMAT;
        }
        if (state_ == State::Started) {
            state_ = State::Claimed;
        }
        IVX_LOG_D("IAudio::Stop");
        return S_OK;
    }

    STDMETHODIMP TotalGet(QWORD* pqWord) override
    {
        if (!pqWord) {
            return E_POINTER;
        }
        if (!have_format_) {
            return AUDERR_NEEDWAVEFORMAT;
        }
        *pqWord = written_;
        return S_OK;
    }

    STDMETHODIMP ToFileTime(QWORD* pqWord, FILETIME* pFT) override
    {
        if (!pqWord || !pFT) {
            return E_POINTER;
        }
        if (!have_format_) {
            return AUDERR_NEEDWAVEFORMAT;
        }
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        ULARGE_INTEGER ticks;
        ticks.LowPart = now.dwLowDateTime;
        ticks.HighPart = now.dwHighDateTime;
        if (format_.nAvgBytesPerSec > 0) {
            ticks.QuadPart += (*pqWord * 10000000ULL) / format_.nAvgBytesPerSec;
        }
        pFT->dwLowDateTime = ticks.LowPart;
        pFT->dwHighDateTime = ticks.HighPart;
        return S_OK;
    }

    STDMETHODIMP WaveFormatGet(SDATA* pdWFEX) override
    {
        if (!pdWFEX) {
            return E_POINTER;
        }
        pdWFEX->pData = nullptr;
        pdWFEX->dwSize = 0;
        if (!have_format_) {
            // Reporting "no format" is what makes Infovox 330 choose its own native rate
            // instead of trying to match ours. Do not turn this into a success.
            IVX_LOG_D("IAudio::WaveFormatGet before any format was set");
            return AUDERR_NEEDWAVEFORMAT;
        }
        void* mem = CoTaskMemAlloc(sizeof(WfxRaw));
        if (!mem) {
            return E_OUTOFMEMORY;
        }
        WfxRaw raw{};
        raw_from_wfx(format_, raw);
        std::memcpy(mem, &raw, sizeof(raw));
        pdWFEX->pData = mem;
        pdWFEX->dwSize = sizeof(WfxRaw);
        return S_OK;
    }

    STDMETHODIMP WaveFormatSet(SDATA dWFEX) override
    {
        if (!dWFEX.pData || dWFEX.dwSize < sizeof(WfxRaw)) {
            return E_INVALIDARG;
        }
        WfxRaw raw{};
        std::memcpy(&raw, dWFEX.pData, sizeof(raw));

        WAVEFORMATEX wfx{};
        wfx_from_raw(raw, wfx);

        if (have_format_) {
            if (std::memcmp(&wfx, &format_, sizeof(WAVEFORMATEX)) == 0) {
                return S_OK;
            }
            IVX_LOG_W("IAudio::WaveFormatSet tried to change format after it was fixed");
            return AUDERR_WAVEDEVICEBUSY;
        }
        if (wfx.wFormatTag != WAVE_FORMAT_PCM) {
            IVX_LOG_E("IAudio::WaveFormatSet with unsupported format tag %u", wfx.wFormatTag);
            return AUDERR_WAVEFORMATNOTSUPPORTED;
        }

        format_ = wfx;
        have_format_ = true;
        state_ = State::Unclaimed;
        free_bytes_ = format_.nAvgBytesPerSec * kBufferSeconds;
        IVX_LOG_I("engine wave format: %lu Hz, %u-bit, %u channel(s), %lu bytes/s",
                  format_.nSamplesPerSec, format_.wBitsPerSample, format_.nChannels,
                  format_.nAvgBytesPerSec);
        return S_OK;
    }

    // ---- IAudioDest ----

    STDMETHODIMP FreeSpace(DWORD* pdwBytes, BOOL* pfEOF) override
    {
        if (!pdwBytes || !pfEOF) {
            return E_POINTER;
        }
        if (!have_format_) {
            return AUDERR_NEEDWAVEFORMAT;
        }
        *pdwBytes = free_bytes_;
        *pfEOF = FALSE;
        return S_OK;
    }

    STDMETHODIMP DataSet(void* pBuffer, DWORD dwSize) override
    {
        if (!pBuffer) {
            return E_INVALIDARG;
        }
        if (!have_format_) {
            return AUDERR_NEEDWAVEFORMAT;
        }
        if (state_ == State::Unclaimed) {
            IVX_LOG_W("IAudioDest::DataSet of %lu bytes while unclaimed", dwSize);
            return AUDERR_NOTCLAIMED;
        }
        written_ += dwSize;
        IVX_LOG_T("IAudioDest::DataSet %lu bytes (total %llu)", dwSize, written_);
        target_->audio_written(pBuffer, dwSize, written_);
        // The buffer emptied as fast as it filled, so tell the engine the room is back.
        notify_later([bytes = free_bytes_](IAudioDestNotifySink* s) { s->FreeSpace(bytes, FALSE); });
        return S_OK;
    }

    STDMETHODIMP BookMark(DWORD dwMarkID) override
    {
        if (!have_format_) {
            return AUDERR_NEEDWAVEFORMAT;
        }
        IVX_LOG_T("IAudioDest::BookMark id=%lu at byte %llu", dwMarkID, written_);
        target_->begin_mark(written_);
        // Notified straight away rather than queued. The buffer is already "played", and
        // the engine only translates its internal mark id back into the number the text
        // asked for - reported through ITTSBufNotifySink::BookMark - while it still
        // considers the utterance live. Deferring this loses every bookmark.
        if (notify_) {
            notify_->BookMark(dwMarkID, FALSE);
        }
        target_->end_mark();
        return S_OK;
    }

    [[nodiscard]] unsigned __int64 written() const { return written_; }
    [[nodiscard]] bool have_format() const { return have_format_; }
    [[nodiscard]] const WAVEFORMATEX& format() const { return format_; }

private:
    enum class State { Invalid, Unclaimed, Claimed, Started };

    void notify_later(std::function<void(IAudioDestNotifySink*)> fn)
    {
        IAudioDestNotifySink* sink = notify_;
        if (!sink) {
            return;
        }
        thread_->post([sink, fn = std::move(fn)] { fn(sink); });
    }

    EngineThread* thread_;
    StreamTarget* target_;
    std::atomic<long> refs_{1};
    State state_ = State::Invalid;
    bool have_format_ = false;
    WAVEFORMATEX format_{};
    DWORD free_bytes_ = 0;
    DWORD level_ = 0xFFFFFFFF;
    unsigned __int64 written_ = 0;
    IAudioDestNotifySink* notify_ = nullptr;
};

// The engine's "something happened to the audio" channel.
class NotifySink : public ITTSNotifySinkW {
public:
    explicit NotifySink(StreamTarget* target) : target_(target) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(ITTSNotifySinkW))) {
            *ppv = static_cast<ITTSNotifySinkW*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(refs_.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        const long n = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n <= 0) {
            refs_.store(1, std::memory_order_relaxed);
            return 1;
        }
        return static_cast<ULONG>(n);
    }

    STDMETHODIMP AttribChanged(DWORD dwAttribute) override
    {
        IVX_LOG_D("ITTSNotifySink::AttribChanged %lu", dwAttribute);
        return S_OK;
    }

    STDMETHODIMP AudioStart(QWORD qTimeStamp) override
    {
        IVX_LOG_D("ITTSNotifySink::AudioStart ts=%llu", qTimeStamp);
        return S_OK;
    }

    STDMETHODIMP AudioStop(QWORD qTimeStamp) override
    {
        IVX_LOG_D("ITTSNotifySink::AudioStop ts=%llu", qTimeStamp);
        target_->engine_audio_stopped();
        return S_OK;
    }

    STDMETHODIMP Visual(QWORD, WCHAR, WCHAR, DWORD, TTSMOUTH*) override { return S_OK; }

private:
    StreamTarget* target_;
    std::atomic<long> refs_{1};
};

// The engine's "something happened to this piece of text" channel.
class BufSink : public ITTSBufNotifySink {
public:
    explicit BufSink(StreamTarget* target) : target_(target) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(ITTSBufNotifySink))) {
            *ppv = static_cast<ITTSBufNotifySink*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return static_cast<ULONG>(refs_.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        const long n = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n <= 0) {
            refs_.store(1, std::memory_order_relaxed);
            return 1;
        }
        return static_cast<ULONG>(n);
    }

    STDMETHODIMP TextDataDone(QWORD qTimeStamp, DWORD dwFlags) override
    {
        IVX_LOG_D("ITTSBufNotifySink::TextDataDone ts=%llu flags=0x%lX", qTimeStamp, dwFlags);
        target_->engine_text_done();
        return S_OK;
    }

    STDMETHODIMP TextDataStarted(QWORD qTimeStamp) override
    {
        IVX_LOG_D("ITTSBufNotifySink::TextDataStarted ts=%llu", qTimeStamp);
        return S_OK;
    }

    STDMETHODIMP BookMark(QWORD qTimeStamp, DWORD dwMarkNum) override
    {
        // This is the mark number the text actually asked for. The engine's timestamps are
        // byte positions in the stream we handed it, so qTimeStamp is the audio offset.
        IVX_LOG_D("ITTSBufNotifySink::BookMark id=%lu ts=%llu", dwMarkNum, qTimeStamp);
        target_->text_bookmark(dwMarkNum, qTimeStamp);
        return S_OK;
    }

    STDMETHODIMP WordPosition(QWORD qTimeStamp, DWORD dwByteOffset) override
    {
        IVX_LOG_T("ITTSBufNotifySink::WordPosition offset=%lu ts=%llu", dwByteOffset, qTimeStamp);
        return S_OK;
    }

private:
    StreamTarget* target_;
    std::atomic<long> refs_{1};
};

using IvxInitFn = int(__cdecl*)(const wchar_t*, const wchar_t*);
using IvxCreateEnumeratorFn = HRESULT(__cdecl*)(void**);
using IvxShutdownFn = void(__cdecl*)();

}  // namespace

class Engine::Impl : public StreamTarget {
public:
    Impl() { fill_expected_format(format_); }

    ~Impl() override { shutdown(); }

    HRESULT ensure_initialised()
    {
        std::lock_guard<std::mutex> lock(init_mutex_);
        if (initialised_) {
            return S_OK;
        }
        if (!thread_) {
            thread_ = std::make_unique<EngineThread>();
        }
        const HRESULT hr = thread_->invoke([this] { return initialise_on_thread(); });
        if (SUCCEEDED(hr)) {
            initialised_ = true;
        }
        return hr;
    }

    [[nodiscard]] bool initialised() const { return initialised_; }

    std::vector<ModeInfo> modes()
    {
        std::lock_guard<std::mutex> lock(init_mutex_);
        return modes_;
    }

    HRESULT get_ranges(const GUID& mode, VoiceRanges& out)
    {
        const HRESULT hr = ensure_initialised();
        if (FAILED(hr)) {
            return hr;
        }
        std::lock_guard<std::mutex> serialise(speak_mutex_);
        const HRESULT sel = thread_->invoke([this, &mode] { return select_mode(mode); });
        if (FAILED(sel)) {
            return sel;
        }
        out = ranges_;
        return S_OK;
    }

    void output_format(WAVEFORMATEX& out)
    {
        std::lock_guard<std::mutex> lock(format_mutex_);
        out = format_;
    }

    HRESULT speak(const SpeakParams& params, SynthSink& sink);

    void abort_current()
    {
        abort_requested_.store(true, std::memory_order_release);
        cv_.notify_all();
        if (thread_ && speaking_.load(std::memory_order_acquire)) {
            thread_->post([this] {
                if (central_) {
                    IVX_LOG_I("aborting: ITTSCentral::AudioReset");
                    central_->AudioReset();
                }
            });
        }
    }

    void shutdown()
    {
        if (!thread_) {
            return;
        }
        thread_->invoke_void([this] { teardown_on_thread(); });
        thread_.reset();
        initialised_ = false;
    }

    // ---- StreamTarget ----

    void audio_written(const void* data, DWORD size, unsigned __int64 total_after) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!speaking_.load(std::memory_order_acquire)) {
            IVX_LOG_W("discarding %lu bytes produced outside an utterance", size);
            return;
        }
        StreamItem item;
        item.kind = StreamItem::Kind::Audio;
        item.offset = total_after - size - utterance_base_;
        item.audio.assign(static_cast<const char*>(data), static_cast<const char*>(data) + size);
        queue_.push_back(std::move(item));
        cv_.notify_all();
    }

    void begin_mark(unsigned __int64 total_at) override
    {
        mark_offset_ = total_at;
        mark_was_bookmark_ = false;
    }

    void text_bookmark(DWORD mark_number, unsigned __int64 absolute_offset) override
    {
        mark_was_bookmark_ = true;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!speaking_.load(std::memory_order_acquire)) {
            return;
        }
        StreamItem item;
        item.kind = StreamItem::Kind::Bookmark;
        item.bookmark_id = mark_number;
        item.offset = absolute_offset >= utterance_base_ ? absolute_offset - utterance_base_
                                                         : mark_offset_ - utterance_base_;
        queue_.push_back(std::move(item));
        cv_.notify_all();
    }

    void end_mark() override
    {
        if (mark_was_bookmark_) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!speaking_.load(std::memory_order_acquire)) {
            return;
        }
        StreamItem item;
        item.kind = StreamItem::Kind::Word;
        item.offset = mark_offset_ - utterance_base_;
        queue_.push_back(std::move(item));
        cv_.notify_all();
    }

    void engine_audio_stopped() override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            audio_stopped_ = true;
        }
        cv_.notify_all();
    }

    void engine_text_done() override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            text_done_ = true;
        }
        cv_.notify_all();
    }

    void engine_flushed() override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.clear();
        }
        cv_.notify_all();
    }

    void engine_claimed() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        unclaimed_tick_ = 0;
    }

    void engine_unclaimed() override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            unclaimed_tick_ = GetTickCount();
            if (unclaimed_tick_ == 0) {
                unclaimed_tick_ = 1;
            }
        }
        cv_.notify_all();
    }

private:
    struct StreamItem {
        enum class Kind { Audio, Bookmark, Word };
        Kind kind = Kind::Audio;
        DWORD bookmark_id = 0;
        unsigned __int64 offset = 0;
        std::vector<char> audio;
    };

    HRESULT initialise_on_thread();
    HRESULT select_mode(const GUID& mode);
    void probe_ranges(DWORD features);
    void release_mode_objects();
    void teardown_on_thread();
    HRESULT start_speak(const SpeakParams& params);
    void finish_speak();

    std::mutex init_mutex_;
    std::mutex speak_mutex_;
    std::mutex format_mutex_;
    std::unique_ptr<EngineThread> thread_;
    bool initialised_ = false;

    HMODULE host_module_ = nullptr;
    IvxInitFn ivx_init_ = nullptr;
    IvxCreateEnumeratorFn ivx_create_enumerator_ = nullptr;
    IvxShutdownFn ivx_shutdown_ = nullptr;

    ITTSEnumW* enumerator_ = nullptr;
    ITTSCentralW* central_ = nullptr;
    ITTSAttributesW* attrs_ = nullptr;
    AudioSink* audio_ = nullptr;
    NotifySink* notify_ = nullptr;
    BufSink* buf_ = nullptr;
    DWORD sink_key_ = 0;
    GUID current_mode_ = GUID_NULL;

    std::vector<ModeInfo> modes_;
    VoiceRanges ranges_;
    std::map<std::wstring, VoiceRanges> range_cache_;

    WAVEFORMATEX format_{};

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<StreamItem> queue_;
    std::atomic<bool> speaking_{false};
    std::atomic<bool> abort_requested_{false};
    bool audio_stopped_ = false;
    bool text_done_ = false;
    DWORD unclaimed_tick_ = 0;
    unsigned __int64 mark_offset_ = 0;
    bool mark_was_bookmark_ = false;
    unsigned __int64 utterance_base_ = 0;
    std::wstring pending_text_;
};

HRESULT Engine::Impl::initialise_on_thread()
{
    const std::wstring host = host_dll_path();
    const std::wstring engine = engine_dll_path();
    const std::wstring voices = voices_dir();

    IVX_LOG_I("initialising: host=%s", log_narrow(host.c_str()).c_str());
    IVX_LOG_I("initialising: engine=%s", log_narrow(engine.c_str()).c_str());
    IVX_LOG_I("initialising: voices=%s", log_narrow(voices.c_str()).c_str());

    if (engine.empty() || voices.empty()) {
        IVX_LOG_E("engine or voices path is empty; data root was not found");
        return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    }
    if (!file_exists(engine)) {
        IVX_LOG_E("engine DLL missing: %s", log_narrow(engine.c_str()).c_str());
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    if (!host_module_) {
        host_module_ = LoadLibraryExW(host.c_str(), nullptr,
                                      LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!host_module_) {
            const DWORD err = GetLastError();
            IVX_LOG_E("LoadLibrary(%s) failed with %lu",
                      log_narrow(host.c_str()).c_str(), err);
            return HRESULT_FROM_WIN32(err);
        }
        ivx_init_ = reinterpret_cast<IvxInitFn>(GetProcAddress(host_module_, "IVX_Init"));
        ivx_create_enumerator_ = reinterpret_cast<IvxCreateEnumeratorFn>(
            GetProcAddress(host_module_, "IVX_CreateEnumerator"));
        ivx_shutdown_ =
            reinterpret_cast<IvxShutdownFn>(GetProcAddress(host_module_, "IVX_Shutdown"));
        if (!ivx_init_ || !ivx_create_enumerator_) {
            IVX_LOG_E("infovox_host.dll is missing IVX_Init or IVX_CreateEnumerator");
            return E_FAIL;
        }
    }

    const int rc = ivx_init_(engine.c_str(), voices.c_str());
    if (rc != 0) {
        IVX_LOG_E("IVX_Init failed with %d", rc);
        return E_FAIL;
    }
    IVX_LOG_I("IVX_Init succeeded");

    if (!enumerator_) {
        void* raw = nullptr;
        const HRESULT hr = ivx_create_enumerator_(&raw);
        if (FAILED(hr) || !raw) {
            IVX_LOG_E("IVX_CreateEnumerator failed %s", hresult_string(hr).c_str());
            return FAILED(hr) ? hr : E_FAIL;
        }
        enumerator_ = static_cast<ITTSEnumW*>(raw);
    }

    if (!notify_) {
        notify_ = new NotifySink(this);
    }
    if (!buf_) {
        buf_ = new BufSink(this);
    }

    modes_.clear();
    enumerator_->Reset();
    for (;;) {
        TTSMODEINFOW mode{};
        ULONG fetched = 0;
        const HRESULT hr = enumerator_->Next(1, &mode, &fetched);
        if (FAILED(hr) || fetched == 0) {
            break;
        }
        ModeInfo info;
        info.mode_guid = mode.gModeID;
        info.mode_name = bounded_wstring(mode.szModeName, kSvfnLen);
        info.product_name = bounded_wstring(mode.szProductName, kSvfnLen);
        info.mfg_name = bounded_wstring(mode.szMfgName, kSvfnLen);
        info.speaker = bounded_wstring(mode.szSpeaker, kSvfnLen);
        info.language_id = mode.language.LanguageID;
        info.gender = mode.wGender;
        info.age = mode.wAge;
        info.features = mode.dwFeatures;
        IVX_LOG_I("mode %s '%s' speaker='%s' lang=%u gender=%u features=0x%lX",
                  guid_string(info.mode_guid).c_str(), log_narrow(info.mode_name.c_str()).c_str(),
                  log_narrow(info.speaker.c_str()).c_str(), info.language_id, info.gender,
                  info.features);
        modes_.push_back(std::move(info));
    }

    if (modes_.empty()) {
        IVX_LOG_E("the engine enumerated no modes");
        return E_FAIL;
    }
    IVX_LOG_I("engine ready with %zu modes", modes_.size());
    return S_OK;
}

void Engine::Impl::release_mode_objects()
{
    if (central_ && sink_key_ != 0) {
        const HRESULT hr = central_->UnRegister(sink_key_);
        if (FAILED(hr)) {
            IVX_LOG_D("UnRegister returned %s (ignored)", hresult_string(hr).c_str());
        }
        sink_key_ = 0;
    }
    if (attrs_) {
        attrs_->Release();
        attrs_ = nullptr;
    }
    if (central_) {
        central_->Release();
        central_ = nullptr;
    }
    // The audio sink is deliberately not deleted; the engine may still hold a pointer.
    audio_ = nullptr;
    current_mode_ = GUID_NULL;
}

HRESULT Engine::Impl::select_mode(const GUID& mode)
{
    if (central_ && IsEqualGUID(current_mode_, mode)) {
        return S_OK;
    }

    IVX_LOG_I("selecting mode %s", guid_string(mode).c_str());

    // Infovox assumes a single live ITTSCentral, so the old one goes first.
    release_mode_objects();

    audio_ = new AudioSink(thread_.get(), this);

    ITTSCentralW* central = nullptr;
    HRESULT hr = enumerator_->Select(mode, &central, static_cast<IAudio*>(audio_));
    if (FAILED(hr) || !central) {
        IVX_LOG_E("ITTSEnum::Select failed %s", hresult_string(hr).c_str());
        return FAILED(hr) ? hr : E_FAIL;
    }
    central_ = central;

    hr = central_->Register(static_cast<ITTSNotifySinkW*>(notify_), __uuidof(ITTSNotifySinkW),
                            &sink_key_);
    if (FAILED(hr)) {
        IVX_LOG_W("ITTSCentral::Register failed %s; continuing without notifications",
                  hresult_string(hr).c_str());
        sink_key_ = 0;
    }

    hr = central_->QueryInterface(__uuidof(ITTSAttributesW), reinterpret_cast<void**>(&attrs_));
    if (FAILED(hr)) {
        IVX_LOG_W("ITTSAttributes unavailable %s", hresult_string(hr).c_str());
        attrs_ = nullptr;
    }

    current_mode_ = mode;

    DWORD features = 0;
    for (const auto& m : modes_) {
        if (IsEqualGUID(m.mode_guid, mode)) {
            features = m.features;
            break;
        }
    }

    const std::wstring key = [&mode] {
        wchar_t buf[64];
        StringFromGUID2(mode, buf, 64);
        return std::wstring(buf);
    }();

    const auto cached = range_cache_.find(key);
    if (cached != range_cache_.end()) {
        ranges_ = cached->second;
        // Re-apply the defaults the probe discovered, since this is a fresh ITTSCentral.
        if (attrs_) {
            if (ranges_.speed.supported) attrs_->SpeedSet(ranges_.speed.default_value);
            if (ranges_.pitch.supported) attrs_->PitchSet(static_cast<WORD>(ranges_.pitch.default_value));
        }
    } else {
        probe_ranges(features);
        range_cache_[key] = ranges_;
    }
    return S_OK;
}

void Engine::Impl::probe_ranges(DWORD features)
{
    ranges_ = VoiceRanges{};
    if (!attrs_) {
        return;
    }

    if (features & TTSFEATURE_SPEED) {
        DWORD current = 0;
        DWORD low = 0;
        DWORD high = 0;
        if (SUCCEEDED(attrs_->SpeedGet(&current)) &&
            SUCCEEDED(attrs_->SpeedSet(TTSATTR_MINSPEED)) && SUCCEEDED(attrs_->SpeedGet(&low)) &&
            SUCCEEDED(attrs_->SpeedSet(TTSATTR_MAXSPEED)) && SUCCEEDED(attrs_->SpeedGet(&high))) {
            if (high > low) {
                ranges_.speed.supported = true;
                ranges_.speed.min_value = low;
                // The engine clamps to its maximum; backing off by one avoids the corner
                // case where "exactly maximum" behaves differently.
                ranges_.speed.max_value = high - 1;
                ranges_.speed.default_value =
                    (std::max)(low, (std::min)(ranges_.speed.max_value, current));
            }
            attrs_->SpeedSet(ranges_.speed.supported ? ranges_.speed.default_value : current);
        }
    }

    if (features & TTSFEATURE_PITCH) {
        WORD current = 0;
        WORD low = 0;
        WORD high = 0;
        if (SUCCEEDED(attrs_->PitchGet(&current)) &&
            SUCCEEDED(attrs_->PitchSet(TTSATTR_MINPITCH)) && SUCCEEDED(attrs_->PitchGet(&low)) &&
            SUCCEEDED(attrs_->PitchSet(TTSATTR_MAXPITCH)) && SUCCEEDED(attrs_->PitchGet(&high))) {
            if (high > low) {
                ranges_.pitch.supported = true;
                ranges_.pitch.min_value = low;
                ranges_.pitch.max_value = high;
                ranges_.pitch.default_value = (std::max<DWORD>)(low, (std::min<DWORD>)(high, current));
            }
            attrs_->PitchSet(static_cast<WORD>(
                ranges_.pitch.supported ? ranges_.pitch.default_value : current));
        }
    }

    if (features & TTSFEATURE_VOLUME) {
        DWORD current = 0;
        DWORD low = 0;
        DWORD high = 0;
        if (SUCCEEDED(attrs_->VolumeGet(&current)) &&
            SUCCEEDED(attrs_->VolumeSet(TTSATTR_MINVOLUME)) &&
            SUCCEEDED(attrs_->VolumeGet(&low)) &&
            SUCCEEDED(attrs_->VolumeSet(TTSATTR_MAXVOLUME)) &&
            SUCCEEDED(attrs_->VolumeGet(&high))) {
            low &= 0xFFFF;
            high &= 0xFFFF;
            if (high > low) {
                ranges_.volume.supported = true;
                ranges_.volume.min_value = low;
                ranges_.volume.max_value = high;
                ranges_.volume.default_value = high;
            }
            const DWORD restore = ranges_.volume.supported ? ranges_.volume.default_value
                                                           : (current & 0xFFFF);
            attrs_->VolumeSet(restore | (restore << 16));
        }
    }

    // SAPI 5 has no concept matching SAPI 4's "real time" attribute, so it is not exposed;
    // it is read once and logged so its value is on the record if it ever matters.
    DWORD real_time = 0;
    if (SUCCEEDED(attrs_->RealTimeGet(&real_time))) {
        IVX_LOG_I("engine RealTime attribute reads %lu", real_time);
    } else {
        IVX_LOG_D("engine has no RealTime attribute");
    }

    IVX_LOG_I("ranges: speed %s [%lu..%lu] default %lu | pitch %s [%lu..%lu] default %lu | "
              "volume %s [%lu..%lu] default %lu",
              ranges_.speed.supported ? "yes" : "no", ranges_.speed.min_value,
              ranges_.speed.max_value, ranges_.speed.default_value,
              ranges_.pitch.supported ? "yes" : "no", ranges_.pitch.min_value,
              ranges_.pitch.max_value, ranges_.pitch.default_value,
              ranges_.volume.supported ? "yes" : "no", ranges_.volume.min_value,
              ranges_.volume.max_value, ranges_.volume.default_value);
}

HRESULT Engine::Impl::start_speak(const SpeakParams& params)
{
    HRESULT hr = select_mode(params.mode);
    if (FAILED(hr)) {
        return hr;
    }

    if (attrs_) {
        if (params.speed >= 0 && ranges_.speed.supported) {
            attrs_->SpeedSet(static_cast<DWORD>(params.speed));
        }
        if (params.pitch >= 0 && ranges_.pitch.supported) {
            attrs_->PitchSet(static_cast<WORD>(params.pitch));
        }
        if (params.volume >= 0 && ranges_.volume.supported) {
            const DWORD v = static_cast<DWORD>(params.volume) & 0xFFFF;
            attrs_->VolumeSet(v | (v << 16));
        }
        if (params.realtime >= 0) {
            const HRESULT rt = attrs_->RealTimeSet(static_cast<DWORD>(params.realtime));
            IVX_LOG_D("RealTimeSet(%d) -> %s", params.realtime, hresult_string(rt).c_str());
        }
    }

    utterance_base_ = audio_ ? audio_->written() : 0;
    pending_text_ = params.text;

    SDATA data{};
    data.pData = const_cast<wchar_t*>(pending_text_.c_str());
    data.dwSize = static_cast<DWORD>((pending_text_.size() + 1) * sizeof(wchar_t));

    IVX_LOG_I("TextData: %zu chars, base offset %llu", pending_text_.size(), utterance_base_);
    IVX_LOG_D("text: %s", log_narrow(pending_text_.c_str()).c_str());

    hr = central_->TextData(CHARSET_TEXT, TTSDATAFLAG_TAGGED, data,
                            static_cast<ITTSBufNotifySink*>(buf_), __uuidof(ITTSBufNotifySink));
    if (FAILED(hr)) {
        IVX_LOG_E("ITTSCentral::TextData failed %s", hresult_string(hr).c_str());
    }
    return hr;
}

void Engine::Impl::finish_speak()
{
    pending_text_.clear();
}

HRESULT Engine::Impl::speak(const SpeakParams& params, SynthSink& sink)
{
    HRESULT hr = ensure_initialised();
    if (FAILED(hr)) {
        return hr;
    }

    std::lock_guard<std::mutex> serialise(speak_mutex_);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        audio_stopped_ = false;
        text_done_ = false;
        unclaimed_tick_ = 0;
        utterance_base_ = 0;
    }
    abort_requested_.store(false, std::memory_order_release);
    speaking_.store(true, std::memory_order_release);

    hr = thread_->invoke([this, &params] { return start_speak(params); });
    if (FAILED(hr)) {
        speaking_.store(false, std::memory_order_release);
        thread_->invoke_void([this] { finish_speak(); });
        return hr;
    }

    {
        std::lock_guard<std::mutex> lock(format_mutex_);
        if (audio_ && audio_->have_format()) {
            format_ = audio_->format();
        }
    }

    const DWORD started = GetTickCount();
    DWORD last_progress = started;
    DWORD abort_deadline = 0;
    bool aborted = false;
    unsigned __int64 total_bytes = 0;

    for (;;) {
        std::deque<StreamItem> batch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(20), [this] {
                return !queue_.empty() || audio_stopped_ || text_done_ || unclaimed_tick_ != 0 ||
                       abort_requested_.load(std::memory_order_acquire);
            });
            batch.swap(queue_);
        }

        for (auto& item : batch) {
            last_progress = GetTickCount();
            if (aborted) {
                continue;
            }
            if (item.kind == StreamItem::Kind::Bookmark) {
                sink.on_bookmark(item.offset, item.bookmark_id);
            } else if (item.kind == StreamItem::Kind::Word) {
                sink.on_word_boundary(item.offset);
            } else {
                total_bytes += item.audio.size();
                if (!sink.on_audio(item.audio.data(), static_cast<DWORD>(item.audio.size()))) {
                    IVX_LOG_I("sink asked to stop after %llu bytes", total_bytes);
                    aborted = true;
                }
            }
        }

        if (!aborted && sink.should_abort()) {
            IVX_LOG_I("sink reported abort");
            aborted = true;
        }
        if (!aborted && abort_requested_.load(std::memory_order_acquire)) {
            aborted = true;
        }

        if (aborted && abort_deadline == 0) {
            abort_current();
            abort_deadline = GetTickCount() + kAbortSettleMs;
        }

        bool queue_empty = false;
        bool done_flagged = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_empty = queue_.empty();
            // TextDataDone and the engine's own AudioStop are definitive. An unclaim on its
            // own is not: engines that unclaim between sentences re-claim immediately, so
            // it only counts once it has stood for a moment with nothing else arriving.
            done_flagged = audio_stopped_ || text_done_ ||
                           (unclaimed_tick_ != 0 && GetTickCount() - unclaimed_tick_ > 250);
        }

        if (done_flagged && queue_empty) {
            break;
        }
        if (aborted && (queue_empty || GetTickCount() > abort_deadline)) {
            break;
        }

        const DWORD now = GetTickCount();
        if (now - last_progress > kStallTimeoutMs) {
            IVX_LOG_E("engine produced nothing for %lu ms; giving up on this utterance",
                      now - last_progress);
            hr = E_FAIL;
            break;
        }
        if (now - started > kUtteranceTimeoutMs) {
            IVX_LOG_E("utterance exceeded %lu ms; giving up", kUtteranceTimeoutMs);
            hr = E_FAIL;
            break;
        }
    }

    speaking_.store(false, std::memory_order_release);
    thread_->invoke_void([this] { finish_speak(); });

    IVX_LOG_I("utterance finished: %llu bytes%s", total_bytes, aborted ? " (aborted)" : "");
    if (SUCCEEDED(hr) && total_bytes == 0 && !aborted) {
        IVX_LOG_W("utterance produced no audio at all");
    }
    return hr;
}

void Engine::Impl::teardown_on_thread()
{
    release_mode_objects();
    if (enumerator_) {
        enumerator_->Release();
        enumerator_ = nullptr;
    }
    if (ivx_shutdown_) {
        ivx_shutdown_();
    }
    // infovox_host.dll and its patched engine stay loaded: the shim rewrites the engine's
    // import table in place and unloading it mid-process has no upside.
    IVX_LOG_I("engine torn down");
}

Engine::Engine() : impl_(std::make_unique<Impl>()) {}
Engine::~Engine() = default;

Engine& Engine::instance()
{
    // Deliberately leaked. A static destructor would run during DLL_PROCESS_DETACH and try
    // to join the engine thread while the loader lock is held, which deadlocks. Letting the
    // process exit take it costs nothing.
    static Engine* engine = new Engine();
    return *engine;
}

HRESULT Engine::ensure_initialised() { return impl_->ensure_initialised(); }
bool Engine::initialised() const { return impl_->initialised(); }
std::vector<ModeInfo> Engine::modes() { return impl_->modes(); }
HRESULT Engine::get_ranges(const GUID& mode, VoiceRanges& out) { return impl_->get_ranges(mode, out); }
void Engine::output_format(WAVEFORMATEX& out) { impl_->output_format(out); }
HRESULT Engine::speak(const SpeakParams& params, SynthSink& sink) { return impl_->speak(params, sink); }
void Engine::abort_current() { impl_->abort_current(); }
void Engine::shutdown() { impl_->shutdown(); }


namespace {

// On 32-bit the engine is right here, so the backend is a pass-through.
class LocalBackend : public SynthBackend {
public:
    HRESULT ensure_ready() override { return Engine::instance().ensure_initialised(); }
    void output_format(WAVEFORMATEX& out) override { Engine::instance().output_format(out); }

    HRESULT get_ranges(const GUID& mode, VoiceRanges& out) override
    {
        return Engine::instance().get_ranges(mode, out);
    }

    HRESULT speak(const SpeakParams& params, SynthSink& sink) override
    {
        return Engine::instance().speak(params, sink);
    }

    void abort_current() override { Engine::instance().abort_current(); }
};

}  // namespace

SynthBackend* create_backend()
{
    return new LocalBackend();
}

}  // namespace ivx
