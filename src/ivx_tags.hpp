#pragma once

#include <cstddef>
#include <cwctype>
#include <string>
#include <vector>

namespace ivx {
namespace tags {

enum class Kind { Text, Engine, Pause, Bookmark, Voice, Volume };

namespace effect {

constexpr unsigned kPitch = 1;
constexpr unsigned kRate = 2;
constexpr unsigned kVolume = 4;
constexpr unsigned kVoiceParams = 8;

}  // namespace effect

struct Token {
    Kind kind = Kind::Text;
    std::size_t begin = 0;
    std::size_t length = 0;
    unsigned effects = 0;
    unsigned long number = 0;
    std::wstring value;
    std::wstring tag;
    std::wstring speaker;
    std::wstring language;
    std::wstring accent;
};

namespace detail {

enum class Value { None, Number, Any, Voice };

struct Spec {
    const wchar_t* name;
    Value value;
    Kind kind;
    unsigned effects;
};

constexpr Spec kSpecs[] = {
    {L"pit", Value::Number, Kind::Engine, effect::kPitch},
    {L"spd", Value::Number, Kind::Engine, effect::kRate},
    {L"vol", Value::Number, Kind::Volume, effect::kVolume},
    {L"rst", Value::None, Kind::Engine, effect::kPitch | effect::kRate | effect::kVoiceParams},
    {L"vce", Value::Voice, Kind::Voice, 0},
    {L"pau", Value::Number, Kind::Pause, 0},
    {L"mrk", Value::Number, Kind::Bookmark, 0},
    {L"emp", Value::None, Kind::Engine, 0},
    {L"chr", Value::Any, Kind::Engine, 0},
    {L"com", Value::Any, Kind::Engine, 0},
    {L"ctx", Value::Any, Kind::Engine, 0},
    {L"prn", Value::Any, Kind::Engine, 0},
    {L"pro", Value::Any, Kind::Engine, 0},
    {L"prt", Value::Any, Kind::Engine, 0},
};

constexpr const wchar_t* kVoiceKeys[] = {L"Speaker", L"Language", L"Accent", L"Gender", L"Age"};

constexpr std::size_t kLongestTag = 256;

inline bool same_name(const wchar_t* text, std::size_t length, const wchar_t* name)
{
    std::size_t i = 0;
    for (; i < length && name[i]; ++i) {
        if (std::towlower(static_cast<std::wint_t>(text[i])) !=
            std::towlower(static_cast<std::wint_t>(name[i]))) {
            return false;
        }
    }
    return i == length && name[i] == 0;
}

inline bool whole_number(const wchar_t* text, std::size_t length, unsigned long* out)
{
    if (length == 0 || length > 10) {
        return false;
    }
    unsigned long long n = 0;
    for (std::size_t i = 0; i < length; ++i) {
        if (text[i] < L'0' || text[i] > L'9') {
            return false;
        }
        n = n * 10 + static_cast<unsigned long long>(text[i] - L'0');
    }
    if (n > 0xFFFFFFFFull) {
        return false;
    }
    *out = static_cast<unsigned long>(n);
    return true;
}

inline bool blank(wchar_t ch)
{
    return std::iswspace(static_cast<std::wint_t>(ch)) != 0;
}

inline std::wstring trimmed(const wchar_t* text, std::size_t length)
{
    std::size_t begin = 0;
    while (begin < length && blank(text[begin])) {
        ++begin;
    }
    while (length > begin && blank(text[length - 1])) {
        --length;
    }
    return std::wstring(text + begin, length - begin);
}

inline bool read_voice(const wchar_t* text, std::size_t length, Token* token)
{
    std::wstring shape;
    std::size_t i = 0;
    for (;;) {
        const std::size_t key_begin = i;
        while (i < length && text[i] != L'=' && text[i] != L',' && text[i] != L'"') {
            ++i;
        }
        if (i == length || text[i] != L'=') {
            return false;
        }
        const std::wstring key = trimmed(text + key_begin, i - key_begin);
        std::size_t known = 0;
        while (known < 5 && !same_name(key.data(), key.size(), kVoiceKeys[known])) {
            ++known;
        }
        if (known == 5) {
            return false;
        }
        ++i;
        while (i < length && blank(text[i])) {
            ++i;
        }
        std::wstring value;
        if (i < length && text[i] == L'"') {
            const std::size_t open = ++i;
            while (i < length && text[i] != L'"') {
                ++i;
            }
            if (i == length) {
                return false;
            }
            value.assign(text + open, i - open);
            ++i;
            while (i < length && blank(text[i])) {
                ++i;
            }
        } else {
            const std::size_t open = i;
            while (i < length && text[i] != L',' && text[i] != L'"') {
                ++i;
            }
            if (i < length && text[i] == L'"') {
                return false;
            }
            value = trimmed(text + open, i - open);
        }
        if (value.empty()) {
            return false;
        }
        switch (known) {
            case 0:
                token->speaker = value;
                break;
            case 1:
                token->language = value;
                break;
            case 2:
                token->accent = value;
                break;
            default:
                if (!shape.empty()) {
                    shape += L',';
                }
                shape += kVoiceKeys[known];
                shape += L"=\"" + value + L"\"";
                break;
        }
        if (i == length) {
            break;
        }
        if (text[i] != L',') {
            return false;
        }
        ++i;
    }
    if (!shape.empty()) {
        token->tag = L"\\Vce=" + shape + L"\\";
        token->effects = effect::kVoiceParams;
    }
    return true;
}

inline bool read_tag(const wchar_t* text, std::size_t length, std::size_t at, Token* token)
{
    std::size_t close = at + 1;
    while (close < length && text[close] != L'\\' && close - at <= kLongestTag) {
        ++close;
    }
    if (close >= length || text[close] != L'\\') {
        return false;
    }

    const wchar_t* inner = text + at + 1;
    const std::size_t inner_length = close - at - 1;
    std::size_t name_length = 0;
    while (name_length < inner_length && inner[name_length] != L'=') {
        ++name_length;
    }
    const bool has_value = name_length < inner_length;
    const wchar_t* value = inner + name_length + 1;
    const std::size_t value_length = has_value ? inner_length - name_length - 1 : 0;

    for (const Spec& spec : kSpecs) {
        if (!same_name(inner, name_length, spec.name)) {
            continue;
        }
        Token read;
        read.kind = spec.kind;
        read.begin = at;
        read.length = close - at + 1;
        read.effects = spec.effects;
        switch (spec.value) {
            case Value::None:
                if (has_value) {
                    return false;
                }
                break;
            case Value::Number:
                if (!has_value || !whole_number(value, value_length, &read.number)) {
                    return false;
                }
                break;
            case Value::Any:
                if (!has_value || value_length == 0) {
                    return false;
                }
                break;
            case Value::Voice:
                if (!has_value || !read_voice(value, value_length, &read)) {
                    return false;
                }
                break;
        }
        read.value.assign(value, value_length);
        if (spec.value != Value::Voice) {
            read.tag.assign(text + at, read.length);
        }
        *token = read;
        return true;
    }
    return false;
}

}  // namespace detail

inline std::vector<Token> scan(const wchar_t* text, std::size_t length)
{
    std::vector<Token> tokens;
    std::size_t text_begin = 0;
    auto close_text = [&](std::size_t end) {
        if (end > text_begin) {
            Token t;
            t.kind = Kind::Text;
            t.begin = text_begin;
            t.length = end - text_begin;
            tokens.push_back(t);
        }
    };

    std::size_t i = 0;
    while (i < length) {
        Token tag;
        if (text[i] == L'\\' && detail::read_tag(text, length, i, &tag)) {
            close_text(i);
            tokens.push_back(tag);
            i += tag.length;
            text_begin = i;
        } else {
            ++i;
        }
    }
    close_text(length);
    return tokens;
}

class Carried {
public:
    void note(unsigned effects, const std::wstring& tag)
    {
        if (effects == 0 || tag.empty()) {
            return;
        }
        forget(effects);
        entries_.push_back(Entry{effects, tag});
    }

    void forget(unsigned effects)
    {
        keep_if([&](const Entry& entry) { return (entry.effects & ~effects) != 0; });
    }

    void yield_to(unsigned changed)
    {
        keep_if([&](const Entry& entry) { return (entry.effects & changed) == 0; });
    }

    std::wstring tags() const
    {
        std::wstring all;
        for (const Entry& entry : entries_) {
            all += entry.tag;
        }
        return all;
    }

private:
    struct Entry {
        unsigned effects;
        std::wstring tag;
    };

    template <typename Keep>
    void keep_if(Keep keep)
    {
        std::vector<Entry> kept;
        for (const Entry& entry : entries_) {
            if (keep(entry)) {
                kept.push_back(entry);
            }
        }
        entries_.swap(kept);
    }

    std::vector<Entry> entries_;
};

}  // namespace tags
}  // namespace ivx
