#include "ivx_config.hpp"

#include "ivx_log.hpp"
#include "ivx_paths.hpp"

#include <shlobj.h>

#include <cstdio>
#include <cwchar>
#include <mutex>

namespace ivx {
namespace {

std::mutex g_mutex;
Config g_config;
bool g_loaded = false;
unsigned g_generation = 0;

FILETIME g_stamp{};
ULONGLONG g_size = 0;
ULONGLONG g_last_check_tick = 0;

// A speaking application asks for the configuration once per utterance at least, and the
// engine is in-process, so the file cannot be stat'd every time. Once a second is far more
// often than a person changes a setting and far less often than an utterance.
constexpr ULONGLONG kRecheckIntervalMs = 1000;

[[nodiscard]] std::wstring trim(const std::wstring& s)
{
    std::size_t first = 0;
    while (first < s.size() && (s[first] == L' ' || s[first] == L'\t' || s[first] == L'\r')) {
        ++first;
    }
    std::size_t last = s.size();
    while (last > first &&
           (s[last - 1] == L' ' || s[last - 1] == L'\t' || s[last - 1] == L'\r')) {
        --last;
    }
    return s.substr(first, last - first);
}

[[nodiscard]] bool iequals(const std::wstring& a, const wchar_t* b)
{
    return _wcsicmp(a.c_str(), b) == 0;
}

[[nodiscard]] int to_int(const std::wstring& s, int fallback)
{
    if (s.empty()) {
        return fallback;
    }
    wchar_t* end = nullptr;
    const long v = wcstol(s.c_str(), &end, 10);
    return end == s.c_str() ? fallback : static_cast<int>(v);
}

[[nodiscard]] double to_double(const std::wstring& s, double fallback)
{
    if (s.empty()) {
        return fallback;
    }
    wchar_t* end = nullptr;
    const double v = wcstod(s.c_str(), &end);
    return (end == s.c_str() || v <= 0.0) ? fallback : v;
}

[[nodiscard]] bool to_bool(const std::wstring& s, bool fallback)
{
    if (s.empty()) {
        return fallback;
    }
    if (iequals(s, L"1") || iequals(s, L"yes") || iequals(s, L"true") || iequals(s, L"on")) {
        return true;
    }
    if (iequals(s, L"0") || iequals(s, L"no") || iequals(s, L"false") || iequals(s, L"off")) {
        return false;
    }
    return fallback;
}

[[nodiscard]] std::wstring env_value(const wchar_t* name)
{
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    return (n == 0 || n >= std::size(buf)) ? std::wstring() : std::wstring(buf, n);
}

void make_directories(const std::wstring& path)
{
    for (std::size_t i = 3; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == L'\\') {
            CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
        }
    }
}

// Substitutions are stored one per line inside a single value, so a "from" containing an
// equals sign or a comma cannot break the file. The separator is a tab, which cannot occur
// in a value that came from a single-line edit control.
[[nodiscard]] std::wstring encode_substitutions(const std::vector<Substitution>& subs)
{
    std::wstring out;
    for (const auto& s : subs) {
        if (s.from.empty()) {
            continue;
        }
        if (!out.empty()) {
            out.push_back(L'|');
        }
        out += s.from;
        out.push_back(L'\t');
        out += s.to;
    }
    return out;
}

[[nodiscard]] std::vector<Substitution> decode_substitutions(const std::wstring& value)
{
    std::vector<Substitution> subs;
    std::size_t pos = 0;
    while (pos <= value.size()) {
        const std::size_t bar = value.find(L'|', pos);
        const std::wstring item =
            value.substr(pos, bar == std::wstring::npos ? std::wstring::npos : bar - pos);
        pos = (bar == std::wstring::npos) ? value.size() + 1 : bar + 1;
        if (item.empty()) {
            continue;
        }
        const std::size_t tab = item.find(L'\t');
        Substitution s;
        s.from = trim(tab == std::wstring::npos ? item : item.substr(0, tab));
        s.to = tab == std::wstring::npos ? std::wstring() : trim(item.substr(tab + 1));
        if (!s.from.empty()) {
            subs.push_back(std::move(s));
        }
    }
    return subs;
}

[[nodiscard]] bool read_text_file(const std::wstring& path, std::wstring& out)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) {
        return false;
    }
    std::string bytes;
    char buf[4096];
    std::size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        bytes.append(buf, n);
    }
    fclose(f);

    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                                           static_cast<int>(bytes.size()), nullptr, 0);
    if (needed < 0) {
        return false;
    }
    out.assign(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(),
                        needed);
    return true;
}

void apply_voice_key(VoiceSettings& v, const std::wstring& key, const std::wstring& value)
{
    if (iequals(key, L"Rate")) {
        v.rate = to_int(value, -1);
    } else if (iequals(key, L"Pitch")) {
        v.pitch = to_int(value, -1);
    } else if (iequals(key, L"Volume")) {
        v.volume = to_int(value, 100);
    } else if (iequals(key, L"RateSpan")) {
        v.rate_span = to_double(value, 3.0);
    } else if (iequals(key, L"PitchSpan")) {
        v.pitch_span = to_double(value, 2.0);
    } else if (iequals(key, L"Prefix")) {
        v.prefix = value;
    } else if (iequals(key, L"Substitutions")) {
        v.substitutions = decode_substitutions(value);
    } else if (iequals(key, L"Language")) {
        v.language = value;
    } else if (iequals(key, L"Gender")) {
        v.gender = value;
    } else if (iequals(key, L"Age")) {
        v.age = value;
    }
}

void write_voice_keys(std::wstring& out, const VoiceSettings& v)
{
    wchar_t buf[64];
    _snwprintf_s(buf, _TRUNCATE, L"%d", v.rate);
    out += L"Rate=" + std::wstring(buf) + L"\r\n";
    _snwprintf_s(buf, _TRUNCATE, L"%d", v.pitch);
    out += L"Pitch=" + std::wstring(buf) + L"\r\n";
    _snwprintf_s(buf, _TRUNCATE, L"%d", v.volume);
    out += L"Volume=" + std::wstring(buf) + L"\r\n";
    _snwprintf_s(buf, _TRUNCATE, L"%.3f", v.rate_span);
    out += L"RateSpan=" + std::wstring(buf) + L"\r\n";
    _snwprintf_s(buf, _TRUNCATE, L"%.3f", v.pitch_span);
    out += L"PitchSpan=" + std::wstring(buf) + L"\r\n";
    out += L"Prefix=" + v.prefix + L"\r\n";
    out += L"Substitutions=" + encode_substitutions(v.substitutions) + L"\r\n";
    out += L"Language=" + v.language + L"\r\n";
    out += L"Gender=" + v.gender + L"\r\n";
    out += L"Age=" + v.age + L"\r\n";
}

void parse(const std::wstring& text, Config& cfg)
{
    enum class Section { None, Settings, Voice, Custom };
    Section section = Section::None;
    std::wstring section_name;
    VoiceSettings voice;
    CustomVoice custom;

    const auto flush = [&] {
        if (section == Section::Voice && !section_name.empty()) {
            cfg.voices[section_name] = voice;
        } else if (section == Section::Custom && !custom.name.empty()) {
            if (custom.display_name.empty()) {
                custom.display_name = custom.name;
            }
            custom.settings = voice;
            cfg.custom.push_back(custom);
        }
    };

    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t eol = text.find(L'\n', pos);
        const std::wstring line =
            trim(text.substr(pos, eol == std::wstring::npos ? std::wstring::npos : eol - pos));
        pos = (eol == std::wstring::npos) ? text.size() + 1 : eol + 1;

        if (line.empty() || line[0] == L';' || line[0] == L'#') {
            continue;
        }

        if (line.front() == L'[' && line.back() == L']') {
            flush();
            voice = VoiceSettings{};
            custom = CustomVoice{};
            section_name.clear();

            const std::wstring header = trim(line.substr(1, line.size() - 2));
            const std::size_t colon = header.find(L':');
            const std::wstring kind = colon == std::wstring::npos ? header
                                                                  : header.substr(0, colon);
            const std::wstring rest =
                colon == std::wstring::npos ? std::wstring() : trim(header.substr(colon + 1));

            if (iequals(kind, L"Settings")) {
                section = Section::Settings;
            } else if (iequals(kind, L"Voice")) {
                section = Section::Voice;
                section_name = rest;
            } else if (iequals(kind, L"CustomVoice")) {
                section = Section::Custom;
                section_name = rest;
                custom.name = rest;
            } else {
                section = Section::None;
                IVX_LOG_W("config: ignoring unknown section [%s]",
                          log_narrow(header.c_str()).c_str());
            }
            continue;
        }

        const std::size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) {
            continue;
        }
        const std::wstring key = trim(line.substr(0, eq));
        const std::wstring value = trim(line.substr(eq + 1));

        switch (section) {
            case Section::Settings:
                if (iequals(key, L"WordEvents")) {
                    cfg.engine.word_events = to_bool(value, true);
                } else if (iequals(key, L"SentenceEvents")) {
                    cfg.engine.sentence_events = to_bool(value, true);
                } else if (iequals(key, L"SoftwareVolume")) {
                    cfg.engine.software_volume = to_bool(value, true);
                } else if (iequals(key, L"HideBuiltInVoices")) {
                    cfg.engine.hide_builtin = to_bool(value, false);
                } else if (iequals(key, L"SetEngineVolume")) {
                    cfg.engine.set_engine_volume = to_bool(value, false);
                } else if (iequals(key, L"RealTime")) {
                    cfg.engine.realtime = to_int(value, -1);
                } else if (iequals(key, L"LogLevel")) {
                    cfg.engine.log_level = value;
                } else if (iequals(key, L"PreviewText")) {
                    cfg.engine.preview_text = value;
                }
                break;

            case Section::Custom:
                if (iequals(key, L"DisplayName")) {
                    custom.display_name = value;
                    break;
                }
                if (iequals(key, L"Base")) {
                    custom.base_speaker = value;
                    break;
                }
                apply_voice_key(voice, key, value);
                break;

            case Section::Voice:
                apply_voice_key(voice, key, value);
                break;

            case Section::None:
            default:
                break;
        }
    }
    flush();
}

// Reloads if the file's timestamp or size has moved. Called with the lock held.
void refresh_locked(bool force)
{
    const ULONGLONG now = GetTickCount64();
    if (g_loaded && !force && now - g_last_check_tick < kRecheckIntervalMs) {
        return;
    }
    g_last_check_tick = now;

    const std::wstring path = config_path();
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    const bool exists = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attrs) != 0;

    if (g_loaded && !force) {
        if (!exists) {
            return;  // nothing to reload; keep whatever is in hand
        }
        const ULONGLONG size = (static_cast<ULONGLONG>(attrs.nFileSizeHigh) << 32) |
                               attrs.nFileSizeLow;
        if (size == g_size &&
            CompareFileTime(&attrs.ftLastWriteTime, &g_stamp) == 0) {
            return;
        }
    }

    Config fresh;
    std::wstring text;
    if (exists && read_text_file(path, text)) {
        parse(text, fresh);
        g_stamp = attrs.ftLastWriteTime;
        g_size = (static_cast<ULONGLONG>(attrs.nFileSizeHigh) << 32) | attrs.nFileSizeLow;
        IVX_LOG_I("config: read %s - %zu custom voice(s), %zu voice override(s)",
                  log_narrow(path.c_str()).c_str(), fresh.custom.size(), fresh.voices.size());
    } else if (exists) {
        IVX_LOG_W("config: %s exists but could not be read; using defaults",
                  log_narrow(path.c_str()).c_str());
    } else {
        g_stamp = FILETIME{};
        g_size = 0;
        IVX_LOG_D("config: no file at %s; using defaults",
                  log_narrow(path.c_str()).c_str());
    }

    g_config = std::move(fresh);
    g_loaded = true;
    ++g_generation;
}

}  // namespace

bool VoiceSettings::is_default() const
{
    return rate < 0 && pitch < 0 && volume == 100 && rate_span == 3.0 && pitch_span == 2.0 &&
           prefix.empty() && substitutions.empty() && language.empty() && gender.empty() &&
           age.empty();
}

const CustomVoice* Config::find_custom(const std::wstring& name) const
{
    for (const auto& c : custom) {
        if (_wcsicmp(c.name.c_str(), name.c_str()) == 0) {
            return &c;
        }
    }
    return nullptr;
}

CustomVoice* Config::find_custom(const std::wstring& name)
{
    return const_cast<CustomVoice*>(
        static_cast<const Config*>(this)->find_custom(name));
}

std::wstring config_path()
{
    const std::wstring from_env = env_value(L"INFOVOX330_CONFIG");
    if (!from_env.empty()) {
        return from_env;
    }

    wchar_t* known = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known)) && known) {
        dir = known;
    }
    if (known) {
        CoTaskMemFree(known);
    }
    if (dir.empty()) {
        wchar_t temp[MAX_PATH];
        if (GetTempPathW(MAX_PATH, temp) > 0) {
            dir = temp;
            while (!dir.empty() && dir.back() == L'\\') {
                dir.pop_back();
            }
        }
    }
    if (dir.empty()) {
        dir = L"C:";
    }
    return dir + L"\\Infovox330 SAPI5\\config.ini";
}

const Config& config()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    refresh_locked(false);
    return g_config;
}

void invalidate_config()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    refresh_locked(true);
}

unsigned config_generation()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    refresh_locked(false);
    return g_generation;
}

bool save_config(const Config& cfg)
{
    const std::wstring path = config_path();
    const std::size_t slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
        make_directories(path.substr(0, slash));
    }

    std::wstring out;
    out += L"; Infovox 330 SAPI 5 configuration.\r\n";
    out += L"; Written by the Infovox 330 Configuration utility. Safe to edit by hand;\r\n";
    out += L"; a speaking application picks up changes within a second.\r\n";
    out += L"\r\n[Settings]\r\n";
    out += std::wstring(L"WordEvents=") + (cfg.engine.word_events ? L"1" : L"0") + L"\r\n";
    out += std::wstring(L"SentenceEvents=") + (cfg.engine.sentence_events ? L"1" : L"0") +
           L"\r\n";
    out += std::wstring(L"SoftwareVolume=") + (cfg.engine.software_volume ? L"1" : L"0") +
           L"\r\n";
    out += std::wstring(L"HideBuiltInVoices=") + (cfg.engine.hide_builtin ? L"1" : L"0") +
           L"\r\n";
    out += std::wstring(L"SetEngineVolume=") + (cfg.engine.set_engine_volume ? L"1" : L"0") +
           L"\r\n";
    wchar_t buf[64];
    _snwprintf_s(buf, _TRUNCATE, L"%d", cfg.engine.realtime);
    out += L"RealTime=" + std::wstring(buf) + L"\r\n";
    out += L"LogLevel=" + cfg.engine.log_level + L"\r\n";
    out += L"PreviewText=" + cfg.engine.preview_text + L"\r\n";

    for (const auto& [name, settings] : cfg.voices) {
        if (settings.is_default()) {
            continue;  // nothing to say about this voice; leave the file smaller
        }
        out += L"\r\n[Voice:" + name + L"]\r\n";
        write_voice_keys(out, settings);
    }

    for (const auto& c : cfg.custom) {
        if (c.name.empty()) {
            continue;
        }
        out += L"\r\n[CustomVoice:" + c.name + L"]\r\n";
        out += L"DisplayName=" + c.display_name + L"\r\n";
        out += L"Base=" + c.base_speaker + L"\r\n";
        write_voice_keys(out, c.settings);
    }

    const int needed = WideCharToMultiByte(CP_UTF8, 0, out.c_str(),
                                           static_cast<int>(out.size()), nullptr, 0, nullptr,
                                           nullptr);
    std::string bytes(static_cast<std::size_t>(needed > 0 ? needed : 0), '\0');
    if (needed > 0) {
        WideCharToMultiByte(CP_UTF8, 0, out.c_str(), static_cast<int>(out.size()),
                            bytes.data(), needed, nullptr, nullptr);
    }

    // Written through a temporary file and moved into place, so that an application reading
    // the configuration at the wrong moment never sees half of it.
    const std::wstring temp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, temp.c_str(), L"wb") != 0 || !f) {
        IVX_LOG_E("config: cannot write %s", log_narrow(temp.c_str()).c_str());
        return false;
    }
    static const unsigned char kBom[] = {0xEF, 0xBB, 0xBF};
    fwrite(kBom, 1, sizeof(kBom), f);
    if (!bytes.empty()) {
        fwrite(bytes.data(), 1, bytes.size(), f);
    }
    fclose(f);

    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        IVX_LOG_E("config: cannot replace %s (error %lu)", log_narrow(path.c_str()).c_str(),
                  GetLastError());
        return false;
    }
    IVX_LOG_I("config: wrote %s", log_narrow(path.c_str()).c_str());

    invalidate_config();
    return true;
}

VoiceSettings settings_for_voice(const std::wstring& token_name)
{
    const Config& cfg = config();
    if (const CustomVoice* custom = cfg.find_custom(token_name)) {
        return custom->settings;
    }
    const auto it = cfg.voices.find(token_name);
    return it != cfg.voices.end() ? it->second : VoiceSettings{};
}

}  // namespace ivx
