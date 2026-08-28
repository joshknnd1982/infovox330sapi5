// Finds out what the engine actually responds to, rather than what the SAPI 4 documentation
// says it should.
//
// Every parameter the configuration utility offers was measured with this: a fixed sentence
// is spoken once as a baseline and again with one thing changed, and the two waveforms are
// compared. If nothing moves, the engine ignored the setting and there is no point offering
// it. That is how the volume attribute was found to do nothing, and it is the only honest
// way to answer "does \Chr=Whisper\ work".
//
//   ivx_probe.exe [--data <root>] [--voice Larry] [--modes] [--tags] [--attrs] [--stats]
//
// With no switch it runs modes, attributes and tags.
//
// Two things make a naive comparison lie, and both are handled here:
//
//   * The engine does not produce byte-identical audio for identical input. Successive
//     renderings of the same sentence differ by a few tens of bytes of trailing buffer and
//     a handful of samples. The baseline is therefore rendered several times first and the
//     spread of those runs becomes the noise floor every trial is judged against.
//
//   * Tagged prosody persists across utterances. "\Spd=90\" in one TextData call is still
//     in force for the next one, so a table of trials run back to back measures the tag
//     before it and not the tag under test. Every trial therefore starts from an explicit
//     attribute reset and a "\Rst\" tag.

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ivx_engine.hpp"
#include "ivx_log.hpp"
#include "ivx_paths.hpp"
#include "ivx_voices.hpp"

namespace {

constexpr wchar_t kDefaultText[] =
    L"Hello there. This is a test of one two three, and a longer sentence so that any "
    L"change in the voice has room to show itself.";

class BufferSink : public ivx::SynthSink {
public:
    bool on_audio(const void* data, DWORD size) override
    {
        const char* p = static_cast<const char*>(data);
        pcm.insert(pcm.end(), p, p + size);
        return true;
    }
    void on_bookmark(unsigned __int64, DWORD) override {}
    std::vector<char> pcm;
};

struct Audio {
    bool ok = false;
    std::size_t raw_size = 0;   // before the silence trim; this is what length is judged on
    std::vector<short> samples;  // trimmed, so a sample-by-sample comparison lines up

    [[nodiscard]] std::size_t size() const { return samples.size(); }

    // Loudness. Moves when the engine changes gain or voicing, not when it changes timing.
    [[nodiscard]] double rms() const
    {
        if (samples.empty()) {
            return 0.0;
        }
        double sum = 0.0;
        for (const short s : samples) {
            sum += static_cast<double>(s) * static_cast<double>(s);
        }
        return std::sqrt(sum / static_cast<double>(samples.size()));
    }

    // A crude pitch/brightness proxy: how often the waveform crosses zero per second. It
    // moves with the fundamental and with the spectral balance, which between them cover
    // every voice-shaping parameter worth testing.
    [[nodiscard]] double zero_crossing_rate() const
    {
        if (samples.size() < 2) {
            return 0.0;
        }
        std::size_t crossings = 0;
        for (std::size_t i = 1; i < samples.size(); ++i) {
            if ((samples[i - 1] < 0) != (samples[i] < 0)) {
                ++crossings;
            }
        }
        return static_cast<double>(crossings) * 16000.0 / static_cast<double>(samples.size());
    }
};

// Mean absolute sample difference over the overlapping part, as a fraction of the baseline's
// own mean absolute level. Zero means the same waveform; anything approaching 1 means the
// two have nothing in common.
[[nodiscard]] double waveform_difference(const Audio& a, const Audio& b)
{
    const std::size_t n = (a.size() < b.size()) ? a.size() : b.size();
    if (n == 0) {
        return 1.0;
    }
    double diff = 0.0;
    double level = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        diff += std::fabs(static_cast<double>(a.samples[i]) - static_cast<double>(b.samples[i]));
        level += std::fabs(static_cast<double>(a.samples[i]));
    }
    return level > 0.0 ? diff / level : 1.0;
}

struct Ranges {
    ivx::VoiceRanges v;
    bool valid = false;
};

Ranges g_ranges;

// Every render starts from the same place: the attributes explicitly at their defaults and
// a "\Rst\" so that any tag left in force by the previous utterance is dropped.
Audio render(const GUID& mode, const std::wstring& text, int speed = -1, int pitch = -1,
             int volume = -1, int realtime = -1, bool reset = true)
{
    ivx::SpeakParams params;
    params.mode = mode;
    params.text = reset ? (L"\\Rst\\ " + text) : text;
    params.speed = speed >= 0 ? speed
                              : (g_ranges.valid ? static_cast<int>(g_ranges.v.speed.default_value)
                                                : -1);
    params.pitch = pitch >= 0 ? pitch
                              : (g_ranges.valid ? static_cast<int>(g_ranges.v.pitch.default_value)
                                                : -1);
    params.volume = volume >= 0
                        ? volume
                        : (g_ranges.valid ? static_cast<int>(g_ranges.v.volume.max_value) : -1);
    params.realtime = realtime;

    BufferSink sink;
    Audio a;
    a.ok = SUCCEEDED(ivx::Engine::instance().speak(params, sink));

    std::vector<short> all(sink.pcm.size() / sizeof(short));
    a.raw_size = all.size();
    if (!all.empty()) {
        std::memcpy(all.data(), sink.pcm.data(), all.size() * sizeof(short));
    }

    // How much silence the engine puts in front of an utterance varies from run to run by
    // enough samples to throw a sample-by-sample comparison out of alignment completely,
    // which is what made the waveform metric read "entirely different" for two renderings
    // of the same sentence. Both ends are trimmed before anything is measured.
    constexpr short kSilence = 64;
    std::size_t first = 0;
    while (first < all.size() && std::abs(static_cast<int>(all[first])) <= kSilence) {
        ++first;
    }
    std::size_t last = all.size();
    while (last > first && std::abs(static_cast<int>(all[last - 1])) <= kSilence) {
        --last;
    }
    a.samples.assign(all.begin() + static_cast<std::ptrdiff_t>(first),
                     all.begin() + static_cast<std::ptrdiff_t>(last));
    return a;
}

// What the same input does to itself, run to run. Everything else is judged against this.
struct NoiseFloor {
    double length_pct = 0.0;
    double waveform = 0.0;
    double rms_pct = 0.0;
    double zcr_pct = 0.0;
};

NoiseFloor g_noise;
Audio g_baseline;

[[nodiscard]] double pct_change(double from, double to)
{
    return from != 0.0 ? (to - from) * 100.0 / from : 0.0;
}

void measure_noise_floor(const GUID& mode, const std::wstring& text, bool reset)
{
    constexpr int kRuns = 4;
    g_noise = NoiseFloor{};
    std::vector<Audio> runs;
    for (int i = 0; i < kRuns; ++i) {
        runs.push_back(render(mode, text, -1, -1, -1, -1, reset));
    }
    g_baseline = runs.front();

    for (std::size_t i = 1; i < runs.size(); ++i) {
        const double len = std::fabs(pct_change(static_cast<double>(g_baseline.raw_size),
                                                static_cast<double>(runs[i].raw_size)));
        const double wav = waveform_difference(g_baseline, runs[i]);
        const double rms = std::fabs(pct_change(g_baseline.rms(), runs[i].rms()));
        const double zcr = std::fabs(
            pct_change(g_baseline.zero_crossing_rate(), runs[i].zero_crossing_rate()));
        if (len > g_noise.length_pct) g_noise.length_pct = len;
        if (wav > g_noise.waveform) g_noise.waveform = wav;
        if (rms > g_noise.rms_pct) g_noise.rms_pct = rms;
        if (zcr > g_noise.zcr_pct) g_noise.zcr_pct = zcr;
    }

    wprintf(L"\nNoise floor over %d identical renderings, %s\n", kRuns,
            reset ? L"each preceded by a reset tag" : L"attributes set explicitly");
    wprintf(L"  length %.3f%%   waveform %.4f   loudness %.3f%%   zero-crossings %.3f%%\n",
            g_noise.length_pct, g_noise.waveform, g_noise.rms_pct, g_noise.zcr_pct);
    wprintf(L"  baseline: %zu samples (%zu after trimming), %.2f s, loudness %.0f, "
            L"zero-crossings %.0f/s\n",
            g_baseline.raw_size, g_baseline.size(),
            static_cast<double>(g_baseline.raw_size) / 16000.0, g_baseline.rms(),
            g_baseline.zero_crossing_rate());
}

// A trial counts as a real effect only when it moves something by more than three times the
// noise floor, with an absolute floor under that so a perfectly quiet measurement cannot
// make every trial look significant.
void report(const wchar_t* label, const Audio& trial)
{
    if (!trial.ok) {
        wprintf(L"  %-32s FAILED\n", label);
        return;
    }
    const double len = pct_change(static_cast<double>(g_baseline.raw_size),
                                  static_cast<double>(trial.raw_size));
    const double wav = waveform_difference(g_baseline, trial);
    const double rms = pct_change(g_baseline.rms(), trial.rms());
    const double zcr = pct_change(g_baseline.zero_crossing_rate(), trial.zero_crossing_rate());

    const auto over = [](double value, double floor, double absolute) {
        const double limit = (floor * 3.0 > absolute) ? floor * 3.0 : absolute;
        return std::fabs(value) > limit;
    };
    const bool changed = over(len, g_noise.length_pct, 0.5) ||
                         over(wav, g_noise.waveform, 0.05) ||
                         over(rms, g_noise.rms_pct, 1.0) ||
                         over(zcr, g_noise.zcr_pct, 1.0);

    wprintf(L"  %-32s %-8s len %+7.2f%%  wave %6.3f  loud %+7.2f%%  zcr %+7.2f%%\n", label,
            changed ? L"EFFECT" : L"none", len, wav, rms, zcr);
}

// ---------------------------------------------------------------------------------------
// Tagged text. Which parts of the SAPI 4 control set Infovox 330 honours is undocumented,
// so each one is spoken and compared. The last few are guesses at engine-specific tags
// named after the settings VoiceDescriptions.txt carries.
// ---------------------------------------------------------------------------------------
struct TagTrial {
    const wchar_t* label;
    const wchar_t* prefix;
    const wchar_t* suffix;
};

constexpr TagTrial kTagTrials[] = {
    {L"\\Chr=Normal\\",          L"\\Chr=Normal\\",          L""},
    {L"\\Chr=Monotone\\",        L"\\Chr=Monotone\\",        L""},
    {L"\\Chr=Whisper\\",         L"\\Chr=Whisper\\",         L""},
    {L"\\Ctx=Address\\",         L"\\Ctx=Address\\",         L""},
    {L"\\Ctx=E-mail\\",          L"\\Ctx=E-mail\\",          L""},
    {L"\\Prt=Noun\\",            L"\\Prt=Noun\\",            L""},
    {L"\\Emp\\",                 L"\\Emp\\",                 L""},
    {L"\\Vce=Gender=Female\\",   L"\\Vce=Gender=Female\\",   L""},
    {L"\\Vce=Style=Business\\",  L"\\Vce=Style=Business\\",  L""},
    {L"\\Com=hello\\",           L"\\Com=hello\\",           L""},
    {L"\\Eng=1\\",               L"\\Eng=1\\",               L""},
    {L"\\RmS=1\\ spell",         L"\\RmS=1\\",               L"\\RmS=0\\"},
    {L"\\RmW=1\\",               L"\\RmW=1\\",               L"\\RmW=0\\"},
    {L"\\Pau=500\\",             L"\\Pau=500\\",             L""},
    {L"\\Spd=250\\",             L"\\Spd=250\\",             L""},
    {L"\\Spd=90\\",              L"\\Spd=90\\",              L""},
    {L"\\Pit=60\\",              L"\\Pit=60\\",              L""},
    {L"\\Pit=200\\",             L"\\Pit=200\\",             L""},
    {L"\\Vol=16000\\",           L"\\Vol=16000\\",           L""},
    {L"\\Dyn=10\\",              L"\\Dyn=10\\",              L""},
    {L"\\Dyn=90\\",              L"\\Dyn=90\\",              L""},
    {L"\\Asp=50\\",              L"\\Asp=50\\",              L""},
    {L"\\Frm=1\\",               L"\\Frm=1\\",               L""},
    {L"\\Prn=hello=h eh l ow\\", L"\\Prn=hello=h eh l ow\\", L""},
};

void probe_tags(const GUID& mode, const std::wstring& text)
{
    wprintf(L"\nTagged text\n");
    for (const auto& trial : kTagTrials) {
        const std::wstring tagged = std::wstring(trial.prefix) + text + trial.suffix;
        report(trial.label, render(mode, tagged));
    }
}

// ---------------------------------------------------------------------------------------
// The four ITTSAttributes settings, RealTime included - nothing in the SAPI 5 layer sets
// that one, and its documented meaning has no SAPI 5 equivalent, so whether it does
// anything at all is worth knowing before offering it.
// ---------------------------------------------------------------------------------------
void probe_attrs(const GUID& mode, const std::wstring& text)
{
    const ivx::VoiceRanges& r = g_ranges.v;
    wprintf(L"\nEngine attributes\n");
    wprintf(L"  speed  supported=%s  range %lu..%lu  default %lu\n",
            r.speed.supported ? L"yes" : L"no", r.speed.min_value, r.speed.max_value,
            r.speed.default_value);
    wprintf(L"  pitch  supported=%s  range %lu..%lu  default %lu\n",
            r.pitch.supported ? L"yes" : L"no", r.pitch.min_value, r.pitch.max_value,
            r.pitch.default_value);
    wprintf(L"  volume supported=%s  range %lu..%lu  default %lu\n",
            r.volume.supported ? L"yes" : L"no", r.volume.min_value, r.volume.max_value,
            r.volume.default_value);
    wprintf(L"\n");

    wchar_t label[80];
    if (r.speed.supported) {
        _snwprintf_s(label, _TRUNCATE, L"SpeedSet(%lu) minimum", r.speed.min_value);
        report(label, render(mode, text, static_cast<int>(r.speed.min_value), -1, -1, -1,
                             false));
        _snwprintf_s(label, _TRUNCATE, L"SpeedSet(%lu) maximum", r.speed.max_value);
        report(label, render(mode, text, static_cast<int>(r.speed.max_value), -1, -1, -1,
                             false));
    }
    if (r.pitch.supported) {
        _snwprintf_s(label, _TRUNCATE, L"PitchSet(%lu) minimum", r.pitch.min_value);
        report(label, render(mode, text, -1, static_cast<int>(r.pitch.min_value), -1, -1,
                             false));
        _snwprintf_s(label, _TRUNCATE, L"PitchSet(%lu) maximum", r.pitch.max_value);
        report(label, render(mode, text, -1, static_cast<int>(r.pitch.max_value), -1, -1,
                             false));
    }
    if (r.volume.supported) {
        _snwprintf_s(label, _TRUNCATE, L"VolumeSet(%lu) minimum", r.volume.min_value);
        report(label,
               render(mode, text, -1, -1, static_cast<int>(r.volume.min_value), -1, false));
        _snwprintf_s(label, _TRUNCATE, L"VolumeSet(%lu) half", r.volume.max_value / 2);
        report(label,
               render(mode, text, -1, -1, static_cast<int>(r.volume.max_value / 2), -1, false));
    }
    for (const int rt : {0, 1, 50, 1000}) {
        _snwprintf_s(label, _TRUNCATE, L"RealTimeSet(%d)", rt);
        report(label, render(mode, text, -1, -1, -1, rt, false));
    }
}

// ---------------------------------------------------------------------------------------
// The mode list as the engine reports it, next to the file it is supposed to have been
// built from. A section in VoiceDescriptions.txt that does not appear as a mode is a
// section the engine will not speak with, whatever the file says.
// ---------------------------------------------------------------------------------------
void probe_modes()
{
    const auto modes = ivx::Engine::instance().modes();
    wprintf(L"\nEngine modes (%zu)\n", modes.size());
    for (const auto& m : modes) {
        wchar_t guid[64] = {0};
        StringFromGUID2(m.mode_guid, guid, 64);
        wprintf(L"  %s  speaker='%s' lang=%u gender=%u age=%u features=0x%lX\n", guid,
                m.speaker.c_str(), m.language_id, m.gender, m.age, m.features);
    }

    const auto& catalogue = ivx::voice_catalogue();
    wprintf(L"\nVoiceDescriptions.txt sections (%zu)\n", catalogue.size());
    for (const auto& v : catalogue) {
        bool in_engine = false;
        for (const auto& m : modes) {
            if (IsEqualGUID(m.mode_guid, v.mode_guid)) {
                in_engine = true;
                break;
            }
        }
        wprintf(L"  %-14s %-20s %s\n", v.speaker.c_str(), v.language_name.c_str(),
                in_engine ? L"is an engine mode" : L"NOT AN ENGINE MODE");
    }
}

// Printed so two runs against different VoiceDescriptions.txt files can be compared. The
// engine is not bit-exact run to run, so this reports the measurements that survive that:
// length, loudness and zero-crossing rate.
void probe_stats(const GUID& mode, const std::wstring& text, bool reset)
{
    wprintf(L"text as received: [%s]\n", text.c_str());
    const Audio a = render(mode, text, -1, -1, -1, -1, reset);
    wprintf(L"stats samples=%zu trimmed=%zu seconds=%.3f loudness=%.1f zcr=%.1f ok=%s\n",
            a.raw_size, a.size(), static_cast<double>(a.raw_size) / 16000.0, a.rms(),
            a.zero_crossing_rate(), a.ok ? L"yes" : L"no");
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    std::wstring voice_name;
    std::wstring text = kDefaultText;
    bool do_modes = false;
    bool do_tags = false;
    bool do_attrs = false;
    bool do_stats = false;
    bool raw = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--data" && i + 1 < argc) {
            ivx::set_data_root(argv[++i]);
        } else if (arg == L"--voice" && i + 1 < argc) {
            voice_name = argv[++i];
        } else if (arg == L"--text" && i + 1 < argc) {
            text = argv[++i];
        } else if (arg == L"--modes") {
            do_modes = true;
        } else if (arg == L"--tags") {
            do_tags = true;
        } else if (arg == L"--attrs") {
            do_attrs = true;
        } else if (arg == L"--stats") {
            do_stats = true;
        } else if (arg == L"--raw") {
            raw = true;  // speak the text exactly as given, with no reset tag in front
        }
    }
    if (!do_modes && !do_tags && !do_attrs && !do_stats) {
        do_modes = do_tags = do_attrs = true;
    }

    ivx::log_init(L"probe");

    if (FAILED(ivx::Engine::instance().ensure_initialised())) {
        wprintf(L"the engine could not be initialised; see the log\n");
        return 1;
    }

    if (do_modes) {
        probe_modes();
    }

    if (do_tags || do_attrs || do_stats) {
        const ivx::VoiceDesc* voice = nullptr;
        if (!voice_name.empty()) {
            voice = ivx::find_voice(voice_name);
            if (!voice) {
                wprintf(L"no voice named '%s'\n", voice_name.c_str());
                return 1;
            }
        } else {
            const auto& catalogue = ivx::voice_catalogue();
            if (catalogue.empty()) {
                wprintf(L"the voice catalogue is empty\n");
                return 1;
            }
            voice = &catalogue.front();
        }

        g_ranges.valid = SUCCEEDED(ivx::Engine::instance().get_ranges(voice->mode_guid,
                                                                      g_ranges.v));

        if (do_stats) {
            probe_stats(voice->mode_guid, text, !raw);
        }
        if (do_tags || do_attrs) {
            wprintf(L"\nprobing with voice '%s' (%s)\n", voice->speaker.c_str(),
                    voice->language_name.c_str());
            if (do_attrs) {
                // Attributes first, and with no reset tag: \Rst\ clears the very settings
                // under test, and nothing has used a tag yet, so there is nothing in force
                // that would need clearing.
                measure_noise_floor(voice->mode_guid, text, false);
                probe_attrs(voice->mode_guid, text);
            }
            if (do_tags) {
                measure_noise_floor(voice->mode_guid, text, true);
                probe_tags(voice->mode_guid, text);
            }
        }
    }

    ivx::Engine::instance().shutdown();
    return 0;
}
