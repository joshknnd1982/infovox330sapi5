#include "ivx_voices.hpp"

#include "ivx_log.hpp"
#include "ivx_paths.hpp"

#include <cstdio>
#include <cwchar>
#include <mutex>

namespace ivx {
namespace {

std::mutex g_catalogue_mutex;
std::vector<VoiceDesc> g_catalogue;
bool g_catalogue_loaded = false;

[[nodiscard]] std::wstring trim(const std::wstring& s)
{
    std::size_t first = 0;
    while (first < s.size() && (s[first] == L' ' || s[first] == L'\t' || s[first] == L'\r' ||
                                s[first] == L'\n')) {
        ++first;
    }
    std::size_t last = s.size();
    while (last > first && (s[last - 1] == L' ' || s[last - 1] == L'\t' || s[last - 1] == L'\r' ||
                            s[last - 1] == L'\n')) {
        --last;
    }
    return s.substr(first, last - first);
}

[[nodiscard]] bool iequals(const std::wstring& a, const wchar_t* b)
{
    return _wcsicmp(a.c_str(), b) == 0;
}

[[nodiscard]] WORD to_word(const std::wstring& s, WORD fallback)
{
    if (s.empty()) {
        return fallback;
    }
    wchar_t* end = nullptr;
    const unsigned long v = wcstoul(s.c_str(), &end, 10);
    if (end == s.c_str() || v > 0xFFFF) {
        return fallback;
    }
    return static_cast<WORD>(v);
}

[[nodiscard]] GUID to_guid(const std::wstring& s)
{
    GUID g = GUID_NULL;
    if (s.size() < 36) {
        return g;
    }
    std::wstring braced = s;
    if (braced.front() != L'{') {
        braced = L"{" + braced + L"}";
    }
    if (FAILED(CLSIDFromString(braced.c_str(), &g))) {
        return GUID_NULL;
    }
    return g;
}

// Reads a whole file and converts it to UTF-16. VoiceDescriptions.txt is plain ASCII in
// practice, but it is decoded as UTF-8 (with a Latin-1 fallback) so that an edited copy
// carrying accented speaker names still parses.
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

    for (const UINT codepage : {static_cast<UINT>(CP_UTF8), static_cast<UINT>(1252)}) {
        const DWORD flags = codepage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
        const int needed = MultiByteToWideChar(codepage, flags, bytes.data(),
                                               static_cast<int>(bytes.size()), nullptr, 0);
        if (needed <= 0) {
            continue;
        }
        out.assign(static_cast<std::size_t>(needed), L'\0');
        MultiByteToWideChar(codepage, flags, bytes.data(), static_cast<int>(bytes.size()),
                            out.data(), needed);
        return true;
    }
    return false;
}

void finish_section(VoiceDesc& v)
{
    // "Larry (American English)" splits into speaker and language display name. The
    // SpeakerName key wins when it is present, which it always is in the shipped file.
    const std::size_t open = v.section.find(L'(');
    if (open != std::wstring::npos && v.section.back() == L')') {
        v.language_name = trim(v.section.substr(open + 1, v.section.size() - open - 2));
        if (v.speaker.empty()) {
            v.speaker = trim(v.section.substr(0, open));
        }
    }
    if (v.speaker.empty()) {
        v.speaker = trim(v.section);
    }
    if (v.language_name.empty()) {
        v.language_name = L"Unknown";
    }
}

}  // namespace

std::wstring VoiceDesc::display_name() const
{
    return L"Infovox 330 " + speaker + L" - " + language_name;
}

WORD VoiceDesc::sapi_lcid() const
{
    if (language_id == 0) {
        return 0x0409;
    }
    // A bare primary language id (sublang 0) is what the file uses for every language but
    // English. SAPI wants a real locale, so promote it to SUBLANG_DEFAULT.
    if ((language_id >> 10) == 0) {
        return static_cast<WORD>(language_id | 0x0400);
    }
    return language_id;
}

std::wstring VoiceDesc::sapi_language_attribute() const
{
    wchar_t buf[16];
    _snwprintf_s(buf, _TRUNCATE, L"%x", static_cast<unsigned>(sapi_lcid()));
    return buf;
}

std::wstring VoiceDesc::sapi_gender() const
{
    return gender == 1 ? L"Female" : L"Male";
}

std::wstring VoiceDesc::sapi_age() const
{
    if (age < 13) {
        return L"Child";
    }
    if (age >= 60) {
        return L"Senior";
    }
    return L"Adult";
}

std::vector<VoiceDesc> parse_voice_descriptions(const std::wstring& file_path)
{
    std::vector<VoiceDesc> voices;

    std::wstring text;
    if (!read_text_file(file_path, text)) {
        IVX_LOG_E("cannot read %s", log_narrow(file_path.c_str()).c_str());
        return voices;
    }

    bool in_section = false;
    VoiceDesc current;

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
            if (in_section) {
                finish_section(current);
                voices.push_back(current);
            }
            current = VoiceDesc{};
            current.section = trim(line.substr(1, line.size() - 2));
            in_section = true;
            continue;
        }

        if (!in_section) {
            continue;
        }

        const std::size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) {
            continue;
        }
        const std::wstring key = trim(line.substr(0, eq));
        const std::wstring value = trim(line.substr(eq + 1));

        if (iequals(key, L"ModeGUID")) {
            current.mode_guid = to_guid(value);
        } else if (iequals(key, L"LanguageID")) {
            current.language_id = to_word(value, 0);
        } else if (iequals(key, L"SpeakerName")) {
            current.speaker = value;
        } else if (iequals(key, L"Gender")) {
            current.gender = to_word(value, 2);
        } else if (iequals(key, L"Age")) {
            current.age = to_word(value, 30);
        } else if (iequals(key, L"Pitch")) {
            current.default_pitch = to_word(value, 50);
        } else if (iequals(key, L"Dynamic")) {
            current.default_dynamic = to_word(value, 32);
        } else if (iequals(key, L"DiphoneFile")) {
            current.diphone_file = value;
        } else if (iequals(key, L"MappingFile")) {
            current.mapping_file = value;
        } else if (iequals(key, L"LanguageFile")) {
            current.language_file = value;
        } else if (iequals(key, L"LibraryFile")) {
            current.library_file = value;
        }
    }

    if (in_section) {
        finish_section(current);
        voices.push_back(current);
    }

    // Two reasons to drop a section: it cannot be selected without a mode GUID, and it
    // cannot speak without its diphone database. The second matters because the installer
    // lets a language be left out, and a voice whose data is absent must not be offered.
    const std::wstring dir = file_path.substr(0, file_path.find_last_of(L'\\'));
    for (auto it = voices.begin(); it != voices.end();) {
        if (IsEqualGUID(it->mode_guid, GUID_NULL)) {
            IVX_LOG_W("voice section '%s' has no usable ModeGUID; ignoring",
                      log_narrow(it->section.c_str()).c_str());
            it = voices.erase(it);
        } else if (!it->diphone_file.empty() && !file_exists(dir + L"\\" + it->diphone_file)) {
            IVX_LOG_I("voice '%s' is not installed (%s missing); ignoring",
                      log_narrow(it->speaker.c_str()).c_str(),
                      log_narrow(it->diphone_file.c_str()).c_str());
            it = voices.erase(it);
        } else {
            ++it;
        }
    }

    IVX_LOG_I("parsed %zu voices from %s", voices.size(),
              log_narrow(file_path.c_str()).c_str());
    for (const auto& v : voices) {
        IVX_LOG_D("  voice '%s' lang=%u->%04x gender=%u age=%u mode=%s",
                  log_narrow(v.speaker.c_str()).c_str(), v.language_id,
                  static_cast<unsigned>(v.sapi_lcid()), v.gender, v.age,
                  guid_string(v.mode_guid).c_str());
    }
    return voices;
}

const std::vector<VoiceDesc>& voice_catalogue()
{
    std::lock_guard<std::mutex> lock(g_catalogue_mutex);
    if (!g_catalogue_loaded) {
        g_catalogue_loaded = true;
        const std::wstring dir = voices_dir();
        if (!dir.empty()) {
            g_catalogue = parse_voice_descriptions(dir + L"\\VoiceDescriptions.txt");
        } else {
            IVX_LOG_E("no voices directory; voice catalogue is empty");
        }
    }
    return g_catalogue;
}

const VoiceDesc* find_voice(const std::wstring& token_name)
{
    for (const auto& v : voice_catalogue()) {
        if (_wcsicmp(v.token_name().c_str(), token_name.c_str()) == 0) {
            return &v;
        }
    }
    return nullptr;
}

const VoiceDesc* find_voice_for_language(WORD lcid, WORD prefer_gender)
{
    if (lcid == 0) {
        return nullptr;
    }
    const auto& all = voice_catalogue();
    const WORD primary = static_cast<WORD>(lcid & 0x03FF);

    const VoiceDesc* exact = nullptr;
    const VoiceDesc* exact_gender = nullptr;
    const VoiceDesc* loose = nullptr;
    const VoiceDesc* loose_gender = nullptr;

    for (const auto& v : all) {
        const WORD voice_lcid = v.sapi_lcid();
        if (voice_lcid == lcid) {
            if (!exact) {
                exact = &v;
            }
            if (!exact_gender && v.gender == prefer_gender) {
                exact_gender = &v;
            }
        } else if (static_cast<WORD>(voice_lcid & 0x03FF) == primary) {
            if (!loose) {
                loose = &v;
            }
            if (!loose_gender && v.gender == prefer_gender) {
                loose_gender = &v;
            }
        }
    }
    if (exact_gender) return exact_gender;
    if (exact) return exact;
    if (loose_gender) return loose_gender;
    return loose;
}

}  // namespace ivx
