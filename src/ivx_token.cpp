#include "ivx_token.hpp"

#include <new>

#include "ivx_tts_engine.hpp"

namespace ivx {
namespace sapi {

voice_token::voice_token(const VoiceDesc& voice)
{
    set(voice.display_name());

    utils::out_ptr<wchar_t> clsid_str(CoTaskMemFree);
    StringFromCLSID(__uuidof(ISpTTSEngineImpl), clsid_str.address());
    set(L"CLSID", clsid_str.get());

    wchar_t mode[64] = {0};
    StringFromGUID2(voice.mode_guid, mode, 64);

    attributes_[L"Name"] = voice.display_name();
    attributes_[L"Vendor"] = L"Infovox";
    attributes_[L"Language"] = voice.sapi_language_attribute();
    attributes_[L"Gender"] = voice.sapi_gender();
    attributes_[L"Age"] = voice.sapi_age();
    // The token name, not the engine speaker: a user-defined voice has to come back as
    // itself in SetObjectToken, or its settings would be looked up under the wrong name.
    attributes_[kAttrSpeaker] = voice.token_name();
    attributes_[kAttrModeGuid] = mode;
    if (voice.is_custom) {
        attributes_[kAttrBaseSpeaker] = voice.base_speaker;
    }
}

STDMETHODIMP voice_token::OpenKey(LPCWSTR pszSubKeyName, ISpDataKey** ppSubKey)
{
    if (!pszSubKeyName) {
        return E_INVALIDARG;
    }
    if (!ppSubKey) {
        return E_POINTER;
    }
    *ppSubKey = nullptr;

    try {
        if (_wcsicmp(pszSubKeyName, L"Attributes") != 0) {
            return SPERR_NOT_FOUND;
        }

        com::object<ISpDataKeyImpl> obj;
        for (const auto& [key, value] : attributes_) {
            obj->set(key, value);
        }

        com::interface_ptr<ISpDataKey> int_ptr(obj);
        *ppSubKey = int_ptr.get();
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        return E_UNEXPECTED;
    }
}

STDMETHODIMP voice_token::EnumKeys(ULONG Index, LPWSTR* ppszSubKeyName)
{
    if (!ppszSubKeyName) {
        return E_POINTER;
    }
    *ppszSubKeyName = nullptr;

    if (Index > 0) {
        return SPERR_NO_MORE_ITEMS;
    }

    try {
        *ppszSubKeyName = com::strdup(L"Attributes");
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        return E_UNEXPECTED;
    }
}

}  // namespace sapi
}  // namespace ivx
