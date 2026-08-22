// Renders a spoken sample for every Infovox 330 voice to a WAV file.
//
// This is the proof that the engine speaks with no SAPI 4 runtime and no registry: it walks
// the voice catalogue from VoiceDescriptions.txt, drives the engine through the same
// ivx::Engine the SAPI 5 wrapper uses, and writes what comes back.
//
//   ivx_render.exe [--data <root>] [--out <dir>] [--voice <name>] [--text "..."]
//                  [--rate <0-100>] [--pitch <0-100>] [--volume <0-100>] [--combined]

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ivx_engine.hpp"
#include "ivx_log.hpp"
#include "ivx_paths.hpp"
#include "ivx_voices.hpp"

namespace {

struct Sample {
    WORD language_id;
    const wchar_t* code;
    const wchar_t* sentence;  // %s is replaced with the speaker's name
};

// One sentence per language, each naming its own voice so the file is self-identifying.
constexpr Sample kSamples[] = {
    {6,    L"da", L"Goddag. Jeg hedder %s, og jeg er en dansk stemme fra Infovox 330. "
                  L"En, to, tre, fire, fem."},
    {19,   L"nl", L"Goedendag. Ik heet %s en ik ben een Nederlandse stem van Infovox 330. "
                  L"Een, twee, drie, vier, vijf."},
    {1033, L"en-US", L"Hello. My name is %s, and I am an American English voice from "
                     L"Infovox 330. One, two, three, four, five."},
    {2057, L"en-GB", L"Good afternoon. My name is %s, and I am a British English voice from "
                     L"Infovox 330. One, two, three, four, five."},
    {11,   L"fi", L"Hyvää päivää. Nimeni on %s, ja olen "
                  L"suomenkielinen ääni Infovox 330:ssä. "
                  L"Yksi, kaksi, kolme, neljä, viisi."},
    {12,   L"fr", L"Bonjour. Je m'appelle %s, et je suis une voix française d'Infovox 330. "
                  L"Un, deux, trois, quatre, cinq."},
    {7,    L"de", L"Guten Tag. Ich heiße %s, und ich bin eine deutsche Stimme von "
                  L"Infovox 330. Eins, zwei, drei, vier, fünf."},
    {15,   L"is", L"Góðan daginn. Ég heiti %s og ég er íslensk rödd "
                  L"frá Infovox 330. Einn, tveir, þrir, fjórir, fimm."},
    {16,   L"it", L"Buongiorno. Mi chiamo %s e sono una voce italiana di Infovox 330. "
                  L"Uno, due, tre, quattro, cinque."},
    {20,   L"no", L"God dag. Jeg heter %s, og jeg er en norsk stemme fra Infovox 330. "
                  L"En, to, tre, fire, fem."},
    {10,   L"es", L"Buenos días. Me llamo %s y soy una voz española de Infovox 330. "
                  L"Uno, dos, tres, cuatro, cinco."},
    {29,   L"sv", L"God dag. Jag heter %s och jag är en svensk röst från "
                  L"Infovox 330. Ett, två, tre, fyra, fem."},
};

[[nodiscard]] std::wstring sample_for(const ivx::VoiceDesc& voice)
{
    const wchar_t* sentence = nullptr;
    for (const auto& s : kSamples) {
        if (s.language_id == voice.language_id) {
            sentence = s.sentence;
            break;
        }
    }
    if (!sentence) {
        sentence = L"Hello. My name is %s. This is Infovox 330.";
    }
    std::vector<wchar_t> buf(wcslen(sentence) + voice.speaker.size() + 32);
    _snwprintf_s(buf.data(), buf.size(), _TRUNCATE, sentence, voice.speaker.c_str());
    return std::wstring(buf.data());
}

[[nodiscard]] std::wstring language_code(const ivx::VoiceDesc& voice)
{
    for (const auto& s : kSamples) {
        if (s.language_id == voice.language_id) {
            return s.code;
        }
    }
    return L"und";
}

class BufferSink : public ivx::SynthSink {
public:
    bool on_audio(const void* data, DWORD size) override
    {
        const char* p = static_cast<const char*>(data);
        pcm.insert(pcm.end(), p, p + size);
        return true;
    }

    void on_bookmark(unsigned __int64 audio_offset, DWORD id) override
    {
        bookmarks.push_back({audio_offset, id});
        wprintf(L"\n      bookmark %lu at byte %llu", id, audio_offset);
    }

    void on_word_boundary(unsigned __int64 audio_offset) override
    {
        words.push_back(audio_offset);
    }

    struct Mark {
        unsigned __int64 offset;
        DWORD id;
    };

    std::vector<char> pcm;
    std::vector<Mark> bookmarks;
    std::vector<unsigned __int64> words;
};

bool write_wav(const std::wstring& path, const WAVEFORMATEX& wfx, const std::vector<char>& pcm)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) {
        return false;
    }

    const DWORD data_size = static_cast<DWORD>(pcm.size());
    const DWORD fmt_size = 16;
    const DWORD riff_size = 4 + (8 + fmt_size) + (8 + data_size);

    auto put32 = [f](DWORD v) { fwrite(&v, 4, 1, f); };
    auto put16 = [f](WORD v) { fwrite(&v, 2, 1, f); };

    fwrite("RIFF", 1, 4, f);
    put32(riff_size);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    put32(fmt_size);
    put16(1);  // PCM
    put16(wfx.nChannels);
    put32(wfx.nSamplesPerSec);
    put32(wfx.nAvgBytesPerSec);
    put16(wfx.nBlockAlign);
    put16(wfx.wBitsPerSample);
    fwrite("data", 1, 4, f);
    put32(data_size);
    if (data_size > 0) {
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

[[nodiscard]] const GUID* mode_for_voice(const std::vector<ivx::ModeInfo>& modes,
                                         const ivx::VoiceDesc& voice)
{
    for (const auto& m : modes) {
        if (IsEqualGUID(m.mode_guid, voice.mode_guid)) {
            return &m.mode_guid;
        }
    }
    // Fall back to matching on the speaker name, in case a voice file and the engine's own
    // manifest disagree about the GUID.
    for (const auto& m : modes) {
        if (_wcsicmp(m.speaker.c_str(), voice.speaker.c_str()) == 0) {
            return &m.mode_guid;
        }
    }
    return nullptr;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    std::wstring out_dir;
    std::wstring data_dir;
    std::wstring only_voice;
    std::wstring custom_text;
    int rate = -1;
    int pitch = -1;
    int volume = -1;
    bool combined = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&]() -> std::wstring { return (i + 1 < argc) ? argv[++i] : std::wstring(); };
        if (arg == L"--out") {
            out_dir = next();
        } else if (arg == L"--data") {
            data_dir = next();
        } else if (arg == L"--voice") {
            only_voice = next();
        } else if (arg == L"--text") {
            custom_text = next();
        } else if (arg == L"--rate") {
            rate = _wtoi(next().c_str());
        } else if (arg == L"--pitch") {
            pitch = _wtoi(next().c_str());
        } else if (arg == L"--volume") {
            volume = _wtoi(next().c_str());
        } else if (arg == L"--combined") {
            combined = true;
        } else {
            fwprintf(stderr, L"unknown argument: %s\n", arg.c_str());
            return 2;
        }
    }

    ivx::log_init(L"render");

    if (!data_dir.empty()) {
        ivx::set_data_root(data_dir);
    }
    if (out_dir.empty()) {
        out_dir = L"samples";
    }
    make_directories(out_dir);

    wprintf(L"data root : %s\n", ivx::data_root().c_str());
    wprintf(L"output    : %s\n", out_dir.c_str());
    wprintf(L"log       : %s\n\n", ivx::log_file_path().c_str());

    const auto& catalogue = ivx::voice_catalogue();
    if (catalogue.empty()) {
        fwprintf(stderr, L"No voices found. Check the data root.\n");
        return 1;
    }

    ivx::Engine& engine = ivx::Engine::instance();
    const HRESULT hr = engine.ensure_initialised();
    if (FAILED(hr)) {
        fwprintf(stderr, L"Engine initialisation failed: 0x%08lX. See %s\n",
                 static_cast<unsigned long>(hr), ivx::log_file_path().c_str());
        return 1;
    }

    const std::vector<ivx::ModeInfo> modes = engine.modes();
    wprintf(L"engine reports %zu modes, catalogue lists %zu voices\n\n", modes.size(),
            catalogue.size());

    std::vector<char> all_pcm;
    WAVEFORMATEX combined_format{};
    int failures = 0;
    int rendered = 0;
    int index = 0;

    for (const auto& voice : catalogue) {
        ++index;
        if (!only_voice.empty() && _wcsicmp(only_voice.c_str(), voice.speaker.c_str()) != 0) {
            continue;
        }

        const GUID* mode = mode_for_voice(modes, voice);
        wprintf(L"[%2d/%2zu] %-9s %-18s ", index, catalogue.size(), voice.speaker.c_str(),
                voice.language_name.c_str());
        fflush(stdout);

        if (!mode) {
            wprintf(L"SKIPPED - the engine does not offer this mode\n");
            ++failures;
            continue;
        }

        ivx::VoiceRanges ranges;
        engine.get_ranges(*mode, ranges);

        ivx::SpeakParams params;
        params.mode = *mode;
        params.text = custom_text.empty() ? sample_for(voice) : custom_text;
        params.speed = rate >= 0 && ranges.speed.supported
                           ? static_cast<int>(ranges.speed.from_percent(rate))
                           : -1;
        params.pitch = pitch >= 0 && ranges.pitch.supported
                           ? static_cast<int>(ranges.pitch.from_percent(pitch))
                           : -1;
        params.volume = volume >= 0 && ranges.volume.supported
                            ? static_cast<int>(ranges.volume.from_percent(volume))
                            : -1;

        BufferSink sink;
        const DWORD begin = GetTickCount();
        const HRESULT speak_hr = engine.speak(params, sink);
        const DWORD elapsed = GetTickCount() - begin;

        WAVEFORMATEX wfx{};
        engine.output_format(wfx);

        if (FAILED(speak_hr) || sink.pcm.empty()) {
            wprintf(L"FAILED (hr=0x%08lX, %zu bytes)\n", static_cast<unsigned long>(speak_hr),
                    sink.pcm.size());
            ++failures;
            continue;
        }

        wchar_t name[MAX_PATH];
        _snwprintf_s(name, _TRUNCATE, L"%s\\%02d_%s_%s.wav", out_dir.c_str(), index,
                     language_code(voice).c_str(), voice.speaker.c_str());

        if (!write_wav(name, wfx, sink.pcm)) {
            wprintf(L"FAILED to write %s\n", name);
            ++failures;
            continue;
        }

        const double seconds =
            wfx.nAvgBytesPerSec ? static_cast<double>(sink.pcm.size()) / wfx.nAvgBytesPerSec : 0.0;
        wprintf(L"ok  %7zu bytes  %5.2f s  %2zu words %2zu marks  (%lu ms, %lu Hz)\n",
                sink.pcm.size(), seconds, sink.words.size(), sink.bookmarks.size(), elapsed,
                wfx.nSamplesPerSec);
        ++rendered;

        if (combined) {
            if (all_pcm.empty()) {
                combined_format = wfx;
            }
            all_pcm.insert(all_pcm.end(), sink.pcm.begin(), sink.pcm.end());
            // Half a second of silence between voices.
            all_pcm.insert(all_pcm.end(), wfx.nAvgBytesPerSec / 2, 0);
        }
    }

    if (combined && !all_pcm.empty()) {
        const std::wstring path = out_dir + L"\\00_all_voices.wav";
        if (write_wav(path, combined_format, all_pcm)) {
            wprintf(L"\ncombined: %s (%zu bytes)\n", path.c_str(), all_pcm.size());
        }
    }

    wprintf(L"\n%d rendered, %d failed\n", rendered, failures);
    engine.shutdown();
    ivx::log_shutdown();
    return failures == 0 ? 0 : 1;
}
