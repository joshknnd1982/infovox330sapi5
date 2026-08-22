// The voice enumerator SAPI calls through the TokenEnums key.
//
// Registering one enumerator rather than sixteen individual voice tokens keeps the
// registry footprint to a single key, and means adding or removing a voice is a matter of
// editing VoiceDescriptions.txt rather than re-running the installer.

#pragma once

#include <vector>

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>
#include <comdef.h>
#include <comip.h>

#include "ivx_com.hpp"
#include "ivx_token.hpp"
#include "ivx_voices.hpp"

namespace ivx {
namespace sapi {

class __declspec(uuid("abbd81fa-fbf7-4e21-a119-784f25f60a7c")) IEnumSpObjectTokensImpl :
    public IEnumSpObjectTokens
{
public:
    explicit IEnumSpObjectTokensImpl(bool initialize = true);

    IEnumSpObjectTokensImpl(const IEnumSpObjectTokensImpl&) = delete;
    IEnumSpObjectTokensImpl& operator=(const IEnumSpObjectTokensImpl&) = delete;

    STDMETHOD(Next)(ULONG celt, ISpObjectToken** pelt, ULONG* pceltFetched) override;
    STDMETHOD(Skip)(ULONG celt) override;
    STDMETHOD(Reset)() override;
    STDMETHOD(Clone)(IEnumSpObjectTokens** ppEnum) override;
    STDMETHOD(Item)(ULONG Index, ISpObjectToken** ppToken) override;
    STDMETHOD(GetCount)(ULONG* pulCount) override;

protected:
    [[nodiscard]] void* get_interface(REFIID riid) noexcept
    {
        return com::try_primary_interface<IEnumSpObjectTokens>(this, riid);
    }

private:
    _COM_SMARTPTR_TYPEDEF(ISpObjectToken, __uuidof(ISpObjectToken));
    _COM_SMARTPTR_TYPEDEF(ISpObjectTokenInit, __uuidof(ISpObjectTokenInit));

    [[nodiscard]] ISpObjectTokenPtr create_token(const VoiceDesc& voice) const;

    std::size_t index_;
    std::vector<VoiceDesc> voices_;
};

}  // namespace sapi
}  // namespace ivx
