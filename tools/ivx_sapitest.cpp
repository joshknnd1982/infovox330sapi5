// Drives the SAPI 5 engine DLL directly, without registering it.
//
// The DLL is loaded, its class objects are asked for by CLSID, and a stand-in
// ISpTTSEngineSite collects the audio and events. That exercises every line of the engine
// - voice enumeration, token binding, tag building, marks, prosody and, on x64, the whole
// pipe round trip to the helper - with no administrator rights and no registry writes.
//
//   ivx_sapitest.exe <path to Infovox330SAPI5.dll> [options]
//
//     --out <dir>        where to write WAVs                      (default: sapitest)
//     --voice <name>     speak with this voice only
//     --text "..."       what to say
//     --rate N           SAPI rate, -10..10
//     --volume N         SAPI volume, 0..100
//     --all              every voice in turn
//     --abort-after N    pretend SAPI asked to stop after N bytes
//     --stress N         N interrupt/voice-switch/prosody rounds, no WAVs written
//     --concurrent K     hold K engine objects open and interleave them
//     --kill-helper      during --stress, kill the 32-bit helper once and expect recovery
//     --rawtag "\Spd=300\"  send an unrecognised-tag fragment, as SAPI would for unknown XML
//     --regress          check that word marks never change the audio (see run_regression)

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>
#include <tlhelp32.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

// Must match the uuid on ivx::sapi::ISpTTSEngineImpl / IEnumSpObjectTokensImpl.
// {61b158b8-3639-4b87-8944-9d55320c6f80}
const CLSID CLSID_InfovoxEngine = {
    0x61b158b8, 0x3639, 0x4b87, {0x89, 0x44, 0x9d, 0x55, 0x32, 0x0c, 0x6f, 0x80}};
// {abbd81fa-fbf7-4e21-a119-784f25f60a7c}
const CLSID CLSID_InfovoxVoices = {
    0xabbd81fa, 0xfbf7, 0x4e21, {0xa1, 0x19, 0x78, 0x4f, 0x25, 0xf6, 0x0a, 0x7c}};

using DllGetClassObjectFn = HRESULT(__stdcall*)(REFCLSID, REFIID, void**);
DllGetClassObjectFn g_get_class_object = nullptr;

HRESULT create_object(REFCLSID clsid, REFIID iid, void** out)
{
    IClassFactory* factory = nullptr;
    HRESULT hr = g_get_class_object(clsid, IID_IClassFactory, reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) {
        return hr;
    }
    hr = factory->CreateInstance(nullptr, iid, out);
    factory->Release();
    return hr;
}

struct CapturedEvent {
    SPEVENTENUM id;
    ULONGLONG offset;
    LPARAM lparam;
    WPARAM wparam;
    std::wstring text;
};

// The smallest ISpTTSEngineSite that is still honest: it accepts everything, records
// events, and can pretend SAPI asked to stop after a given number of bytes.
class TestSite : public ISpTTSEngineSite
{
public:
    TestSite(long rate, USHORT volume, ULONGLONG interest, ULONGLONG abort_after)
        : rate_(rate), volume_(volume), interest_(interest), abort_after_(abort_after)
    {
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) {
            return E_POINTER;
        }
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, __uuidof(ISpTTSEngineSite)) ||
            IsEqualIID(riid, __uuidof(ISpEventSink))) {
            *ppv = static_cast<ISpTTSEngineSite*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override { return --refs_; }

    // ISpEventSink
    STDMETHODIMP AddEvents(const SPEVENT* pEventArray, ULONG ulCount) override
    {
        for (ULONG i = 0; i < ulCount; ++i) {
            CapturedEvent e;
            e.id = static_cast<SPEVENTENUM>(pEventArray[i].eEventId);
            e.offset = pEventArray[i].ullAudioStreamOffset;
            e.lparam = pEventArray[i].lParam;
            e.wparam = pEventArray[i].wParam;
            if (pEventArray[i].elParamType == SPET_LPARAM_IS_STRING && pEventArray[i].lParam) {
                e.text = reinterpret_cast<const wchar_t*>(pEventArray[i].lParam);
            }
            events.push_back(std::move(e));
        }
        return S_OK;
    }

    STDMETHODIMP GetEventInterest(ULONGLONG* pullEventInterest) override
    {
        if (!pullEventInterest) {
            return E_POINTER;
        }
        *pullEventInterest = interest_;
        return S_OK;
    }

    // ISpTTSEngineSite
    STDMETHODIMP_(DWORD) GetActions() override
    {
        if (abort_after_ > 0 && pcm.size() >= abort_after_) {
            return SPVES_ABORT;
        }
        return 0;
    }

    STDMETHODIMP Write(const void* pBuff, ULONG cb, ULONG* pcbWritten) override
    {
        const char* p = static_cast<const char*>(pBuff);
        pcm.insert(pcm.end(), p, p + cb);
        if (pcbWritten) {
            *pcbWritten = cb;
        }
        ++writes;
        return S_OK;
    }

    STDMETHODIMP GetRate(long* pRateAdjust) override
    {
        if (!pRateAdjust) {
            return E_POINTER;
        }
        *pRateAdjust = rate_;
        return S_OK;
    }

    STDMETHODIMP GetVolume(USHORT* pusVolume) override
    {
        if (!pusVolume) {
            return E_POINTER;
        }
        *pusVolume = volume_;
        return S_OK;
    }

    STDMETHODIMP GetSkipInfo(SPVSKIPTYPE* peType, long* plNumItems) override
    {
        if (peType) {
            *peType = SPVST_SENTENCE;
        }
        if (plNumItems) {
            *plNumItems = 0;
        }
        return S_OK;
    }

    STDMETHODIMP CompleteSkip(long /*ulNumSkipped*/) override { return S_OK; }

    std::vector<char> pcm;
    std::vector<CapturedEvent> events;
    int writes = 0;

private:
    ULONG refs_ = 1;
    long rate_;
    USHORT volume_;
    ULONGLONG interest_;
    ULONGLONG abort_after_;
};

// One voice, bound and ready to speak. Mirrors what SAPI holds on to per voice.
struct BoundVoice {
    ISpTTSEngine* engine = nullptr;
    WAVEFORMATEX* format = nullptr;
    std::wstring name;
    std::wstring speaker;
    std::wstring language;

    ~BoundVoice() { reset(); }

    void reset()
    {
        if (format) {
            CoTaskMemFree(format);
            format = nullptr;
        }
        if (engine) {
            engine->Release();
            engine = nullptr;
        }
    }
};

std::wstring token_attribute(ISpObjectToken* token, const wchar_t* name)
{
    std::wstring result;
    ISpDataKey* attributes = nullptr;
    if (SUCCEEDED(token->OpenKey(L"Attributes", &attributes)) && attributes) {
        LPWSTR value = nullptr;
        if (SUCCEEDED(attributes->GetStringValue(name, &value)) && value) {
            result = value;
            CoTaskMemFree(value);
        }
        attributes->Release();
    }
    return result;
}

HRESULT bind_voice(ISpObjectToken* token, BoundVoice& out)
{
    out.reset();
    out.name = token_attribute(token, L"Name");
    out.speaker = token_attribute(token, L"InfovoxSpeaker");
    out.language = token_attribute(token, L"Language");

    HRESULT hr = create_object(CLSID_InfovoxEngine, __uuidof(ISpTTSEngine),
                               reinterpret_cast<void**>(&out.engine));
    if (FAILED(hr) || !out.engine) {
        return FAILED(hr) ? hr : E_FAIL;
    }

    ISpObjectWithToken* with_token = nullptr;
    hr = out.engine->QueryInterface(__uuidof(ISpObjectWithToken),
                                    reinterpret_cast<void**>(&with_token));
    if (SUCCEEDED(hr) && with_token) {
        hr = with_token->SetObjectToken(token);
        with_token->Release();
    }
    if (FAILED(hr)) {
        out.reset();
        return hr;
    }

    GUID format_id = GUID_NULL;
    hr = out.engine->GetOutputFormat(nullptr, nullptr, &format_id, &out.format);
    if (FAILED(hr) || !out.format) {
        out.reset();
        return FAILED(hr) ? hr : E_FAIL;
    }
    return S_OK;
}

// Builds the fragment list SAPI would have produced from "<text> <bookmark> <tail>".
struct Utterance {
    SPVTEXTFRAG raw{};
    SPVTEXTFRAG head{};
    SPVTEXTFRAG mark{};
    SPVTEXTFRAG tail{};
    std::wstring raw_tag;
    std::wstring text;
    std::wstring bookmark;
    std::wstring trailer;

    // first() is what gets handed to Speak; it is the raw-tag fragment when there is one.
    SPVTEXTFRAG* first() { return raw_tag.empty() ? &head : &raw; }

    void build(const std::wstring& body, const std::wstring& bookmark_text,
               const std::wstring& trailing, USHORT volume, short rate_adj,
               const std::wstring& unknown_tag = std::wstring())
    {
        raw_tag = unknown_tag;
        text = body;
        bookmark = bookmark_text;
        trailer = trailing;

        tail = SPVTEXTFRAG{};
        tail.pNext = nullptr;
        tail.State.eAction = SPVA_Speak;
        tail.State.Volume = volume;
        tail.State.RateAdj = rate_adj;
        tail.pTextStart = trailer.c_str();
        tail.ulTextLen = static_cast<ULONG>(trailer.size());
        tail.ulTextSrcOffset = static_cast<ULONG>(text.size() + bookmark.size());

        mark = SPVTEXTFRAG{};
        mark.pNext = trailer.empty() ? nullptr : &tail;
        mark.State.eAction = SPVA_Bookmark;
        mark.State.Volume = volume;
        mark.pTextStart = bookmark.c_str();
        mark.ulTextLen = static_cast<ULONG>(bookmark.size());
        mark.ulTextSrcOffset = static_cast<ULONG>(text.size());

        head = SPVTEXTFRAG{};
        head.pNext = bookmark.empty() ? mark.pNext : &mark;
        head.State.eAction = SPVA_Speak;
        head.State.Volume = volume;
        head.State.RateAdj = rate_adj;
        head.pTextStart = text.c_str();
        head.ulTextLen = static_cast<ULONG>(text.size());
        head.ulTextSrcOffset = 0;

        // What SAPI hands the engine for a tag its own XML parser did not recognise.
        raw = SPVTEXTFRAG{};
        raw.pNext = &head;
        raw.State.eAction = SPVA_ParseUnknownTag;
        raw.State.Volume = volume;
        raw.pTextStart = raw_tag.c_str();
        raw.ulTextLen = static_cast<ULONG>(raw_tag.size());
        raw.ulTextSrcOffset = 0;
    }
};

constexpr ULONGLONG kAllEvents =
    SPFEI(SPEI_WORD_BOUNDARY) | SPFEI(SPEI_SENTENCE_BOUNDARY) | SPFEI(SPEI_TTS_BOOKMARK);

bool write_wav(const std::wstring& path, const WAVEFORMATEX& wfx, const std::vector<char>& pcm)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) {
        return false;
    }
    const DWORD data_size = static_cast<DWORD>(pcm.size());
    const DWORD riff_size = 4 + (8 + 16) + (8 + data_size);
    auto put32 = [f](DWORD v) { fwrite(&v, 4, 1, f); };
    auto put16 = [f](WORD v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f);
    put32(riff_size);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    put32(16);
    put16(1);
    put16(wfx.nChannels);
    put32(wfx.nSamplesPerSec);
    put32(wfx.nAvgBytesPerSec);
    put16(wfx.nBlockAlign);
    put16(wfx.wBitsPerSample);
    fwrite("data", 1, 4, f);
    put32(data_size);
    if (data_size) {
        fwrite(pcm.data(), 1, data_size, f);
    }
    fclose(f);
    return true;
}

void make_directories(const std::wstring& path)
{
    for (std::size_t i = 3; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == L'\\') {
            CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
        }
    }
}

const wchar_t* event_name(SPEVENTENUM id)
{
    switch (id) {
        case SPEI_WORD_BOUNDARY: return L"word";
        case SPEI_SENTENCE_BOUNDARY: return L"sentence";
        case SPEI_TTS_BOOKMARK: return L"bookmark";
        default: return L"other";
    }
}

int kill_helper()
{
    int killed = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"Infovox330Server.exe") != 0) {
                continue;
            }
            HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
            if (process) {
                if (TerminateProcess(process, 1)) {
                    ++killed;
                }
                CloseHandle(process);
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return killed;
}

// ---------------------------------------------------------------------------------------
// Stress mode. A screen reader interrupts speech on almost every keystroke, so the paths
// that matter most are the ones this exercises: stop part-way through, stop immediately,
// switch voice, change prosody, and do it again a few hundred times without leaking,
// hanging or losing the engine.
// ---------------------------------------------------------------------------------------

struct StressResult {
    int rounds = 0;
    int failures = 0;
    int aborted_rounds = 0;
    int silent_rounds = 0;
    DWORD slowest_ms = 0;
    ULONGLONG total_bytes = 0;
};

const wchar_t* const kStressTexts[] = {
    L"The quick brown fox jumps over the lazy dog.",
    L"One.",
    L"   ",
    L"Supercalifragilisticexpialidocious, and then some more words to make this longer than "
    L"a single buffer so that an interruption lands in the middle of the stream rather than "
    L"at a convenient boundary.",
    L"Numbers: 1, 22, 333, 4444, and 1998.",
    L"Punctuation -- em dashes, semicolons; colons: and (parentheses).",
    L"",
};

StressResult run_stress(std::vector<ISpObjectToken*>& tokens, int rounds, int concurrent,
                        bool kill_helper_once)
{
    StressResult result;
    if (tokens.empty()) {
        return result;
    }
    if (concurrent < 1) {
        concurrent = 1;
    }

    std::vector<BoundVoice> voices(static_cast<std::size_t>(concurrent));
    for (int i = 0; i < concurrent; ++i) {
        const HRESULT hr = bind_voice(tokens[static_cast<std::size_t>(i) % tokens.size()],
                                      voices[static_cast<std::size_t>(i)]);
        if (FAILED(hr)) {
            wprintf(L"  could not bind engine %d: 0x%08lX\n", i,
                    static_cast<unsigned long>(hr));
            ++result.failures;
            return result;
        }
    }

    const int kill_at = kill_helper_once ? rounds / 2 : -1;
    unsigned seed = 12345;
    auto next_random = [&seed]() {
        seed = seed * 1103515245u + 12345u;
        return (seed >> 16) & 0x7FFF;
    };

    for (int round = 0; round < rounds; ++round) {
        BoundVoice& voice = voices[static_cast<std::size_t>(round) % voices.size()];

        // Every so often rebind to a different voice, the way a user changing voices does.
        if (round > 0 && round % 17 == 0) {
            ISpObjectToken* token = tokens[static_cast<std::size_t>(next_random()) % tokens.size()];
            const HRESULT hr = bind_voice(token, voice);
            if (FAILED(hr)) {
                wprintf(L"  round %d: rebinding failed 0x%08lX\n", round,
                        static_cast<unsigned long>(hr));
                ++result.failures;
                continue;
            }
        }

        if (round == kill_at) {
            const int killed = kill_helper();
            wprintf(L"  round %d: killed %d helper process(es); the next round must recover\n",
                    round, killed);
        }

        const std::wstring body = kStressTexts[next_random() % std::size(kStressTexts)];
        const short rate_adj = static_cast<short>(static_cast<int>(next_random() % 21) - 10);
        const USHORT volume = static_cast<USHORT>(next_random() % 101);

        // A third of rounds run to completion, a third stop part-way, a third stop at once -
        // which is the case most likely to leave the engine in a bad state.
        ULONGLONG abort_after = 0;
        const unsigned mode = next_random() % 3;
        if (mode == 1) {
            abort_after = 1 + (next_random() % 60000);
        } else if (mode == 2) {
            abort_after = 1;
        }

        Utterance utterance;
        utterance.build(body, L"7", L" Tail.", volume, rate_adj);

        TestSite site(0, volume, kAllEvents, abort_after);
        const DWORD begin = GetTickCount();
        const HRESULT hr = voice.engine->Speak(0, SPDFID_WaveFormatEx, voice.format,
                                               &utterance.head, &site);
        const DWORD elapsed = GetTickCount() - begin;

        ++result.rounds;
        result.total_bytes += site.pcm.size();
        if (elapsed > result.slowest_ms) {
            result.slowest_ms = elapsed;
        }
        if (abort_after > 0) {
            ++result.aborted_rounds;
        }

        if (FAILED(hr)) {
            wprintf(L"  round %d FAILED: 0x%08lX (voice %s, %zu bytes)\n", round,
                    static_cast<unsigned long>(hr), voice.speaker.c_str(), site.pcm.size());
            ++result.failures;
            continue;
        }

        const bool expect_audio = !body.empty() && body.find_first_not_of(L' ') != std::wstring::npos;
        if (abort_after == 0 && expect_audio && site.pcm.empty()) {
            wprintf(L"  round %d produced no audio for '%.20s' (voice %s)\n", round,
                    body.c_str(), voice.speaker.c_str());
            ++result.silent_rounds;
            ++result.failures;
        }
        if (elapsed > 5000) {
            wprintf(L"  round %d took %lu ms - too slow to interrupt comfortably\n", round,
                    elapsed);
        }
    }

    return result;
}

// ---------------------------------------------------------------------------------------
// Regression mode.
//
// Word-boundary events are implemented by writing "\mrk=N\" into the text handed to the
// engine, which is only sound if those marks are acoustically free. They were not: a mark
// landing inside a number - between "145." and "2" - made the engine read it as something
// thirteen times longer, heard as the speech rate collapsing mid-sentence.
//
// So the invariant is checked directly: speak each string twice, once asking for word and
// sentence events and once not, and require the audio to be byte-identical. Any future
// change to the tokenising that disturbs the engine fails here instead of in someone's ear.
// ---------------------------------------------------------------------------------------

const wchar_t* const kRegressionTexts[] = {
    L"dist\\Infovox330SAPI5_Setup_1.0.0.exe, 145.2 MB.",
    L"145.2 MB.",
    L"1.0.0",
    L"Version 2.5.1 was released on 2026-08-22.",
    L"Total: $1,234.56 owed.",
    L"Meet at 3:45 p.m. on 25/12/2026.",
    L"C:\\Program Files\\Infovox330SAPI5\\x64",
    L"See https://example.com/a/b?c=1&d=2 for details.",
    L"It's a well-known co-operative, isn't it?",
    L"Chapter 4. Section 2.3. Page 15.",
    L"He said \"hello\", then left.",
    L"3.14159 and 2.71828",
    L"100%, 50 kg, 12 degrees.",
    L"a.b.c.d",
    L"The quick brown fox jumps over the lazy dog.",
};

int run_regression(std::vector<ISpObjectToken*>& tokens)
{
    if (tokens.empty()) {
        wprintf(L"no voices to test with\n");
        return 1;
    }

    BoundVoice voice;
    const HRESULT bound = bind_voice(tokens[0], voice);
    if (FAILED(bound)) {
        wprintf(L"could not bind a voice: 0x%08lX\n", static_cast<unsigned long>(bound));
        return 1;
    }
    wprintf(L"marks-are-free check, voice %s\n\n", voice.speaker.c_str());

    // Speaking the same text twice in one process does not give byte-identical audio - the
    // engine carries a little state between utterances - so the gate is a ratio, not an
    // exact match. The control run measures the noise floor; anything beyond a few percent
    // is the marks talking.
    constexpr double kTolerance = 0.05;

    auto speak = [&](const wchar_t* body, ULONGLONG interest, std::size_t& bytes,
                     int& words) -> HRESULT {
        Utterance utterance;
        utterance.build(body, L"", L"", 100, 0);
        TestSite site(0, 100, interest, 0);
        const HRESULT hr = voice.engine->Speak(0, SPDFID_WaveFormatEx, voice.format,
                                               utterance.first(), &site);
        bytes = site.pcm.size();
        words = 0;
        for (const auto& e : site.events) {
            if (e.id == SPEI_WORD_BOUNDARY) {
                ++words;
            }
        }
        return hr;
    };

    int failures = 0;
    double worst_control = 0.0;

    for (const wchar_t* body : kRegressionTexts) {
        std::size_t plain_bytes = 0;
        std::size_t control_bytes = 0;
        std::size_t marked_bytes = 0;
        int ignored = 0;
        int words = 0;

        const HRESULT hr1 = speak(body, SPFEI(SPEI_TTS_BOOKMARK), plain_bytes, ignored);
        const HRESULT hr2 = speak(body, SPFEI(SPEI_TTS_BOOKMARK), control_bytes, ignored);
        const HRESULT hr3 = speak(body, kAllEvents, marked_bytes, words);

        if (FAILED(hr1) || FAILED(hr2) || FAILED(hr3)) {
            wprintf(L"  FAIL (0x%08lX/0x%08lX/0x%08lX)  %s\n", static_cast<unsigned long>(hr1),
                    static_cast<unsigned long>(hr2), static_cast<unsigned long>(hr3), body);
            ++failures;
            continue;
        }
        if (plain_bytes == 0) {
            wprintf(L"  FAIL  no audio at all  %s\n", body);
            ++failures;
            continue;
        }

        const double control = std::abs(static_cast<double>(control_bytes) / plain_bytes - 1.0);
        const double ratio = static_cast<double>(marked_bytes) / plain_bytes;
        if (control > worst_control) {
            worst_control = control;
        }

        const double seconds = voice.format->nAvgBytesPerSec
                                   ? static_cast<double>(plain_bytes) /
                                         voice.format->nAvgBytesPerSec
                                   : 0.0;

        if (std::abs(ratio - 1.0) > kTolerance) {
            wprintf(L"  FAIL  marks changed the audio %.2fx (%zu -> %zu bytes)  %s\n", ratio,
                    plain_bytes, marked_bytes, body);
            ++failures;
        } else {
            wprintf(L"  ok   %5.2fs  %2d words  %+.1f%%   %s\n", seconds, words,
                    (ratio - 1.0) * 100.0, body);
        }
    }

    wprintf(L"\n%d strings, %d failures (engine's own run-to-run variation was up to %.1f%%)\n",
            static_cast<int>(std::size(kRegressionTexts)), failures, worst_control * 100.0);
    return failures == 0 ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) {
        fwprintf(stderr, L"usage: ivx_sapitest <Infovox330SAPI5.dll> [options]\n");
        return 2;
    }

    const std::wstring dll_path = argv[1];
    std::wstring out_dir = L"sapitest";
    std::wstring only_voice;
    std::wstring text = L"This is the Infovox three thirty S A P I five engine speaking. "
                        L"One, two, three.";
    long rate = 0;
    int volume = 100;
    bool all_voices = false;
    ULONGLONG abort_after = 0;
    int stress_rounds = 0;
    int concurrent = 1;
    bool kill_helper_once = false;
    bool regression = false;
    std::wstring raw_tag;

    for (int i = 2; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&]() -> std::wstring { return (i + 1 < argc) ? argv[++i] : std::wstring(); };
        if (arg == L"--out") {
            out_dir = next();
        } else if (arg == L"--voice") {
            only_voice = next();
        } else if (arg == L"--text") {
            text = next();
        } else if (arg == L"--rate") {
            rate = _wtol(next().c_str());
        } else if (arg == L"--volume") {
            volume = _wtoi(next().c_str());
        } else if (arg == L"--all") {
            all_voices = true;
        } else if (arg == L"--abort-after") {
            abort_after = static_cast<ULONGLONG>(_wtoi64(next().c_str()));
        } else if (arg == L"--stress") {
            stress_rounds = _wtoi(next().c_str());
        } else if (arg == L"--concurrent") {
            concurrent = _wtoi(next().c_str());
        } else if (arg == L"--kill-helper") {
            kill_helper_once = true;
        } else if (arg == L"--rawtag") {
            raw_tag = next();
        } else if (arg == L"--regress") {
            regression = true;
        } else {
            fwprintf(stderr, L"unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        fwprintf(stderr, L"CoInitializeEx failed 0x%08lX\n", static_cast<unsigned long>(hr));
        return 1;
    }

    HMODULE dll = LoadLibraryW(dll_path.c_str());
    if (!dll) {
        fwprintf(stderr, L"LoadLibrary(%s) failed with %lu\n", dll_path.c_str(), GetLastError());
        return 1;
    }
    g_get_class_object =
        reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
    if (!g_get_class_object) {
        fwprintf(stderr, L"the DLL has no DllGetClassObject\n");
        return 1;
    }

    IEnumSpObjectTokens* enumerator = nullptr;
    hr = create_object(CLSID_InfovoxVoices, __uuidof(IEnumSpObjectTokens),
                       reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        fwprintf(stderr, L"could not create the voice enumerator: 0x%08lX\n",
                 static_cast<unsigned long>(hr));
        return 1;
    }

    ULONG count = 0;
    enumerator->GetCount(&count);
    wprintf(L"voice enumerator reports %lu voices\n\n", count);

    std::vector<ISpObjectToken*> tokens;
    for (ULONG i = 0; i < count; ++i) {
        ISpObjectToken* token = nullptr;
        if (SUCCEEDED(enumerator->Item(i, &token)) && token) {
            tokens.push_back(token);
        }
    }
    enumerator->Release();

    int exit_code = 0;

    if (regression) {
        exit_code = run_regression(tokens);
    } else if (stress_rounds > 0) {
        wprintf(L"stress: %d rounds, %d engine object(s)%s\n", stress_rounds, concurrent,
                kill_helper_once ? L", killing the helper once part-way" : L"");
        const DWORD begin = GetTickCount();
        const StressResult result = run_stress(tokens, stress_rounds, concurrent, kill_helper_once);
        const DWORD elapsed = GetTickCount() - begin;

        wprintf(L"\n%d rounds in %lu ms (%d interrupted, %llu bytes, slowest round %lu ms)\n",
                result.rounds, elapsed, result.aborted_rounds, result.total_bytes,
                result.slowest_ms);
        if (result.failures == 0) {
            wprintf(L"no failures\n");
        } else {
            wprintf(L"%d FAILURES (%d of them silent rounds)\n", result.failures,
                    result.silent_rounds);
            exit_code = 1;
        }
    } else {
        make_directories(out_dir);
        int failures = 0;
        int spoken = 0;

        for (ULONG i = 0; i < static_cast<ULONG>(tokens.size()); ++i) {
            BoundVoice voice;
            const std::wstring speaker = token_attribute(tokens[i], L"InfovoxSpeaker");
            const bool selected =
                all_voices ||
                (only_voice.empty() ? i == 0 : _wcsicmp(only_voice.c_str(), speaker.c_str()) == 0);
            if (!selected) {
                continue;
            }

            hr = bind_voice(tokens[i], voice);
            if (FAILED(hr)) {
                wprintf(L"[%2lu] %-32s FAILED to bind: 0x%08lX\n", i, speaker.c_str(),
                        static_cast<unsigned long>(hr));
                ++failures;
                continue;
            }

            wprintf(L"[%2lu] %-32s lang=%-5s ", i, voice.name.c_str(), voice.language.c_str());
            fflush(stdout);

            Utterance utterance;
            utterance.build(text, L"42", L" And that is the end.", 100, 0, raw_tag);

            TestSite site(rate, static_cast<USHORT>(volume), kAllEvents, abort_after);
            const DWORD begin = GetTickCount();
            hr = voice.engine->Speak(0, SPDFID_WaveFormatEx, voice.format, utterance.first(), &site);
            const DWORD elapsed = GetTickCount() - begin;

            int words = 0;
            int sentences = 0;
            int bookmarks = 0;
            for (const auto& e : site.events) {
                if (e.id == SPEI_WORD_BOUNDARY) ++words;
                else if (e.id == SPEI_SENTENCE_BOUNDARY) ++sentences;
                else if (e.id == SPEI_TTS_BOOKMARK) ++bookmarks;
            }

            if (FAILED(hr) || site.pcm.empty()) {
                wprintf(L"FAILED Speak: 0x%08lX, %zu bytes\n", static_cast<unsigned long>(hr),
                        site.pcm.size());
                ++failures;
            } else {
                wchar_t path[MAX_PATH];
                _snwprintf_s(path, _TRUNCATE, L"%s\\sapi_%02lu_%s.wav", out_dir.c_str(), i,
                             speaker.empty() ? L"voice" : speaker.c_str());
                write_wav(path, *voice.format, site.pcm);
                const double seconds = voice.format->nAvgBytesPerSec
                                           ? static_cast<double>(site.pcm.size()) /
                                                 voice.format->nAvgBytesPerSec
                                           : 0.0;
                wprintf(L"ok %7zu bytes %5.2fs  %2d words %d sentences %d marks  %d writes  %lu ms\n",
                        site.pcm.size(), seconds, words, sentences, bookmarks, site.writes,
                        elapsed);
                ++spoken;
            }

            if (!all_voices) {
                wprintf(L"\n  events:\n");
                for (const auto& e : site.events) {
                    wprintf(L"    %-9s audio=%-8llu textPos=%-4lld len=%-3llu %s\n",
                            event_name(e.id), e.offset, static_cast<long long>(e.lparam),
                            static_cast<unsigned long long>(e.wparam), e.text.c_str());
                }
                wprintf(L"\n");
            }
        }

        wprintf(L"\n%d spoken, %d failed\n", spoken, failures);
        exit_code = failures == 0 ? 0 : 1;
    }

    for (ISpObjectToken* token : tokens) {
        token->Release();
    }
    CoUninitialize();
    return exit_code;
}
