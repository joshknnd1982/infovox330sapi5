// The voice catalogue, read from Voices Ivx330\VoiceDescriptions.txt.
//
// That file is the engine's own manifest: it carries the mode GUID, language id, speaker
// name, gender, age and default prosody for every installed voice. Reading it directly
// means voice enumeration needs neither the registry nor the engine, so it costs nothing
// in the 64-bit build and does not have to start the 32-bit helper.

#pragma once

#include <windows.h>
#include <objbase.h>

#include <string>
#include <vector>

namespace ivx {

struct VoiceDesc {
    std::wstring section;       // raw section header, e.g. "Larry (American English)"
    std::wstring speaker;       // "Larry"
    std::wstring language_name; // "American English"
    GUID mode_guid = GUID_NULL;
    WORD language_id = 0;       // as written in the file (may be a neutral primary id)
    WORD gender = 0;            // 1 = female, 2 = male, as the file spells it
    WORD age = 30;
    WORD default_pitch = 50;
    WORD default_dynamic = 32;
    std::wstring diphone_file;
    std::wstring mapping_file;
    std::wstring language_file;
    std::wstring library_file;

    // A voice the user defined in the configuration utility. It carries the mode GUID and
    // the data files of the voice it is built on and differs only in what it is called and
    // in the settings saved against its name, because the engine's sixteen modes are fixed:
    // a section added to VoiceDescriptions.txt by hand is not enumerated, and changing a
    // section's own keys was measured to change nothing. See tools/ivx_probe.cpp.
    bool is_custom = false;
    std::wstring custom_name;       // the SAPI 5 token name when is_custom
    std::wstring display_override;  // what applications show when is_custom
    std::wstring base_speaker;      // the built-in voice underneath, when is_custom

    // Unique, stable identifier used as the SAPI 5 token name, and the key the voice's
    // saved settings are stored under.
    [[nodiscard]] std::wstring token_name() const
    {
        return is_custom ? custom_name : speaker;
    }

    // What a speech application shows the user.
    [[nodiscard]] std::wstring display_name() const;

    // Full LCID for the SAPI 5 "Language" attribute; neutral ids get SUBLANG_DEFAULT.
    [[nodiscard]] WORD sapi_lcid() const;

    // "406", "409", ... - SAPI 5 wants the LCID in lowercase hex without a prefix.
    [[nodiscard]] std::wstring sapi_language_attribute() const;

    [[nodiscard]] std::wstring sapi_gender() const;  // "Male" / "Female"
    [[nodiscard]] std::wstring sapi_age() const;     // "Adult" / "Child" / "Senior"
};

// Parses VoiceDescriptions.txt. Returns an empty vector and logs on failure.
[[nodiscard]] std::vector<VoiceDesc> parse_voice_descriptions(const std::wstring& file_path);

// The sixteen voices the engine ships with, straight from VoiceDescriptions.txt. This is
// what a user-defined voice picks its base from, and it is never affected by configuration.
[[nodiscard]] const std::vector<VoiceDesc>& builtin_catalogue();

// What SAPI is offered: the built-in voices unless the user has hidden them, followed by
// the voices the user defined. Rebuilt whenever the configuration file changes, so a voice
// added in the configuration utility appears in applications that are already running.
[[nodiscard]] const std::vector<VoiceDesc>& voice_catalogue();

// Case-insensitive lookup by token name (speaker). Returns nullptr if absent.
[[nodiscard]] const VoiceDesc* find_voice(const std::wstring& token_name);

// The best voice for an LCID, used when a <lang> tag asks for a language the current voice
// does not speak. An exact locale match wins; failing that, any voice sharing the primary
// language. prefer_gender (1 female, 2 male) breaks ties so a switch mid-sentence does not
// change sex unnecessarily. Returns nullptr when nothing speaks that language.
[[nodiscard]] const VoiceDesc* find_voice_for_language(WORD lcid, WORD prefer_gender);

}  // namespace ivx
