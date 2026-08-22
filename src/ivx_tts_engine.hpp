// The SAPI 5 engine object: ISpTTSEngine plus ISpObjectWithToken.
//
// Identical source for both architectures. Everything architecture-specific sits behind
// ivx::SynthBackend, which is either the in-process engine (32-bit) or a pipe to the
// 32-bit helper (64-bit).

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>
#include <comdef.h>
#include <comip.h>

#include "ivx_com.hpp"
#include "ivx_synth.hpp"
#include "ivx_token.hpp"
#include "ivx_voices.hpp"

namespace ivx {
namespace sapi {

// One entry per "\mrk=N\" tag written into the text handed to the engine. Marks carry
// either a SAPI bookmark or a word boundary back out of the engine with an exact audio
// offset attached, which is the whole reason for inserting them.
struct MarkInfo {
    enum class Kind { Bookmark, Word, Sentence };

    Kind kind = Kind::Word;
    std::wstring bookmark_text;  // the text of a SAPI bookmark, verbatim
    LONG bookmark_number = 0;
    ULONG text_offset = 0;
    ULONG text_length = 0;
};

class __declspec(uuid("61b158b8-3639-4b87-8944-9d55320c6f80")) ISpTTSEngineImpl :
    public ISpTTSEngine, public ISpObjectWithToken
{
public:
    ISpTTSEngineImpl();
    ~ISpTTSEngineImpl();

    ISpTTSEngineImpl(const ISpTTSEngineImpl&) = delete;
    ISpTTSEngineImpl& operator=(const ISpTTSEngineImpl&) = delete;

    STDMETHOD(Speak)(DWORD dwSpeakFlags, REFGUID rguidFormatId,
                     const WAVEFORMATEX* pWaveFormatEx, const SPVTEXTFRAG* pTextFragList,
                     ISpTTSEngineSite* pOutputSite) override;
    STDMETHOD(GetOutputFormat)(const GUID* pTargetFmtId, const WAVEFORMATEX* pTargetWaveFormatEx,
                               GUID* pOutputFormatId,
                               WAVEFORMATEX** ppCoMemOutputWaveFormatEx) override;

    STDMETHOD(SetObjectToken)(ISpObjectToken* pToken) override;
    STDMETHOD(GetObjectToken)(ISpObjectToken** ppToken) override;

protected:
    [[nodiscard]] void* get_interface(REFIID riid) noexcept
    {
        void* ptr = com::try_primary_interface<ISpTTSEngine>(this, riid);
        return ptr ? ptr : com::try_interface<ISpObjectWithToken>(this, riid);
    }

private:
    _COM_SMARTPTR_TYPEDEF(ISpObjectToken, __uuidof(ISpObjectToken));
    _COM_SMARTPTR_TYPEDEF(ISpDataKey, __uuidof(ISpDataKey));

    // A stretch of fragments sharing one set of prosody settings, which is as fine-grained
    // as the engine can be told: SAPI 4 sets rate, pitch and volume per utterance.
    struct Run {
        std::wstring text;
        GUID mode = GUID_NULL;  // which voice speaks it; a <lang> tag can change this
        int rate = 0;           // combined SAPI rate, -10..10
        int pitch_adj = 0;      // SAPI middle-pitch adjustment, -10..10
        int volume_pct = 100;   // 0..100
        bool empty() const { return text.empty(); }
    };

    HRESULT ensure_backend();
    HRESULT ranges_for(const GUID& mode, VoiceRanges& out);
    [[nodiscard]] DWORD next_mark_id(MarkInfo info);
    void append_escaped(std::wstring& out, const wchar_t* text, ULONG length);
    void append_words(std::wstring& out, const SPVTEXTFRAG* frag);

    ISpObjectTokenPtr token_;
    VoiceDesc voice_;
    bool have_voice_ = false;

    // Ranges are per voice, and a <lang> tag can pull in a second voice mid-utterance, so
    // they are cached by mode rather than held singly.
    std::map<std::wstring, VoiceRanges> ranges_cache_;

    std::unique_ptr<SynthBackend> backend_;

    std::vector<MarkInfo> marks_;  // index N holds mark id N + 1
    bool want_word_events_ = false;
};

}  // namespace sapi
}  // namespace ivx
