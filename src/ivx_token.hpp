// A SAPI 5 voice token backed by one entry from VoiceDescriptions.txt.
//
// The token is built in memory rather than read back out of the registry, so installing a
// voice costs nothing beyond the one TokenEnums key that points SAPI at our enumerator.

#pragma once

#include <map>
#include <string>

#include "ivx_datakey.hpp"
#include "ivx_voices.hpp"

namespace ivx {
namespace sapi {

// Attribute names carrying our own data through the token. SAPI passes the whole token to
// the engine, so SetObjectToken can recover the exact voice without guessing from the
// display name.
inline constexpr wchar_t kAttrSpeaker[] = L"InfovoxSpeaker";
inline constexpr wchar_t kAttrModeGuid[] = L"InfovoxModeGuid";

class voice_token : public ISpDataKeyImpl
{
public:
    explicit voice_token(const VoiceDesc& voice);

    STDMETHOD(OpenKey)(LPCWSTR pszSubKeyName, ISpDataKey** ppSubKey) override;
    STDMETHOD(EnumKeys)(ULONG Index, LPWSTR* ppszSubKeyName) override;

private:
    using attribute_map = std::map<std::wstring, std::wstring, str_less>;

    attribute_map attributes_;
};

}  // namespace sapi
}  // namespace ivx
