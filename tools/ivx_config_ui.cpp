// Infovox 330 Configuration - the utility that defines voices and engine settings.
//
// It writes %LOCALAPPDATA%\Infovox330 SAPI5\config.ini, which the SAPI 5 engine DLL reads
// in whatever application is speaking. Nothing here needs administrator rights and nothing
// here touches the registry.
//
// It is built 32-bit and links the engine driver directly, so the Preview button speaks
// through the real engine with the settings currently on screen, including ones that have
// not been saved yet. That matters more than it sounds: the whole point of the utility is
// choosing a voice by ear.
//
// What can be adjusted follows what tools\ivx_probe.cpp measured, not what SAPI 4
// documents. The engine has exactly two working acoustic controls - speed and pitch - and
// sixteen fixed voices; its volume attribute accepts a value and changes nothing, its
// per-voice keys in VoiceDescriptions.txt change nothing, and a section added to that file
// is not enumerated. A user-defined voice is therefore a SAPI 5 voice of the user's own -
// its own name, its own defaults, its own pronunciations, its own idea of how far an
// application's rate slider should reach - speaking through one of the sixteen. The
// settings the engine ignores are still offered, on the page that says they are ignored,
// because hiding a control is not the same as reporting what it does.
//
// Accessibility is the reason for most of the shape of this file. Every control lives in a
// standard dialog template, every input is preceded in the tab order by a label describing
// it, blocks of explanation are focusable read-only edits rather than static text, and
// anything that happens without the user pressing a key says so in the "Last message" field
// at the bottom of the window. tools\check_config_a11y.ps1 walks the whole thing through
// MSAA and fails if any focusable control would be announced as nothing but its type.

#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ivx_config.hpp"
#include "ivx_engine.hpp"
#include "ivx_log.hpp"
#include "ivx_paths.hpp"
#include "ivx_synth.hpp"
#include "ivx_voices.hpp"

#include "ivx_config_ui.h"

namespace {

// Posted by the worker thread when the engine has finished starting, when a voice's ranges
// have arrived, and when a preview has been rendered.
constexpr UINT WM_APP_ENGINE_READY = WM_APP + 1;
constexpr UINT WM_APP_RANGES_READY = WM_APP + 2;
constexpr UINT WM_APP_PREVIEW_READY = WM_APP + 3;
constexpr UINT WM_APP_STATUS = WM_APP + 4;

// Used before the engine has been asked, and if it cannot be started at all. These are the
// values this engine reports for its American English voice; they are not invented, and the
// capabilities box says plainly when they are being used as a stand-in.
constexpr DWORD kFallbackRateMin = 45;
constexpr DWORD kFallbackRateMax = 499;
constexpr DWORD kFallbackRateDefault = 150;
constexpr DWORD kFallbackPitchMin = 30;
constexpr DWORD kFallbackPitchMax = 250;
constexpr DWORD kFallbackPitchDefault = 101;

constexpr wchar_t kDefaultPreviewText[] =
    L"The quick brown fox jumps over the lazy dog. One, two, three.";

// What the probe measured, reported to the user rather than kept in a source comment.
constexpr wchar_t kEngineReport[] =
    L"Measured by speaking the same sentence twice and comparing the audio:\r\n"
    L"• Speaking rate and pitch change the speech. They are the engine's only two "
    L"working controls.\r\n"
    L"• The engine's volume accepts any value and produces identical audio, so volume "
    L"is applied to the audio here instead.\r\n"
    L"• The SAPI 4 real time attribute, and the pause, spell-out, emphasis, whisper, "
    L"context and pronunciation tags, all leave the audio unchanged.\r\n"
    L"• The sixteen voices are fixed. Editing VoiceDescriptions.txt, or adding a "
    L"section to it, changes nothing, so a voice you define here speaks through one of "
    L"them.";

std::wstring g_status_pending;
std::mutex g_status_mutex;

// ---------------------------------------------------------------------------------------
// One line in the voice list.
// ---------------------------------------------------------------------------------------
struct UiVoice {
    std::wstring token;  // the key settings are saved under
    std::wstring label;  // what the combo box shows
    std::wstring base;   // the built-in voice underneath, for a user-defined voice
    GUID mode = GUID_NULL;
    bool custom = false;
};

// ---------------------------------------------------------------------------------------
// The engine runs on its own thread so that starting it, probing a voice's ranges and
// rendering a preview never block the window. A frozen window is an accessibility problem
// as much as a usability one: a screen reader cannot read what is not responding.
// ---------------------------------------------------------------------------------------
class Worker {
public:
    void start()
    {
        thread_ = std::thread([this] { run(); });
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void post(std::function<void()> job)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

    [[nodiscard]] bool busy() const { return busy_.load(std::memory_order_acquire); }

private:
    void run()
    {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
                if (quit_ && jobs_.empty()) {
                    return;
                }
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            busy_.store(true, std::memory_order_release);
            job();
            busy_.store(false, std::memory_order_release);
        }
    }

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    bool quit_ = false;
    std::atomic<bool> busy_{false};
};

// ---------------------------------------------------------------------------------------
// Everything the dialog needs to know, in one place.
// ---------------------------------------------------------------------------------------
struct AppState {
    HWND main = nullptr;
    HWND page[3] = {nullptr, nullptr, nullptr};
    HINSTANCE instance = nullptr;

    ivx::Config cfg;          // the working copy; the file is only touched on save
    std::vector<UiVoice> voices;
    int selected = -1;        // index into voices
    bool loading = false;     // true while controls are being filled, to ignore notifications
    bool dirty = false;

    std::map<std::wstring, ivx::VoiceRanges> ranges;  // by mode GUID string
    bool engine_ready = false;
    bool engine_failed = false;

    Worker worker;
    std::vector<char> preview_wav;
    std::mutex preview_mutex;
    std::atomic<bool> preview_playing{false};
};

AppState g_app;

// ---------------------------------------------------------------------------------------
// Small helpers.
// ---------------------------------------------------------------------------------------
[[nodiscard]] std::wstring guid_key(const GUID& g)
{
    wchar_t buf[64] = {0};
    StringFromGUID2(g, buf, 64);
    return buf;
}

[[nodiscard]] std::wstring get_text(HWND parent, int id)
{
    HWND control = GetDlgItem(parent, id);
    if (!control) {
        return {};
    }
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(copied < 0 ? 0 : copied));
    return text;
}

void set_text(HWND parent, int id, const std::wstring& text)
{
    SetDlgItemTextW(parent, id, text.c_str());
}

[[nodiscard]] std::wstring trim(const std::wstring& s)
{
    std::size_t first = 0;
    while (first < s.size() && iswspace(static_cast<wint_t>(s[first]))) {
        ++first;
    }
    std::size_t last = s.size();
    while (last > first && iswspace(static_cast<wint_t>(s[last - 1]))) {
        --last;
    }
    return s.substr(first, last - first);
}

// The status line is the only way a change that happened without a keypress is announced,
// so everything that finishes in the background ends up here.
void set_status(const std::wstring& text)
{
    if (g_app.main) {
        set_text(g_app.main, IDC_STATUS, text);
    }
    IVX_LOG_I("status: %s", ivx::log_narrow(text.c_str()).c_str());
}

void post_status(const std::wstring& text)
{
    {
        std::lock_guard<std::mutex> lock(g_status_mutex);
        g_status_pending = text;
    }
    if (g_app.main) {
        PostMessageW(g_app.main, WM_APP_STATUS, 0, 0);
    }
}

void mark_dirty()
{
    if (!g_app.loading) {
        g_app.dirty = true;
    }
}

// ---------------------------------------------------------------------------------------
// The voice list, and the settings behind whichever entry is selected.
// ---------------------------------------------------------------------------------------
[[nodiscard]] ivx::VoiceSettings* settings_for(const std::wstring& token)
{
    if (ivx::CustomVoice* custom = g_app.cfg.find_custom(token)) {
        return &custom->settings;
    }
    return &g_app.cfg.voices[token];  // default-constructed; save drops untouched entries
}

[[nodiscard]] const UiVoice* current_voice()
{
    if (g_app.selected < 0 ||
        g_app.selected >= static_cast<int>(g_app.voices.size())) {
        return nullptr;
    }
    return &g_app.voices[static_cast<std::size_t>(g_app.selected)];
}

[[nodiscard]] ivx::VoiceRanges ranges_for(const GUID& mode)
{
    const auto it = g_app.ranges.find(guid_key(mode));
    if (it != g_app.ranges.end()) {
        return it->second;
    }
    ivx::VoiceRanges fallback;
    fallback.speed = {true, kFallbackRateMin, kFallbackRateMax, kFallbackRateDefault};
    fallback.pitch = {true, kFallbackPitchMin, kFallbackPitchMax, kFallbackPitchDefault};
    fallback.volume = {true, 0, 65535, 65535};
    return fallback;
}

void rebuild_voice_list()
{
    g_app.voices.clear();
    for (const auto& v : ivx::builtin_catalogue()) {
        UiVoice entry;
        entry.token = v.speaker;
        entry.label = v.speaker + L" (built in, " + v.language_name + L")";
        entry.mode = v.mode_guid;
        entry.custom = false;
        g_app.voices.push_back(std::move(entry));
    }
    for (const auto& c : g_app.cfg.custom) {
        const ivx::VoiceDesc* base = nullptr;
        for (const auto& v : ivx::builtin_catalogue()) {
            if (_wcsicmp(v.speaker.c_str(), c.base_speaker.c_str()) == 0) {
                base = &v;
                break;
            }
        }
        UiVoice entry;
        entry.token = c.name;
        entry.label = c.name + L" (yours, speaking through " +
                      (base ? base->speaker : c.base_speaker) + L")";
        entry.base = c.base_speaker;
        entry.mode = base ? base->mode_guid : GUID_NULL;
        entry.custom = true;
        g_app.voices.push_back(std::move(entry));
    }
}

void fill_voice_combo()
{
    HWND combo = GetDlgItem(g_app.page[0], IDC_VOICE_LIST);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const auto& v : g_app.voices) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(v.label.c_str()));
    }
    if (g_app.selected >= 0) {
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(g_app.selected), 0);
    }
}

// The engine's ranges for this voice, spelled out. Someone who cannot see a slider needs to
// be told what the numbers either side of the box mean.
void update_capabilities()
{
    const UiVoice* voice = current_voice();
    if (!voice) {
        return;
    }
    const ivx::VoiceRanges r = ranges_for(voice->mode);
    const bool measured = g_app.ranges.count(guid_key(voice->mode)) != 0;

    wchar_t buf[768];
    _snwprintf_s(buf, _TRUNCATE,
                 L"Speaking rate: %lu to %lu words per minute, and the engine's own rate "
                 L"for this voice is %lu.\r\n"
                 L"Pitch: %lu to %lu, and the engine's own pitch for this voice is %lu.\r\n"
                 L"Volume: the engine accepts a volume and ignores it, so volume is applied "
                 L"to the audio here.\r\n"
                 L"%s",
                 r.speed.min_value, r.speed.max_value, r.speed.default_value,
                 r.pitch.min_value, r.pitch.max_value, r.pitch.default_value,
                 measured ? L"These figures came from the engine itself."
                          : (g_app.engine_failed
                                 ? L"The engine could not be started, so these are the "
                                   L"figures it reports on a working installation rather "
                                   L"than ones read from it just now."
                                 : L"The engine is still starting; these figures will be "
                                   L"replaced by the ones it reports."));
    set_text(g_app.page[0], IDC_CAPABILITIES, buf);

    // Spin ranges and labels follow the voice, so the label always states the real limits.
    SendDlgItemMessageW(g_app.page[0], IDC_RATE_SPIN, UDM_SETRANGE32,
                        static_cast<WPARAM>(r.speed.min_value),
                        static_cast<LPARAM>(r.speed.max_value));
    SendDlgItemMessageW(g_app.page[0], IDC_PITCH_SPIN, UDM_SETRANGE32,
                        static_cast<WPARAM>(r.pitch.min_value),
                        static_cast<LPARAM>(r.pitch.max_value));

    _snwprintf_s(buf, _TRUNCATE, L"Speaking &rate, %lu to %lu:", r.speed.min_value,
                 r.speed.max_value);
    set_text(g_app.page[0], IDC_RATE_LABEL, buf);
    _snwprintf_s(buf, _TRUNCATE, L"&Pitch, %lu to %lu:", r.pitch.min_value,
                 r.pitch.max_value);
    set_text(g_app.page[0], IDC_PITCH_LABEL, buf);
}

[[nodiscard]] std::wstring substitutions_to_text(const std::vector<ivx::Substitution>& subs)
{
    std::wstring out;
    for (const auto& s : subs) {
        out += s.from + L"=" + s.to + L"\r\n";
    }
    return out;
}

[[nodiscard]] std::vector<ivx::Substitution> substitutions_from_text(const std::wstring& text)
{
    std::vector<ivx::Substitution> subs;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t eol = text.find(L'\n', pos);
        const std::wstring line =
            trim(text.substr(pos, eol == std::wstring::npos ? std::wstring::npos : eol - pos));
        pos = (eol == std::wstring::npos) ? text.size() + 1 : eol + 1;
        if (line.empty()) {
            continue;
        }
        const std::size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) {
            continue;
        }
        ivx::Substitution s;
        s.from = trim(line.substr(0, eq));
        s.to = trim(line.substr(eq + 1));
        if (!s.from.empty()) {
            subs.push_back(std::move(s));
        }
    }
    return subs;
}

// The languages the installed voices speak, offered by name rather than by number.
struct LanguageChoice {
    const wchar_t* label;
    const wchar_t* lcid;
};

constexpr LanguageChoice kLanguages[] = {
    {L"Same as the voice it speaks through", L""},
    {L"Danish", L"406"},          {L"Dutch", L"413"},
    {L"English (United States)", L"409"}, {L"English (United Kingdom)", L"809"},
    {L"Finnish", L"40b"},         {L"French", L"40c"},
    {L"German", L"407"},          {L"Icelandic", L"40f"},
    {L"Italian", L"410"},         {L"Norwegian", L"414"},
    {L"Spanish", L"40a"},         {L"Swedish", L"41d"},
};

void select_combo_by_data(HWND parent, int id, const std::wstring& value,
                          const std::vector<std::wstring>& values)
{
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (_wcsicmp(values[i].c_str(), value.c_str()) == 0) {
            SendDlgItemMessageW(parent, id, CB_SETCURSEL, i, 0);
            return;
        }
    }
    SendDlgItemMessageW(parent, id, CB_SETCURSEL, 0, 0);
}

std::vector<std::wstring> g_language_values;
const std::vector<std::wstring> g_gender_values = {L"", L"Male", L"Female"};
const std::vector<std::wstring> g_age_values = {L"", L"Child", L"Adult", L"Senior"};
const std::vector<std::wstring> g_log_values = {L"",      L"off",   L"error", L"warn",
                                                L"info",  L"debug", L"trace"};

// Reads the controls back into the working copy. Called before anything that changes which
// voice is selected, and before saving, so nothing typed is ever silently lost.
void store_current()
{
    const UiVoice* voice = current_voice();
    if (!voice || g_app.loading) {
        return;
    }
    ivx::VoiceSettings* s = settings_for(voice->token);

    const bool rate_default =
        IsDlgButtonChecked(g_app.page[0], IDC_RATE_DEFAULT) == BST_CHECKED;
    const bool pitch_default =
        IsDlgButtonChecked(g_app.page[0], IDC_PITCH_DEFAULT) == BST_CHECKED;
    s->rate = rate_default ? -1 : _wtoi(get_text(g_app.page[0], IDC_RATE).c_str());
    s->pitch = pitch_default ? -1 : _wtoi(get_text(g_app.page[0], IDC_PITCH).c_str());
    s->volume = _wtoi(get_text(g_app.page[0], IDC_VOLUME).c_str());
    if (s->volume < 0) {
        s->volume = 0;
    }
    if (s->volume > 200) {
        s->volume = 200;
    }

    s->substitutions =
        substitutions_from_text(get_text(g_app.page[1], IDC_SUBSTITUTIONS));
    s->prefix = trim(get_text(g_app.page[1], IDC_PREFIX));

    const double rate_span = _wtof(get_text(g_app.page[1], IDC_RATE_SPAN).c_str());
    const double pitch_span = _wtof(get_text(g_app.page[1], IDC_PITCH_SPAN).c_str());
    s->rate_span = rate_span > 0.0 ? rate_span : 3.0;
    s->pitch_span = pitch_span > 0.0 ? pitch_span : 2.0;

    const auto combo_value = [](HWND parent, int id, const std::vector<std::wstring>& values) {
        const LRESULT index = SendDlgItemMessageW(parent, id, CB_GETCURSEL, 0, 0);
        if (index == CB_ERR || static_cast<std::size_t>(index) >= values.size()) {
            return std::wstring();
        }
        return values[static_cast<std::size_t>(index)];
    };
    s->language = combo_value(g_app.page[1], IDC_LANGUAGE, g_language_values);
    s->gender = combo_value(g_app.page[1], IDC_GENDER, g_gender_values);
    s->age = combo_value(g_app.page[1], IDC_AGE, g_age_values);

    if (voice->custom) {
        if (ivx::CustomVoice* custom = g_app.cfg.find_custom(voice->token)) {
            custom->display_name = trim(get_text(g_app.page[0], IDC_DISPLAY_NAME));
            if (custom->display_name.empty()) {
                custom->display_name = custom->name;
            }
            const LRESULT index =
                SendDlgItemMessageW(g_app.page[0], IDC_BASE_VOICE, CB_GETCURSEL, 0, 0);
            const auto& builtin = ivx::builtin_catalogue();
            if (index != CB_ERR && static_cast<std::size_t>(index) < builtin.size()) {
                custom->base_speaker = builtin[static_cast<std::size_t>(index)].speaker;
            }
        }
    }

    g_app.cfg.engine.preview_text = get_text(g_app.page[0], IDC_PREVIEW_TEXT);
}

void load_current()
{
    const UiVoice* voice = current_voice();
    if (!voice) {
        return;
    }
    g_app.loading = true;

    const ivx::VoiceSettings* s = settings_for(voice->token);
    const ivx::VoiceRanges r = ranges_for(voice->mode);

    CheckDlgButton(g_app.page[0], IDC_RATE_DEFAULT, s->rate < 0 ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_app.page[0], IDC_PITCH_DEFAULT,
                   s->pitch < 0 ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(g_app.page[0], IDC_RATE,
                  s->rate < 0 ? r.speed.default_value : static_cast<UINT>(s->rate), FALSE);
    SetDlgItemInt(g_app.page[0], IDC_PITCH,
                  s->pitch < 0 ? r.pitch.default_value : static_cast<UINT>(s->pitch), FALSE);
    SetDlgItemInt(g_app.page[0], IDC_VOLUME, static_cast<UINT>(s->volume), FALSE);
    EnableWindow(GetDlgItem(g_app.page[0], IDC_RATE), s->rate >= 0);
    EnableWindow(GetDlgItem(g_app.page[0], IDC_PITCH), s->pitch >= 0);

    set_text(g_app.page[1], IDC_SUBSTITUTIONS, substitutions_to_text(s->substitutions));
    set_text(g_app.page[1], IDC_PREFIX, s->prefix);

    wchar_t buf[32];
    _snwprintf_s(buf, _TRUNCATE, L"%.3g", s->rate_span);
    set_text(g_app.page[1], IDC_RATE_SPAN, buf);
    _snwprintf_s(buf, _TRUNCATE, L"%.3g", s->pitch_span);
    set_text(g_app.page[1], IDC_PITCH_SPAN, buf);

    select_combo_by_data(g_app.page[1], IDC_LANGUAGE, s->language, g_language_values);
    select_combo_by_data(g_app.page[1], IDC_GENDER, s->gender, g_gender_values);
    select_combo_by_data(g_app.page[1], IDC_AGE, s->age, g_age_values);
    set_text(g_app.page[1], IDC_TEXT_FOR, voice->label);

    // A built-in voice can be adjusted but not renamed, re-based or removed: it belongs to
    // the engine. The controls that do not apply are disabled rather than hidden, so the
    // page reads the same whichever kind of voice is selected.
    if (voice->custom) {
        const ivx::CustomVoice* custom = g_app.cfg.find_custom(voice->token);
        set_text(g_app.page[0], IDC_DISPLAY_NAME,
                 custom ? custom->display_name : voice->token);
        const auto& builtin = ivx::builtin_catalogue();
        for (std::size_t i = 0; i < builtin.size(); ++i) {
            if (_wcsicmp(builtin[i].speaker.c_str(), voice->base.c_str()) == 0) {
                SendDlgItemMessageW(g_app.page[0], IDC_BASE_VOICE, CB_SETCURSEL, i, 0);
                break;
            }
        }
    } else {
        const auto& builtin = ivx::builtin_catalogue();
        std::wstring shown = voice->token;
        for (const auto& v : builtin) {
            if (_wcsicmp(v.speaker.c_str(), voice->token.c_str()) == 0) {
                shown = v.display_name();
                break;
            }
        }
        set_text(g_app.page[0], IDC_DISPLAY_NAME, shown);
        SendDlgItemMessageW(g_app.page[0], IDC_BASE_VOICE, CB_SETCURSEL,
                            static_cast<WPARAM>(g_app.selected), 0);
    }
    EnableWindow(GetDlgItem(g_app.page[0], IDC_DISPLAY_NAME), voice->custom);
    EnableWindow(GetDlgItem(g_app.page[0], IDC_BASE_VOICE), voice->custom);
    EnableWindow(GetDlgItem(g_app.page[0], IDC_VOICE_RENAME), voice->custom);
    EnableWindow(GetDlgItem(g_app.page[0], IDC_VOICE_DELETE), voice->custom);

    update_capabilities();
    g_app.loading = false;
}

// ---------------------------------------------------------------------------------------
// Preview.
// ---------------------------------------------------------------------------------------
class PreviewSink : public ivx::SynthSink {
public:
    bool on_audio(const void* data, DWORD size) override
    {
        const char* p = static_cast<const char*>(data);
        pcm.insert(pcm.end(), p, p + size);
        return !cancelled.load(std::memory_order_acquire);
    }
    void on_bookmark(unsigned __int64, DWORD) override {}
    bool should_abort() override { return cancelled.load(std::memory_order_acquire); }

    std::vector<char> pcm;
    std::atomic<bool> cancelled{false};
};

std::atomic<bool> g_preview_cancel{false};

[[nodiscard]] std::vector<char> build_wav(const std::vector<char>& pcm,
                                          const WAVEFORMATEX& wfx)
{
    std::vector<char> wav;
    const auto append = [&wav](const void* data, std::size_t size) {
        const char* p = static_cast<const char*>(data);
        wav.insert(wav.end(), p, p + size);
    };
    const DWORD data_size = static_cast<DWORD>(pcm.size());
    const DWORD riff_size = 36 + data_size;
    const DWORD fmt_size = 16;
    const WORD format_tag = WAVE_FORMAT_PCM;

    append("RIFF", 4);
    append(&riff_size, 4);
    append("WAVEfmt ", 8);
    append(&fmt_size, 4);
    append(&format_tag, 2);
    append(&wfx.nChannels, 2);
    append(&wfx.nSamplesPerSec, 4);
    append(&wfx.nAvgBytesPerSec, 4);
    append(&wfx.nBlockAlign, 2);
    append(&wfx.wBitsPerSample, 2);
    append("data", 4);
    append(&data_size, 4);
    if (!pcm.empty()) {
        append(pcm.data(), pcm.size());
    }
    return wav;
}

void stop_preview()
{
    g_preview_cancel.store(true, std::memory_order_release);
    ivx::Engine::instance().abort_current();
    PlaySoundW(nullptr, nullptr, 0);
    g_app.preview_playing.store(false, std::memory_order_release);
}

void start_preview()
{
    const UiVoice* voice = current_voice();
    if (!voice) {
        return;
    }
    if (g_app.engine_failed) {
        set_status(L"The engine could not be started, so nothing can be spoken. The log "
                   L"folder button on the Speech engine page has the details.");
        return;
    }
    if (g_app.worker.busy()) {
        set_status(L"Still working on the previous request; try again in a moment.");
        return;
    }

    store_current();
    const ivx::VoiceSettings settings = *settings_for(voice->token);
    const GUID mode = voice->mode;
    std::wstring text = get_text(g_app.page[0], IDC_PREVIEW_TEXT);
    if (trim(text).empty()) {
        text = kDefaultPreviewText;
        set_text(g_app.page[0], IDC_PREVIEW_TEXT, text);
    }

    stop_preview();
    g_preview_cancel.store(false, std::memory_order_release);
    set_status(L"Speaking the preview...");

    g_app.worker.post([mode, settings, text] {
        if (FAILED(ivx::Engine::instance().ensure_initialised())) {
            post_status(L"The engine could not be started, so the preview was not spoken.");
            return;
        }
        ivx::VoiceRanges ranges;
        if (FAILED(ivx::Engine::instance().get_ranges(mode, ranges))) {
            post_status(L"The engine would not report this voice's ranges.");
            return;
        }

        // The same arithmetic the SAPI 5 layer uses, at SAPI's neutral rate and pitch, so
        // the preview is what an application will hear with its own controls centred.
        ivx::SpeakParams params;
        params.mode = mode;
        params.speed = static_cast<int>(ranges.speed.clamped(settings.rate));
        params.pitch = static_cast<int>(ranges.pitch.clamped(settings.pitch));
        params.volume = static_cast<int>(ranges.volume.max_value);

        // Whole-word substitutions, applied the way the engine layer applies them.
        std::wstring spoken;
        std::size_t pos = 0;
        while (pos < text.size()) {
            if (iswspace(static_cast<wint_t>(text[pos]))) {
                spoken.push_back(text[pos]);
                ++pos;
                continue;
            }
            const std::size_t start = pos;
            while (pos < text.size() && !iswspace(static_cast<wint_t>(text[pos]))) {
                ++pos;
            }
            std::wstring run = text.substr(start, pos - start);
            std::size_t word_start = 0;
            while (word_start < run.size() && !iswalnum(static_cast<wint_t>(run[word_start]))) {
                ++word_start;
            }
            std::size_t word_end = run.size();
            while (word_end > word_start &&
                   !iswalnum(static_cast<wint_t>(run[word_end - 1]))) {
                --word_end;
            }
            const std::wstring word = run.substr(word_start, word_end - word_start);
            for (const auto& sub : settings.substitutions) {
                if (!word.empty() && _wcsicmp(sub.from.c_str(), word.c_str()) == 0) {
                    run = run.substr(0, word_start) + sub.to + run.substr(word_end);
                    break;
                }
            }
            // Tagged text treats a backslash as the start of a command.
            for (const wchar_t c : run) {
                if (c == L'\\') {
                    spoken.append(L"\\\\");
                } else {
                    spoken.push_back(c);
                }
            }
        }

        params.text = settings.prefix.empty() ? spoken : settings.prefix + L" " + spoken;

        PreviewSink sink;
        const HRESULT hr = ivx::Engine::instance().speak(params, sink);
        if (g_preview_cancel.load(std::memory_order_acquire)) {
            return;
        }
        if (FAILED(hr) || sink.pcm.empty()) {
            post_status(L"The engine did not produce any audio for the preview.");
            return;
        }

        WAVEFORMATEX wfx{};
        ivx::Engine::instance().output_format(wfx);
        if (wfx.nSamplesPerSec == 0) {
            ivx::fill_expected_format(wfx);
        }
        if (settings.volume != 100) {
            ivx::apply_volume(sink.pcm.data(), sink.pcm.size(), settings.volume,
                              wfx.wBitsPerSample);
        }

        {
            std::lock_guard<std::mutex> lock(g_app.preview_mutex);
            g_app.preview_wav = build_wav(sink.pcm, wfx);
        }
        if (g_app.main) {
            PostMessageW(g_app.main, WM_APP_PREVIEW_READY, 0, 0);
        }
    });
}

// ---------------------------------------------------------------------------------------
// Asking for a voice name.
// ---------------------------------------------------------------------------------------
struct NamePrompt {
    std::wstring caption;
    std::wstring explanation;
    std::wstring value;
};

INT_PTR CALLBACK name_prompt_proc(HWND dlg, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* prompt = reinterpret_cast<NamePrompt*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));

    switch (message) {
        case WM_INITDIALOG: {
            prompt = reinterpret_cast<NamePrompt*>(lparam);
            SetWindowLongPtrW(dlg, GWLP_USERDATA, lparam);
            SetWindowTextW(dlg, prompt->caption.c_str());
            set_text(dlg, IDC_PROMPT_LABEL, prompt->explanation);
            set_text(dlg, IDC_PROMPT_EDIT, prompt->value);
            SendDlgItemMessageW(dlg, IDC_PROMPT_EDIT, EM_SETSEL, 0, -1);
            SetFocus(GetDlgItem(dlg, IDC_PROMPT_EDIT));
            return FALSE;  // focus was set explicitly
        }

        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case IDOK:
                    if (prompt) {
                        prompt->value = trim(get_text(dlg, IDC_PROMPT_EDIT));
                    }
                    EndDialog(dlg, IDOK);
                    return TRUE;
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
                default:
                    break;
            }
            break;

        default:
            break;
    }
    return FALSE;
}

[[nodiscard]] bool ask_for_name(HWND parent, const std::wstring& caption,
                                const std::wstring& explanation, std::wstring& value)
{
    NamePrompt prompt{caption, explanation, value};
    const INT_PTR result = DialogBoxParamW(g_app.instance, MAKEINTRESOURCEW(IDD_NAME_PROMPT),
                                           parent, name_prompt_proc,
                                           reinterpret_cast<LPARAM>(&prompt));
    if (result != IDOK) {
        return false;
    }
    value = prompt.value;
    return true;
}

// A name has to be usable as a SAPI token name and must not collide with a voice that is
// already on offer, or Windows would have two voices claiming one identity.
[[nodiscard]] bool name_is_available(const std::wstring& name, const std::wstring& except)
{
    if (name.empty()) {
        return false;
    }
    if (name.find_first_of(L"\\/[]=") != std::wstring::npos) {
        return false;
    }
    for (const auto& v : g_app.voices) {
        if (_wcsicmp(v.token.c_str(), except.c_str()) == 0) {
            continue;
        }
        if (_wcsicmp(v.token.c_str(), name.c_str()) == 0) {
            return false;
        }
    }
    return true;
}

void select_voice(const std::wstring& token)
{
    for (std::size_t i = 0; i < g_app.voices.size(); ++i) {
        if (_wcsicmp(g_app.voices[i].token.c_str(), token.c_str()) == 0) {
            g_app.selected = static_cast<int>(i);
            SendDlgItemMessageW(g_app.page[0], IDC_VOICE_LIST, CB_SETCURSEL, i, 0);
            load_current();
            return;
        }
    }
}

void new_voice()
{
    const UiVoice* voice = current_voice();
    if (!voice) {
        return;
    }
    store_current();

    const std::wstring base = voice->custom ? voice->base : voice->token;
    std::wstring name = L"My " + base;
    for (int suffix = 2; !name_is_available(name, L""); ++suffix) {
        wchar_t buf[64];
        _snwprintf_s(buf, _TRUNCATE, L"My %s %d", base.c_str(), suffix);
        name = buf;
    }

    if (!ask_for_name(g_app.main, L"New voice",
                      L"The new voice will speak through " + base +
                          L" and start with the settings of " + voice->token + L".",
                      name)) {
        return;
    }
    if (!name_is_available(name, L"")) {
        MessageBoxW(g_app.main,
                    L"That name is already used by another voice, or contains a character "
                    L"that cannot appear in a voice name. Choose a different name.",
                    L"Name already in use", MB_OK | MB_ICONWARNING);
        return;
    }

    ivx::CustomVoice custom;
    custom.name = name;
    custom.display_name = name;
    custom.base_speaker = base;
    custom.settings = *settings_for(voice->token);
    g_app.cfg.custom.push_back(std::move(custom));

    rebuild_voice_list();
    fill_voice_combo();
    select_voice(name);
    g_app.dirty = true;
    set_status(L"Created the voice " + name + L", speaking through " + base +
               L". It appears in other applications once the settings are saved.");
}

void rename_voice()
{
    const UiVoice* voice = current_voice();
    if (!voice || !voice->custom) {
        return;
    }
    store_current();

    std::wstring name = voice->token;
    const std::wstring old_name = name;
    if (!ask_for_name(g_app.main, L"Rename voice",
                      L"Applications that are already set to use " + old_name +
                          L" will need to be pointed at the new name.",
                      name)) {
        return;
    }
    if (!name_is_available(name, old_name)) {
        MessageBoxW(g_app.main,
                    L"That name is already used by another voice, or contains a character "
                    L"that cannot appear in a voice name. Choose a different name.",
                    L"Name already in use", MB_OK | MB_ICONWARNING);
        return;
    }
    if (_wcsicmp(name.c_str(), old_name.c_str()) == 0) {
        return;
    }

    ivx::CustomVoice* custom = g_app.cfg.find_custom(old_name);
    if (!custom) {
        return;
    }
    const bool display_followed_name =
        _wcsicmp(custom->display_name.c_str(), old_name.c_str()) == 0;
    custom->name = name;
    if (display_followed_name) {
        custom->display_name = name;
    }

    rebuild_voice_list();
    fill_voice_combo();
    select_voice(name);
    g_app.dirty = true;
    set_status(L"Renamed " + old_name + L" to " + name + L".");
}

void delete_voice()
{
    const UiVoice* voice = current_voice();
    if (!voice || !voice->custom) {
        return;
    }
    const std::wstring name = voice->token;

    const std::wstring question =
        L"Delete the voice " + name +
        L"? Its settings will be removed, and applications set to use it will fall back to "
        L"another voice. The built-in voice it speaks through is not affected.";
    if (MessageBoxW(g_app.main, question.c_str(), L"Delete voice",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    for (auto it = g_app.cfg.custom.begin(); it != g_app.cfg.custom.end(); ++it) {
        if (_wcsicmp(it->name.c_str(), name.c_str()) == 0) {
            g_app.cfg.custom.erase(it);
            break;
        }
    }

    g_app.selected = 0;
    rebuild_voice_list();
    fill_voice_combo();
    SendDlgItemMessageW(g_app.page[0], IDC_VOICE_LIST, CB_SETCURSEL, 0, 0);
    load_current();
    g_app.dirty = true;
    set_status(L"Deleted the voice " + name + L".");
}

// ---------------------------------------------------------------------------------------
// The engine settings page.
// ---------------------------------------------------------------------------------------
void load_engine_page()
{
    const ivx::EngineSettings& e = g_app.cfg.engine;
    CheckDlgButton(g_app.page[2], IDC_WORD_EVENTS,
                   e.word_events ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_app.page[2], IDC_SENTENCE_EVENTS,
                   e.sentence_events ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_app.page[2], IDC_SOFTWARE_VOLUME,
                   e.software_volume ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_app.page[2], IDC_HIDE_BUILTIN,
                   e.hide_builtin ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_app.page[2], IDC_CONTROL_TAGS,
                   e.control_tags ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_app.page[2], IDC_SET_ENGINE_VOLUME,
                   e.set_engine_volume ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_app.page[2], IDC_REALTIME_ENABLE,
                   e.realtime >= 0 ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(g_app.page[2], IDC_REALTIME,
                  static_cast<UINT>(e.realtime >= 0 ? e.realtime : 0), FALSE);
    EnableWindow(GetDlgItem(g_app.page[2], IDC_REALTIME), e.realtime >= 0);
    select_combo_by_data(g_app.page[2], IDC_LOG_LEVEL, e.log_level, g_log_values);
}

void store_engine_page()
{
    ivx::EngineSettings& e = g_app.cfg.engine;
    e.word_events = IsDlgButtonChecked(g_app.page[2], IDC_WORD_EVENTS) == BST_CHECKED;
    e.sentence_events = IsDlgButtonChecked(g_app.page[2], IDC_SENTENCE_EVENTS) == BST_CHECKED;
    e.software_volume = IsDlgButtonChecked(g_app.page[2], IDC_SOFTWARE_VOLUME) == BST_CHECKED;
    e.hide_builtin = IsDlgButtonChecked(g_app.page[2], IDC_HIDE_BUILTIN) == BST_CHECKED;
    e.control_tags = IsDlgButtonChecked(g_app.page[2], IDC_CONTROL_TAGS) == BST_CHECKED;
    e.set_engine_volume =
        IsDlgButtonChecked(g_app.page[2], IDC_SET_ENGINE_VOLUME) == BST_CHECKED;
    e.realtime = IsDlgButtonChecked(g_app.page[2], IDC_REALTIME_ENABLE) == BST_CHECKED
                     ? _wtoi(get_text(g_app.page[2], IDC_REALTIME).c_str())
                     : -1;
    const LRESULT index = SendDlgItemMessageW(g_app.page[2], IDC_LOG_LEVEL, CB_GETCURSEL, 0, 0);
    e.log_level = (index != CB_ERR && static_cast<std::size_t>(index) < g_log_values.size())
                      ? g_log_values[static_cast<std::size_t>(index)]
                      : std::wstring();
}

[[nodiscard]] bool save_everything()
{
    store_current();
    store_engine_page();
    if (!ivx::save_config(g_app.cfg)) {
        MessageBoxW(g_app.main,
                    L"The settings could not be written. The log folder button on the "
                    L"Speech engine page opens the folder holding the details.",
                    L"Could not save", MB_OK | MB_ICONERROR);
        return false;
    }
    g_app.dirty = false;
    set_status(L"Settings saved to " + ivx::config_path() +
               L". Applications pick them up within a second.");
    return true;
}

// ---------------------------------------------------------------------------------------
// Page procedures. Each is a child dialog inside the tab control.
// ---------------------------------------------------------------------------------------
INT_PTR CALLBACK voices_page_proc(HWND dlg, UINT message, WPARAM wparam, LPARAM)
{
    switch (message) {
        case WM_INITDIALOG: {
            HWND base = GetDlgItem(dlg, IDC_BASE_VOICE);
            for (const auto& v : ivx::builtin_catalogue()) {
                const std::wstring label = v.speaker + L" - " + v.language_name;
                SendMessageW(base, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
            SendDlgItemMessageW(dlg, IDC_VOLUME_SPIN, UDM_SETRANGE32, 0, 200);
            SendDlgItemMessageW(dlg, IDC_RATE_SPIN, UDM_SETBUDDY,
                                reinterpret_cast<WPARAM>(GetDlgItem(dlg, IDC_RATE)), 0);
            SendDlgItemMessageW(dlg, IDC_PITCH_SPIN, UDM_SETBUDDY,
                                reinterpret_cast<WPARAM>(GetDlgItem(dlg, IDC_PITCH)), 0);
            SendDlgItemMessageW(dlg, IDC_VOLUME_SPIN, UDM_SETBUDDY,
                                reinterpret_cast<WPARAM>(GetDlgItem(dlg, IDC_VOLUME)), 0);
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case IDC_VOICE_LIST:
                    if (HIWORD(wparam) == CBN_SELCHANGE) {
                        store_current();
                        g_app.selected = static_cast<int>(
                            SendDlgItemMessageW(dlg, IDC_VOICE_LIST, CB_GETCURSEL, 0, 0));
                        load_current();
                    }
                    return TRUE;

                case IDC_VOICE_NEW:
                    new_voice();
                    return TRUE;
                case IDC_VOICE_RENAME:
                    rename_voice();
                    return TRUE;
                case IDC_VOICE_DELETE:
                    delete_voice();
                    return TRUE;

                case IDC_RATE_DEFAULT:
                case IDC_PITCH_DEFAULT: {
                    const bool use_default =
                        IsDlgButtonChecked(dlg, LOWORD(wparam)) == BST_CHECKED;
                    const int edit = LOWORD(wparam) == IDC_RATE_DEFAULT ? IDC_RATE : IDC_PITCH;
                    EnableWindow(GetDlgItem(dlg, edit), !use_default);
                    if (use_default) {
                        const UiVoice* voice = current_voice();
                        if (voice) {
                            const ivx::VoiceRanges r = ranges_for(voice->mode);
                            SetDlgItemInt(dlg, edit,
                                          edit == IDC_RATE ? r.speed.default_value
                                                           : r.pitch.default_value,
                                          FALSE);
                        }
                    }
                    mark_dirty();
                    return TRUE;
                }

                case IDC_PREVIEW_SPEAK:
                    start_preview();
                    return TRUE;
                case IDC_PREVIEW_STOP:
                    stop_preview();
                    set_status(L"Stopped speaking.");
                    return TRUE;

                default:
                    if (HIWORD(wparam) == EN_CHANGE || HIWORD(wparam) == CBN_SELCHANGE) {
                        mark_dirty();
                    }
                    break;
            }
            break;

        default:
            break;
    }
    return FALSE;
}

INT_PTR CALLBACK text_page_proc(HWND dlg, UINT message, WPARAM wparam, LPARAM)
{
    switch (message) {
        case WM_INITDIALOG: {
            g_language_values.clear();
            for (const auto& choice : kLanguages) {
                SendDlgItemMessageW(dlg, IDC_LANGUAGE, CB_ADDSTRING, 0,
                                    reinterpret_cast<LPARAM>(choice.label));
                g_language_values.emplace_back(choice.lcid);
            }
            const wchar_t* genders[] = {L"Same as the voice it speaks through", L"Male",
                                        L"Female"};
            for (const wchar_t* g : genders) {
                SendDlgItemMessageW(dlg, IDC_GENDER, CB_ADDSTRING, 0,
                                    reinterpret_cast<LPARAM>(g));
            }
            const wchar_t* ages[] = {L"Same as the voice it speaks through", L"Child",
                                     L"Adult", L"Senior"};
            for (const wchar_t* a : ages) {
                SendDlgItemMessageW(dlg, IDC_AGE, CB_ADDSTRING, 0,
                                    reinterpret_cast<LPARAM>(a));
            }
            return TRUE;
        }

        case WM_COMMAND:
            if (HIWORD(wparam) == EN_CHANGE || HIWORD(wparam) == CBN_SELCHANGE) {
                mark_dirty();
            }
            break;

        default:
            break;
    }
    return FALSE;
}

INT_PTR CALLBACK engine_page_proc(HWND dlg, UINT message, WPARAM wparam, LPARAM)
{
    switch (message) {
        case WM_INITDIALOG: {
            const wchar_t* levels[] = {L"Leave as it is",
                                       L"Nothing",
                                       L"Errors only",
                                       L"Errors and warnings",
                                       L"Normal",
                                       L"Detailed",
                                       L"Everything, including every audio buffer"};
            for (const wchar_t* level : levels) {
                SendDlgItemMessageW(dlg, IDC_LOG_LEVEL, CB_ADDSTRING, 0,
                                    reinterpret_cast<LPARAM>(level));
            }
            set_text(dlg, IDC_ENGINE_REPORT, kEngineReport);
            set_text(dlg, IDC_CONFIG_PATH, ivx::config_path());
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case IDC_REALTIME_ENABLE:
                    EnableWindow(GetDlgItem(dlg, IDC_REALTIME),
                                 IsDlgButtonChecked(dlg, IDC_REALTIME_ENABLE) == BST_CHECKED);
                    mark_dirty();
                    return TRUE;

                case IDC_OPEN_LOGS: {
                    std::wstring dir = ivx::log_file_path();
                    const std::size_t slash = dir.find_last_of(L'\\');
                    if (slash != std::wstring::npos) {
                        dir.resize(slash);
                        ShellExecuteW(dlg, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        set_status(L"Opened the log folder " + dir + L".");
                    } else {
                        set_status(L"There is no log folder yet; logging may be turned off.");
                    }
                    return TRUE;
                }

                case IDC_OPEN_CONFIG: {
                    const std::wstring path = ivx::config_path();
                    if (ivx::file_exists(path)) {
                        ShellExecuteW(dlg, L"open", L"notepad.exe", path.c_str(), nullptr,
                                      SW_SHOWNORMAL);
                        set_status(L"Opened " + path + L" in Notepad.");
                    } else {
                        set_status(L"The settings file does not exist yet. Save first, then "
                                   L"it can be opened.");
                    }
                    return TRUE;
                }

                default:
                    if (HIWORD(wparam) == BN_CLICKED || HIWORD(wparam) == EN_CHANGE ||
                        HIWORD(wparam) == CBN_SELCHANGE) {
                        mark_dirty();
                    }
                    break;
            }
            break;

        default:
            break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------------------
// The frame.
// ---------------------------------------------------------------------------------------
void show_page(int index)
{
    for (int i = 0; i < 3; ++i) {
        ShowWindow(g_app.page[i], i == index ? SW_SHOW : SW_HIDE);
    }
}

void create_pages(HWND dlg)
{
    HWND tabs = GetDlgItem(dlg, IDC_TABS);

    // Inno's components tree taught this project that a container with no accessible name
    // is announced as nothing but its type. A tab control is the same: name it.
    SetWindowTextW(tabs, L"Configuration pages");

    TCITEMW item{};
    item.mask = TCIF_TEXT;
    const wchar_t* titles[] = {L"Voices", L"Text and reporting", L"Speech engine"};
    for (int i = 0; i < 3; ++i) {
        item.pszText = const_cast<wchar_t*>(titles[i]);
        TabCtrl_InsertItem(tabs, i, &item);
    }

    // The pages are children of the tab control rather than of the dialog. As children of
    // the dialog they would have to sit above the tab control in the z-order to be painted,
    // and the dialog manager walks the z-order to build the tab order - so every control on
    // the page came before the tab control that chooses the page. Parenting them here puts
    // the tab control first and the page's controls immediately after it, which is the
    // order a keyboard user expects and the order a screen reader describes.
    SetWindowLongPtrW(tabs, GWL_EXSTYLE,
                      GetWindowLongPtrW(tabs, GWL_EXSTYLE) | WS_EX_CONTROLPARENT);

    RECT area{};
    GetClientRect(tabs, &area);
    TabCtrl_AdjustRect(tabs, FALSE, &area);

    const int templates[] = {IDD_PAGE_VOICES, IDD_PAGE_TEXT, IDD_PAGE_ENGINE};
    DLGPROC procs[] = {voices_page_proc, text_page_proc, engine_page_proc};
    for (int i = 0; i < 3; ++i) {
        g_app.page[i] = CreateDialogParamW(g_app.instance, MAKEINTRESOURCEW(templates[i]),
                                           tabs, procs[i], 0);
        SetWindowPos(g_app.page[i], HWND_TOP, area.left, area.top, area.right - area.left,
                     area.bottom - area.top, SWP_SHOWWINDOW);
        ShowWindow(g_app.page[i], SW_HIDE);
    }
    show_page(0);
}

void start_engine()
{
    g_app.worker.post([] {
        if (FAILED(ivx::Engine::instance().ensure_initialised())) {
            if (g_app.main) {
                PostMessageW(g_app.main, WM_APP_ENGINE_READY, 0, 0);
            }
            return;
        }
        if (g_app.main) {
            PostMessageW(g_app.main, WM_APP_ENGINE_READY, 1, 0);
        }
    });
}

void fetch_ranges(const GUID& mode)
{
    if (!g_app.engine_ready || g_app.ranges.count(guid_key(mode))) {
        return;
    }
    g_app.worker.post([mode] {
        ivx::VoiceRanges ranges;
        if (SUCCEEDED(ivx::Engine::instance().get_ranges(mode, ranges))) {
            auto* copy = new std::pair<std::wstring, ivx::VoiceRanges>(guid_key(mode), ranges);
            if (g_app.main) {
                PostMessageW(g_app.main, WM_APP_RANGES_READY, 0,
                             reinterpret_cast<LPARAM>(copy));
            } else {
                delete copy;
            }
        }
    });
}

INT_PTR CALLBACK main_proc(HWND dlg, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
        case WM_INITDIALOG: {
            g_app.main = dlg;
            create_pages(dlg);

            g_app.cfg = ivx::config();
            if (g_app.cfg.engine.preview_text.empty()) {
                g_app.cfg.engine.preview_text = kDefaultPreviewText;
            }

            rebuild_voice_list();
            fill_voice_combo();
            load_engine_page();

            set_text(g_app.page[0], IDC_PREVIEW_TEXT, g_app.cfg.engine.preview_text);
            if (!g_app.voices.empty()) {
                g_app.selected = 0;
                SendDlgItemMessageW(g_app.page[0], IDC_VOICE_LIST, CB_SETCURSEL, 0, 0);
                load_current();
            } else {
                set_status(L"No Infovox 330 voices were found. Check that the voice data is "
                           L"installed beside the program.");
            }
            g_app.dirty = false;

            set_status(L"Ready. Choose a voice, adjust it, and press Speak the preview to "
                       L"hear it. The engine is still starting.");
            start_engine();
            return TRUE;
        }

        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lparam);
            if (header && header->idFrom == IDC_TABS && header->code == TCN_SELCHANGE) {
                store_current();
                show_page(TabCtrl_GetCurSel(GetDlgItem(dlg, IDC_TABS)));
                return TRUE;
            }
            break;
        }

        case WM_APP_ENGINE_READY: {
            g_app.engine_ready = wparam != 0;
            g_app.engine_failed = wparam == 0;
            if (g_app.engine_ready) {
                set_status(L"The engine is ready. Preview will speak with the settings on "
                           L"screen, saved or not.");
                const UiVoice* voice = current_voice();
                if (voice) {
                    fetch_ranges(voice->mode);
                }
            } else {
                set_status(L"The engine could not be started, so nothing can be previewed. "
                           L"Settings can still be edited and saved.");
                update_capabilities();
            }
            return TRUE;
        }

        case WM_APP_RANGES_READY: {
            auto* pair = reinterpret_cast<std::pair<std::wstring, ivx::VoiceRanges>*>(lparam);
            if (pair) {
                g_app.ranges[pair->first] = pair->second;
                delete pair;
                const bool was_loading = g_app.loading;
                g_app.loading = true;
                update_capabilities();
                g_app.loading = was_loading;
            }
            return TRUE;
        }

        case WM_APP_PREVIEW_READY: {
            std::lock_guard<std::mutex> lock(g_app.preview_mutex);
            if (!g_app.preview_wav.empty()) {
                g_app.preview_playing.store(true, std::memory_order_release);
                PlaySoundW(reinterpret_cast<LPCWSTR>(g_app.preview_wav.data()), nullptr,
                           SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
                set_status(L"Speaking the preview.");
            }
            return TRUE;
        }

        case WM_APP_STATUS: {
            std::wstring text;
            {
                std::lock_guard<std::mutex> lock(g_status_mutex);
                text = g_status_pending;
            }
            set_status(text);
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case IDOK:
                    if (save_everything()) {
                        EndDialog(dlg, IDOK);
                    }
                    return TRUE;

                case IDC_APPLY:
                    save_everything();
                    return TRUE;

                case IDCANCEL:
                    // Closing with unsaved changes asks rather than assuming. Saving is the
                    // default answer, because losing a voice someone has just tuned by ear
                    // is worse than saving a change they meant to abandon.
                    store_current();
                    store_engine_page();
                    if (g_app.dirty) {
                        const int answer = MessageBoxW(
                            dlg,
                            L"Save the changes to your Infovox 330 settings before closing?",
                            L"Save changes", MB_YESNOCANCEL | MB_ICONQUESTION);
                        if (answer == IDCANCEL) {
                            return TRUE;
                        }
                        if (answer == IDYES && !save_everything()) {
                            return TRUE;
                        }
                    }
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;

                default:
                    break;
            }
            break;

        case WM_CLOSE:
            SendMessageW(dlg, WM_COMMAND, IDCANCEL, 0);
            return TRUE;

        default:
            break;
    }
    return FALSE;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
    ivx::log_init(L"config");
    IVX_LOG_I("Infovox 330 configuration utility starting");

    g_app.instance = instance;

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TAB_CLASSES | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    g_app.worker.start();

    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_MAIN), nullptr, main_proc, 0);

    stop_preview();
    g_app.worker.stop();
    ivx::Engine::instance().shutdown();

    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    IVX_LOG_I("Infovox 330 configuration utility exiting");
    return 0;
}
