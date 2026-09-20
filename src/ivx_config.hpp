// User configuration: engine settings, per-voice overrides, and user-defined voices.
//
// Everything here is read by the SAPI 5 engine DLL (both architectures) and written by the
// configuration utility. The file lives beside the logs, in
//
//     %LOCALAPPDATA%\Infovox330 SAPI5\config.ini
//
// which is deliberately per-user and needs no administrator rights: the engine is loaded
// into whatever application is speaking, so it is already running as the person whose
// settings these are. INFOVOX330_CONFIG overrides the path.
//
// What the engine can actually be told is narrower than SAPI 4 suggests, and the shape of
// this file follows the measurements in tools/ivx_probe.cpp rather than the documentation:
// speed and pitch are real, volume is applied in software because the engine ignores its
// own volume attribute, and the per-voice keys in VoiceDescriptions.txt change nothing.
// A user-defined voice is therefore a SAPI 5 voice - its own name, its own defaults, its
// own text substitutions - in front of one of the sixteen engine voices.

#pragma once

#include <windows.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ivx {

// Case-insensitive ordering, so a voice named "larry" finds the settings saved for "Larry".
struct ci_less {
    bool operator()(const std::wstring& a, const std::wstring& b) const
    {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};

// A whole-word text replacement applied before the text reaches the engine. This is the
// only pronunciation control available: the engine ignores SAPI 4's \Prn\ tag, and there is
// no mapping between SAPI 5 phoneme ids and this engine's phoneme set.
struct Substitution {
    std::wstring from;
    std::wstring to;
};

// Everything adjustable about one voice, whether it is one of the sixteen or a user-defined
// one built on top of one of them.
struct VoiceSettings {
    // Engine-native prosody. -1 means "leave the engine's own default for this voice
    // alone", which is what an untouched voice stores, so that a future engine or a
    // different voice is not pinned to a number that came from somewhere else.
    int rate = -1;   // words per minute, inside the engine's own speed range
    int pitch = -1;  // engine pitch value, inside the engine's own pitch range

    // Applied to the samples, not by the engine: the engine accepts a volume and then
    // produces byte-identical audio whatever it is set to. Percent, and allowed above 100
    // because software gain can do what the engine will not. Clipping is saturated.
    int volume = 100;

    // What SAPI's rate and pitch sliders reach. SAPI's -10..+10 is logarithmic, so +10
    // multiplies the voice's own rate by this factor and -10 divides by it. 3.0 and 2.0
    // are the values the engine's range was originally matched to.
    double rate_span = 3.0;
    double pitch_span = 2.0;

    // Engine tagged text placed in front of every utterance. The escape hatch for anything
    // the engine understands that this project does not model; \Spd\, \Pit\ and \Rst\ are
    // the tags measured to do something.
    std::wstring prefix;

    std::vector<Substitution> substitutions;

    // What the voice reports to SAPI. Empty means "whatever the underlying voice says".
    // A user-defined voice usually wants its base voice's language but may want to be
    // announced as, say, a different age or gender.
    std::wstring language;  // LCID in lowercase hex, e.g. "409"
    std::wstring gender;    // "Male" or "Female"
    std::wstring age;       // "Child", "Adult" or "Senior"

    [[nodiscard]] bool is_default() const;
};

// A voice the user defined. It appears to every SAPI 5 application as a voice in its own
// right, and speaks through the engine voice named by base_speaker.
struct CustomVoice {
    std::wstring name;          // unique; the SAPI 5 token name
    std::wstring display_name;  // what applications show; defaults to name
    std::wstring base_speaker;  // one of the sixteen, e.g. "Larry"
    VoiceSettings settings;
};

struct EngineSettings {
    // Word and sentence boundary events cost a mark in the text for every word. They are
    // what moves a screen reader's highlight, so they are on, but a user who does not need
    // them can have the engine see clean text instead.
    bool word_events = true;
    bool sentence_events = true;

    // Off would mean a volume slider does nothing at all, since the engine ignores volume;
    // it exists so the behaviour is visible and reversible rather than silently hard-wired.
    bool software_volume = true;

    // Show only the user's own voices in speech settings. The custom voices still speak
    // through the built-in ones; this only affects what is offered.
    bool hide_builtin = false;

    bool control_tags = true;

    // Both of these were measured to change nothing about the audio. They are here because
    // they are the remaining things the engine will accept, and hiding a control is not the
    // same as reporting that it does nothing.
    bool set_engine_volume = false;
    int realtime = -1;  // -1 never sets SAPI 4's RealTime attribute

    // Empty leaves INFOVOX330_LOG_LEVEL and the built-in default in charge.
    std::wstring log_level;

    // The sentence the configuration utility speaks when previewing. Kept here so it
    // survives a restart of the utility.
    std::wstring preview_text;
};

struct Config {
    EngineSettings engine;

    // Overrides for the sixteen built-in voices, keyed by speaker name.
    std::map<std::wstring, VoiceSettings, ci_less> voices;

    std::vector<CustomVoice> custom;

    [[nodiscard]] const CustomVoice* find_custom(const std::wstring& name) const;
    [[nodiscard]] CustomVoice* find_custom(const std::wstring& name);
};

// %LOCALAPPDATA%\Infovox330 SAPI5\config.ini, or INFOVOX330_CONFIG.
[[nodiscard]] std::wstring config_path();

// The configuration, parsed once and re-read when the file on disk changes. Applications
// that host speech run for days, so a change made in the configuration utility has to
// reach them without a restart; the check is a file timestamp and is rate-limited, so it
// costs nothing on the speaking path.
[[nodiscard]] const Config& config();

// Forces the next config() call to re-read, whatever the timestamps say.
void invalidate_config();

// Rises by one every time the configuration is re-read. The voice catalogue watches it so
// that a voice added in the utility appears without the process being restarted.
[[nodiscard]] unsigned config_generation();

// Writes the file, creating its directory. Returns false and logs on failure.
bool save_config(const Config& cfg);

// Settings for one voice by its token name: a user-defined voice's own, or the overrides
// saved against one of the sixteen. Never fails; an unconfigured voice gets the defaults.
[[nodiscard]] VoiceSettings settings_for_voice(const std::wstring& token_name);

}  // namespace ivx
