#include "ivx_tts_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <new>

#include "ivx_log.hpp"
#include "ivx_paths.hpp"
#include "ivx_tags.hpp"

namespace ivx {
namespace sapi {
namespace {

// Audio is handed to SAPI in pieces this size so a stop request is noticed promptly rather
// than after a whole sentence has already been passed over.
constexpr ULONG kWriteChunkBytes = 4096;

constexpr unsigned long kLongestPauseMs = 60000;

[[nodiscard]] bool is_word_char(wchar_t c)
{
    return iswalnum(static_cast<wint_t>(c)) != 0 || c == L'\'' || c == L'-';
}

[[nodiscard]] int clamp_rate(int value)
{
    return (std::max)(-10, (std::min)(10, value));
}

[[nodiscard]] bool same_text(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

[[nodiscard]] bool contains_text(std::wstring text, std::wstring part)
{
    CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    CharLowerBuffW(part.data(), static_cast<DWORD>(part.size()));
    return text.find(part) != std::wstring::npos;
}

[[nodiscard]] VoiceSettings settings_of(const VoiceDesc& voice)
{
    return settings_for_voice(voice.token_name());
}

// A Language gets the voice the engine itself would pick: the first male one.
[[nodiscard]] const VoiceDesc* voice_named(const tags::Token& token, const VoiceDesc& now)
{
    if (!token.speaker.empty()) {
        for (const bool loose : {false, true}) {
            for (const auto* voices : {&voice_catalogue(), &builtin_catalogue()}) {
                for (const VoiceDesc& v : *voices) {
                    if (loose ? contains_text(v.display_name(), token.speaker)
                              : same_text(v.token_name(), token.speaker) ||
                                    same_text(v.display_name(), token.speaker)) {
                        return &v;
                    }
                }
            }
        }
    }

    if (!token.language.empty()) {
        std::wstring wanted = token.language;
        if (same_text(wanted, L"English") &&
            (same_text(token.accent, L"American") || same_text(token.accent, L"British"))) {
            wanted = token.accent + L" English";
        } else if (same_text(wanted, L"American") || same_text(wanted, L"British")) {
            wanted += L" English";
        }
        for (const bool loose : {false, true}) {
            const auto speaks = [&](const VoiceDesc& v) {
                return loose ? contains_text(v.language_name, wanted)
                             : same_text(v.language_name, wanted);
            };
            if (speaks(now)) {
                return &now;
            }
            const VoiceDesc* first = nullptr;
            for (const VoiceDesc& v : builtin_catalogue()) {
                if (!speaks(v)) {
                    continue;
                }
                if (v.gender == 2) {
                    return &v;
                }
                if (!first) {
                    first = &v;
                }
            }
            if (first) {
                return first;
            }
        }
    }
    return nullptr;
}

// Feeds one utterance into ISpTTSEngineSite, turning marks back into SAPI events.
class SiteSink : public SynthSink
{
public:
    SiteSink(ISpTTSEngineSite* site, SynthBackend* backend, const std::vector<MarkInfo>& marks,
             std::size_t first_mark, std::size_t end_mark, ULONGLONG stream_base,
             int volume_percent, const WAVEFORMATEX& format)
        : site_(site),
          backend_(backend),
          marks_(marks),
          first_mark_(first_mark),
          reported_(end_mark - first_mark, false),
          stream_base_(stream_base),
          volume_percent_(volume_percent),
          bits_per_sample_(format.wBitsPerSample ? format.wBitsPerSample : kExpectedBitsPerSample),
          bytes_per_second_(format.nAvgBytesPerSec),
          block_align_(format.nBlockAlign ? format.nBlockAlign : 1)
    {
    }

    // A mark arrives ahead of the audio it belongs to, so a pause or a change of volume
    // waits here until the audio reaches it.
    bool on_audio(const void* data, DWORD size) override
    {
        const BYTE* p = static_cast<const BYTE*>(data);
        ULONG remaining = size;
        while (remaining > 0) {
            ULONG take = remaining;
            if (!pending_.empty() && pending_.front().first < delivered_ + remaining) {
                take = static_cast<ULONG>(
                    pending_.front().first > delivered_ ? pending_.front().first - delivered_ : 0);
            }
            if (take > 0) {
                if (!write_audio(p, take)) {
                    return false;
                }
                p += take;
                remaining -= take;
                delivered_ += take;
            }
            if (!release(delivered_)) {
                return false;
            }
        }
        return true;
    }

    void on_bookmark(unsigned __int64 audio_offset, DWORD id) override
    {
        if (id == 0 || id > marks_.size()) {
            IVX_LOG_W("engine reported unknown mark id %lu", id);
            return;
        }
        if (id - 1 >= first_mark_ && id - 1 < first_mark_ + reported_.size()) {
            reported_[id - 1 - first_mark_] = true;
        }
        const MarkInfo& mark = marks_[id - 1];
        if (mark.kind == MarkInfo::Kind::Pause || mark.kind == MarkInfo::Kind::Volume) {
            if (mark.kind == MarkInfo::Kind::Pause) {
                scheduled_ += silence_bytes(mark.silence_ms);
            }
            pending_.emplace_back(audio_offset - audio_offset % block_align_, &mark);
            release(delivered_);
            return;
        }
        const ULONGLONG offset = stream_base_ + scheduled_ + audio_offset;

        SPEVENT event = {};
        event.ulStreamNum = 0;
        event.ullAudioStreamOffset = offset;

        switch (mark.kind) {
            case MarkInfo::Kind::Bookmark:
                event.eEventId = SPEI_TTS_BOOKMARK;
                event.elParamType = SPET_LPARAM_IS_STRING;
                event.lParam = reinterpret_cast<LPARAM>(mark.bookmark_text.c_str());
                event.wParam = static_cast<WPARAM>(mark.bookmark_number);
                IVX_LOG_D("event: bookmark '%s' (%ld) at %llu",
                          log_narrow(mark.bookmark_text.c_str()).c_str(), mark.bookmark_number,
                          offset);
                break;

            case MarkInfo::Kind::Sentence:
                event.eEventId = SPEI_SENTENCE_BOUNDARY;
                event.elParamType = SPET_LPARAM_IS_UNDEFINED;
                event.lParam = static_cast<LPARAM>(mark.text_offset);
                event.wParam = static_cast<WPARAM>(mark.text_length);
                IVX_LOG_D("event: sentence at char %lu len %lu, audio %llu", mark.text_offset,
                          mark.text_length, offset);
                break;

            case MarkInfo::Kind::Word:
            default:
                event.eEventId = SPEI_WORD_BOUNDARY;
                event.elParamType = SPET_LPARAM_IS_UNDEFINED;
                event.lParam = static_cast<LPARAM>(mark.text_offset);
                event.wParam = static_cast<WPARAM>(mark.text_length);
                IVX_LOG_T("event: word at char %lu len %lu, audio %llu", mark.text_offset,
                          mark.text_length, offset);
                break;
        }

        const HRESULT hr = site_->AddEvents(&event, 1);
        if (FAILED(hr)) {
            IVX_LOG_W("AddEvents failed %s", hresult_string(hr).c_str());
        }
    }

    bool should_abort() override { return aborted_ || check_actions(); }

    // The engine drops a mark that comes after the last full stop of its text, so whatever
    // it has not reported by the end is reported there.
    void finish()
    {
        for (std::size_t i = 0; i < reported_.size(); ++i) {
            if (!reported_[i]) {
                on_bookmark(delivered_, static_cast<DWORD>(first_mark_ + i + 1));
            }
        }
        release(~0ull);
    }

    [[nodiscard]] ULONGLONG bytes_written() const { return bytes_written_; }
    [[nodiscard]] bool aborted() const { return aborted_; }
    [[nodiscard]] bool skipped() const { return skipped_; }

private:
    bool write(const BYTE* p, ULONG remaining)
    {
        while (remaining > 0) {
            if (check_actions()) {
                return false;
            }
            const ULONG want = (std::min)(remaining, kWriteChunkBytes);
            ULONG written = 0;
            const HRESULT hr = site_->Write(p, want, &written);
            if (FAILED(hr)) {
                IVX_LOG_E("ISpTTSEngineSite::Write failed %s", hresult_string(hr).c_str());
                aborted_ = true;
                return false;
            }
            if (written == 0 || written > want) {
                IVX_LOG_E("ISpTTSEngineSite::Write accepted %lu of %lu bytes", written, want);
                aborted_ = true;
                return false;
            }
            bytes_written_ += written;
            remaining -= written;
            p += written;
        }
        return true;
    }

    // The engine ignores its own volume attribute, so the gain is applied here. A copy is
    // needed because the buffer belongs to the engine.
    bool write_audio(const BYTE* p, ULONG size)
    {
        const int gain = volume_percent_ * tag_volume_ / 100;
        if (gain < 100) {
            scratch_.assign(p, p + size);
            apply_volume(scratch_.data(), scratch_.size(), gain, bits_per_sample_);
            p = scratch_.data();
        }
        return write(p, size);
    }

    [[nodiscard]] ULONG silence_bytes(ULONG ms) const
    {
        return static_cast<ULONG>(static_cast<ULONGLONG>(ms) * bytes_per_second_ / 1000) /
               block_align_ * block_align_;
    }

    bool release(ULONGLONG reached)
    {
        static const BYTE zeros[kWriteChunkBytes] = {};
        while (!pending_.empty() && pending_.front().first <= reached) {
            const MarkInfo& mark = *pending_.front().second;
            pending_.erase(pending_.begin());
            if (mark.kind == MarkInfo::Kind::Volume) {
                tag_volume_ = mark.volume_percent;
                continue;
            }
            for (ULONG left = silence_bytes(mark.silence_ms); left > 0;) {
                const ULONG chunk = (std::min)(left, kWriteChunkBytes);
                if (!write(zeros, chunk)) {
                    return false;
                }
                left -= chunk;
            }
        }
        return true;
    }

    // Returns true when the utterance should stop.
    bool check_actions()
    {
        if (aborted_) {
            return true;
        }
        const DWORD actions = site_->GetActions();
        if (actions & SPVES_ABORT) {
            IVX_LOG_I("SAPI asked to abort");
            aborted_ = true;
        } else if (actions & SPVES_SKIP) {
            IVX_LOG_I("SAPI asked to skip; the engine has no skip, reporting none skipped");
            site_->CompleteSkip(0);
            skipped_ = true;
            aborted_ = true;
        }
        if (aborted_ && backend_) {
            backend_->abort_current();
        }
        return aborted_;
    }

    ISpTTSEngineSite* site_;
    SynthBackend* backend_;
    const std::vector<MarkInfo>& marks_;
    std::size_t first_mark_;
    std::vector<bool> reported_;
    ULONGLONG stream_base_;
    ULONGLONG bytes_written_ = 0;
    ULONGLONG delivered_ = 0;
    ULONGLONG scheduled_ = 0;
    std::vector<std::pair<ULONGLONG, const MarkInfo*>> pending_;
    int volume_percent_ = 100;
    int tag_volume_ = 100;
    WORD bits_per_sample_ = 16;
    DWORD bytes_per_second_ = 0;
    WORD block_align_ = 1;
    std::vector<BYTE> scratch_;
    bool aborted_ = false;
    bool skipped_ = false;
};

}  // namespace

ISpTTSEngineImpl::ISpTTSEngineImpl() = default;

ISpTTSEngineImpl::~ISpTTSEngineImpl() = default;

HRESULT ISpTTSEngineImpl::ensure_backend()
{
    if (!backend_) {
        backend_.reset(create_backend());
        if (!backend_) {
            return E_OUTOFMEMORY;
        }
    }
    return backend_->ensure_ready();
}

HRESULT ISpTTSEngineImpl::ranges_for(const GUID& mode, VoiceRanges& out)
{
    wchar_t key_buf[64] = {0};
    StringFromGUID2(mode, key_buf, 64);
    const std::wstring key = key_buf;

    const auto cached = ranges_cache_.find(key);
    if (cached != ranges_cache_.end()) {
        out = cached->second;
        return S_OK;
    }

    VoiceRanges ranges;
    const HRESULT hr = backend_->get_ranges(mode, ranges);
    if (FAILED(hr)) {
        return hr;
    }
    ranges_cache_[key] = ranges;
    out = ranges;
    return S_OK;
}

DWORD ISpTTSEngineImpl::next_mark_id(MarkInfo info)
{
    marks_.push_back(std::move(info));
    return static_cast<DWORD>(marks_.size());  // ids are 1-based
}

void ISpTTSEngineImpl::append_mark(Run& run, MarkInfo mark)
{
    wchar_t tag[32];
    _snwprintf_s(tag, _TRUNCATE, L"\\mrk=%lu\\", next_mark_id(std::move(mark)));
    run.text.append(tag);
}

// Neither \Pau= nor \Vol= changes the audio the engine hands back, so those two become marks
// that the audio is paused or turned down at; every other tag goes to the engine as it is.
void ISpTTSEngineImpl::append_tags(Run& run, const std::wstring& tagged)
{
    for (const tags::Token& token : tags::scan(tagged.data(), tagged.size())) {
        if (token.kind == tags::Kind::Pause) {
            MarkInfo mark;
            mark.kind = MarkInfo::Kind::Pause;
            mark.silence_ms = (std::min)(token.number, kLongestPauseMs);
            append_mark(run, std::move(mark));
        } else if (token.kind == tags::Kind::Volume) {
            MarkInfo mark;
            mark.kind = MarkInfo::Kind::Volume;
            mark.volume_percent = static_cast<int>((std::min)(token.number, 65535ul) * 100 / 65535);
            append_mark(run, std::move(mark));
        } else {
            run.text.append(tagged, token.begin, token.length);
        }
    }
}

void ISpTTSEngineImpl::append_escaped(std::wstring& out, const wchar_t* text, ULONG length)
{
    // Tagged text treats a backslash as the start of a command, so literal ones are doubled.
    out.reserve(out.size() + length + 8);
    for (ULONG i = 0; i < length; ++i) {
        const wchar_t c = text[i];
        if (c == L'\\') {
            out.append(L"\\\\");
        } else {
            out.push_back(c);
        }
    }
}

const std::wstring* ISpTTSEngineImpl::substitution_for(const Run& run, const wchar_t* word,
                                                      ULONG length)
{
    if (run.settings.substitutions.empty() || length == 0) {
        return nullptr;
    }
    for (const auto& sub : run.settings.substitutions) {
        if (sub.from.size() == length &&
            _wcsnicmp(sub.from.c_str(), word, length) == 0) {
            return &sub.to;
        }
    }
    return nullptr;
}

// Marks and substitutions both work on whitespace-delimited runs, so one walk does both.
// with_marks is false when the application has not asked for word events, and then this is
// only here to give substitutions somewhere to happen.
void ISpTTSEngineImpl::append_fragment(Run& run, const wchar_t* text, ULONG length,
                                       ULONG source_offset, bool with_marks)
{
    std::wstring& out = run.text;

    if (!with_marks && run.settings.substitutions.empty()) {
        append_escaped(out, text, length);
        return;
    }

    ULONG i = 0;
    while (i < length) {
        if (iswspace(static_cast<wint_t>(text[i]))) {
            append_escaped(out, text + i, 1);
            ++i;
            continue;
        }

        // A mark is only ever placed at the start of a whitespace-delimited run, never
        // inside one. The engine tokenises the text itself, and a mark landing in the
        // middle of a token destroys that: "\mrk\145.\mrk\2" makes it read the number as
        // something thirteen times longer than "145.2", which is heard as the rate
        // collapsing. Runs stay intact; only the gaps between them are marked.
        const ULONG start = i;
        while (i < length && !iswspace(static_cast<wint_t>(text[i]))) {
            ++i;
        }
        const ULONG end = i;

        // The event still reports the word itself rather than the whole run, so a caret or
        // a highlight lands on "world" and not on "world," - the mark's audio offset is
        // the run's, which is the same instant.
        ULONG word_start = start;
        while (word_start < end && !is_word_char(text[word_start])) {
            ++word_start;
        }
        ULONG word_end = end;
        while (word_end > word_start && !is_word_char(text[word_end - 1])) {
            --word_end;
        }

        if (with_marks && word_start < word_end) {
            MarkInfo mark;
            mark.kind = MarkInfo::Kind::Word;
            mark.text_offset = source_offset + word_start;
            mark.text_length = word_end - word_start;

            wchar_t tag[32];
            _snwprintf_s(tag, _TRUNCATE, L"\\mrk=%lu\\", next_mark_id(std::move(mark)));
            out.append(tag);
        }

        // A substitution replaces the word inside the run and leaves whatever punctuation
        // surrounds it alone, so "Larry's," keeps its apostrophe and its comma. The event
        // offsets above still describe the original text, which is what an application
        // highlighting the source needs.
        const std::wstring* replacement =
            (word_start < word_end)
                ? substitution_for(run, text + word_start, word_end - word_start)
                : nullptr;
        if (replacement) {
            append_escaped(out, text + start, word_start - start);
            append_escaped(out, replacement->c_str(),
                           static_cast<ULONG>(replacement->size()));
            append_escaped(out, text + word_end, end - word_end);
        } else {
            append_escaped(out, text + start, end - start);
        }
    }
}

void ISpTTSEngineImpl::refresh_settings()
{
    const unsigned generation = config_generation();
    if (generation == settings_generation_ && settings_generation_ != 0) {
        return;
    }
    settings_generation_ = generation;
    settings_ = settings_of(voice_);
}

STDMETHODIMP ISpTTSEngineImpl::SetObjectToken(ISpObjectToken* pToken)
{
    if (!pToken) {
        return E_INVALIDARG;
    }

    try {
        ISpDataKeyPtr attributes;
        if (FAILED(pToken->OpenKey(L"Attributes", &attributes)) || !attributes) {
            IVX_LOG_E("SetObjectToken: the token has no Attributes key");
            return E_INVALIDARG;
        }

        std::wstring speaker;
        utils::out_ptr<wchar_t> value(CoTaskMemFree);
        if (SUCCEEDED(attributes->GetStringValue(kAttrSpeaker, value.address())) && value.get()) {
            speaker = value.get();
        } else if (SUCCEEDED(attributes->GetStringValue(L"Name", value.address())) && value.get()) {
            // A token written by hand, or one whose display name is all we have. The name is
            // "Infovox 330 <speaker> - <language>", so take the middle word out of it.
            const std::wstring name = value.get();
            const std::size_t dash = name.rfind(L" - ");
            std::wstring trimmed = dash == std::wstring::npos ? name : name.substr(0, dash);
            const std::size_t space = trimmed.rfind(L' ');
            speaker = space == std::wstring::npos ? trimmed : trimmed.substr(space + 1);
        }

        if (speaker.empty()) {
            IVX_LOG_E("SetObjectToken: could not work out which voice the token means");
            return E_INVALIDARG;
        }

        const VoiceDesc* voice = find_voice(speaker);
        if (!voice) {
            IVX_LOG_E("SetObjectToken: no voice named '%s' in the catalogue",
                      log_narrow(speaker.c_str()).c_str());
            return SPERR_NOT_FOUND;
        }

        voice_ = *voice;
        have_voice_ = true;
        token_ = pToken;
        settings_generation_ = 0;
        refresh_settings();
        if (voice_.is_custom) {
            IVX_LOG_I("voice set to the user-defined voice '%s', speaking through '%s', "
                      "language %s", log_narrow(voice_.custom_name.c_str()).c_str(),
                      log_narrow(voice_.base_speaker.c_str()).c_str(),
                      log_narrow(voice_.sapi_language_attribute().c_str()).c_str());
        } else {
            IVX_LOG_I("voice set to '%s' (%s), language %s",
                      log_narrow(voice_.speaker.c_str()).c_str(),
                      log_narrow(voice_.language_name.c_str()).c_str(),
                      log_narrow(voice_.sapi_language_attribute().c_str()).c_str());
        }
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        return E_UNEXPECTED;
    }
}

STDMETHODIMP ISpTTSEngineImpl::GetObjectToken(ISpObjectToken** ppToken)
{
    if (!ppToken) {
        return E_POINTER;
    }
    *ppToken = nullptr;
    if (!token_) {
        return E_UNEXPECTED;
    }
    token_.AddRef();
    *ppToken = token_.GetInterfacePtr();
    return S_OK;
}

STDMETHODIMP ISpTTSEngineImpl::GetOutputFormat(const GUID* /*pTargetFmtId*/,
                                               const WAVEFORMATEX* /*pTargetWaveFormatEx*/,
                                               GUID* pOutputFormatId,
                                               WAVEFORMATEX** ppCoMemOutputWaveFormatEx)
{
    if (!pOutputFormatId || !ppCoMemOutputWaveFormatEx) {
        return E_POINTER;
    }
    *ppCoMemOutputWaveFormatEx = nullptr;
    *pOutputFormatId = SPDFID_WaveFormatEx;

    auto* wfx = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (!wfx) {
        return E_OUTOFMEMORY;
    }

    // Every Infovox 330 voice is 16 kHz / 16-bit / mono. Reporting it without starting the
    // engine keeps voice selection cheap; Speak() checks the engine agrees and complains
    // loudly in the log if a voice ever turns up that does not.
    fill_expected_format(*wfx);
    *ppCoMemOutputWaveFormatEx = wfx;
    IVX_LOG_D("GetOutputFormat: %lu Hz, %u-bit, %u channel(s)", wfx->nSamplesPerSec,
              wfx->wBitsPerSample, wfx->nChannels);
    return S_OK;
}

STDMETHODIMP ISpTTSEngineImpl::Speak(DWORD dwSpeakFlags, REFGUID /*rguidFormatId*/,
                                     const WAVEFORMATEX* /*pWaveFormatEx*/,
                                     const SPVTEXTFRAG* pTextFragList,
                                     ISpTTSEngineSite* pOutputSite)
{
    if (!pTextFragList || !pOutputSite) {
        return E_INVALIDARG;
    }
    if (!have_voice_) {
        IVX_LOG_E("Speak called before a voice token was set");
        return SPERR_UNINITIALIZED;
    }

    try {
        HRESULT hr = ensure_backend();
        if (FAILED(hr)) {
            IVX_LOG_E("engine not available: %s", hresult_string(hr).c_str());
            return hr;
        }

        WAVEFORMATEX actual{};
        backend_->output_format(actual);
        if (actual.nSamplesPerSec != kExpectedSampleRate ||
            actual.wBitsPerSample != kExpectedBitsPerSample ||
            actual.nChannels != kExpectedChannels) {
            IVX_LOG_E("voice '%s' produces %lu Hz/%u-bit/%uch but SAPI was told %lu Hz/%u-bit/%uch",
                      log_narrow(voice_.speaker.c_str()).c_str(), actual.nSamplesPerSec,
                      actual.wBitsPerSample, actual.nChannels, kExpectedSampleRate,
                      kExpectedBitsPerSample, kExpectedChannels);
        }

        refresh_settings();
        const EngineSettings& engine_settings = config().engine;

        ULONGLONG event_interest = 0;
        pOutputSite->GetEventInterest(&event_interest);
        // Each mark costs a tag in the text the engine parses. A user who does not need a
        // moving highlight can turn them off and have the engine see clean text.
        want_word_events_ = engine_settings.word_events &&
                            (event_interest & SPFEI(SPEI_WORD_BOUNDARY)) != 0;
        const bool want_sentence_events =
            engine_settings.sentence_events &&
            (event_interest & SPFEI(SPEI_SENTENCE_BOUNDARY)) != 0;

        long site_rate = 0;
        pOutputSite->GetRate(&site_rate);
        USHORT site_volume = 100;
        pOutputSite->GetVolume(&site_volume);

        IVX_LOG_I("Speak: flags=0x%lX rate=%ld volume=%u interest=0x%llX voice='%s'",
                  dwSpeakFlags, site_rate, site_volume, event_interest,
                  log_narrow(voice_.speaker.c_str()).c_str());

        marks_.clear();

        // SAPI 4 sets rate, pitch and volume for a whole utterance, so consecutive fragments
        // that agree about prosody are merged into one call and a change starts a new one.
        std::vector<Run> runs;
        Run current;
        bool have_current = false;

        VoiceDesc speaking = voice_;
        VoiceSettings speaking_settings = settings_;
        unsigned speaking_serial = 0;
        tags::Carried carried;

        for (const SPVTEXTFRAG* frag = pTextFragList; frag; frag = frag->pNext) {
            const int rate = clamp_rate(static_cast<int>(site_rate) + frag->State.RateAdj);
            const int pitch_adj = clamp_rate(frag->State.PitchAdj.MiddleAdj);
            const int volume_pct =
                (std::max)(0, (std::min)(100, static_cast<int>(site_volume) *
                                                  static_cast<int>(frag->State.Volume) / 100));

            // A <lang> tag naming a language this voice does not speak switches to one that
            // does, for as long as the tag lasts. Without this an English voice would be
            // asked to read French, which it will happily do very badly.
            const auto language_voice = [&] {
                GUID mode = speaking.mode_guid;
                if (frag->State.LangID != 0 && frag->State.LangID != voice_.sapi_lcid() &&
                    frag->State.LangID != speaking.sapi_lcid()) {
                    const VoiceDesc* alternate =
                        find_voice_for_language(frag->State.LangID, speaking.gender);
                    if (alternate) {
                        mode = alternate->mode_guid;
                        IVX_LOG_D("language %04x -> voice '%s'", frag->State.LangID,
                                  log_narrow(alternate->speaker.c_str()).c_str());
                    } else {
                        IVX_LOG_D("no voice speaks language %04x; staying with '%s'",
                                  frag->State.LangID,
                                  log_narrow(speaking.speaker.c_str()).c_str());
                    }
                }
                return mode;
            };
            GUID mode = language_voice();

            const auto open_run = [&] {
                if (have_current && IsEqualGUID(current.mode, mode) && current.rate == rate &&
                    current.pitch_adj == pitch_adj && current.volume_pct == volume_pct &&
                    current.voice == speaking_serial) {
                    return;
                }
                if (have_current) {
                    carried.yield_to(
                        (rate != current.rate ? tags::effect::kRate : 0u) |
                        (pitch_adj != current.pitch_adj ? tags::effect::kPitch : 0u) |
                        (volume_pct != current.volume_pct ? tags::effect::kVolume : 0u));
                    if (!current.empty()) {
                        current.end_mark = marks_.size();
                        runs.push_back(std::move(current));
                    }
                }
                current = Run{};
                current.mode = mode;
                current.rate = rate;
                current.pitch_adj = pitch_adj;
                current.volume_pct = volume_pct;
                current.settings = speaking_settings;
                current.voice = speaking_serial;
                current.first_mark = marks_.size();
                have_current = true;
                append_tags(current, carried.tags());
                current.carried = current.text.size();
            };
            open_run();

            switch (frag->State.eAction) {
                case SPVA_Bookmark: {
                    MarkInfo mark;
                    mark.kind = MarkInfo::Kind::Bookmark;
                    if (frag->ulTextLen > 0 && frag->pTextStart) {
                        mark.bookmark_text.assign(frag->pTextStart, frag->ulTextLen);
                        mark.bookmark_number = wcstol(mark.bookmark_text.c_str(), nullptr, 10);
                    }
                    mark.text_offset = frag->ulTextSrcOffset;
                    mark.text_length = frag->ulTextLen;
                    wchar_t tag[32];
                    _snwprintf_s(tag, _TRUNCATE, L"\\mrk=%lu\\", next_mark_id(std::move(mark)));
                    current.text.append(tag);
                    break;
                }

                case SPVA_Silence: {
                    if (frag->State.SilenceMSecs > 0) {
                        MarkInfo mark;
                        mark.kind = MarkInfo::Kind::Pause;
                        mark.silence_ms = (std::min)(
                            static_cast<unsigned long>(frag->State.SilenceMSecs), kLongestPauseMs);
                        append_mark(current, std::move(mark));
                    }
                    break;
                }

                case SPVA_SpellOut: {
                    if (frag->ulTextLen > 0 && frag->pTextStart) {
                        current.text.append(L"\\RmS=1\\");
                        append_escaped(current.text, frag->pTextStart, frag->ulTextLen);
                        current.text.append(L"\\RmS=0\\");
                    }
                    break;
                }

                case SPVA_ParseUnknownTag: {
                    // A tag SAPI did not recognise. If it looks like engine tagged text it
                    // is passed straight through unescaped, which is the only route by
                    // which an application can reach an Infovox control SAPI has no
                    // concept of. The engine ignores tags it does not know rather than
                    // speaking them, so a wrong guess here is silent rather than audible.
                    if (frag->ulTextLen == 0 || !frag->pTextStart) {
                        break;
                    }
                    ULONG start = 0;
                    while (start < frag->ulTextLen && iswspace(frag->pTextStart[start])) {
                        ++start;
                    }
                    if (start < frag->ulTextLen && frag->pTextStart[start] == L'\\') {
                        current.text.append(frag->pTextStart + start, frag->ulTextLen - start);
                        IVX_LOG_D("passed an engine tag through verbatim: %s",
                                  log_narrow(std::wstring(frag->pTextStart, frag->ulTextLen)
                                                 .c_str())
                                      .c_str());
                    } else {
                        IVX_LOG_D("ignored an unknown tag that is not engine tagged text");
                    }
                    break;
                }

                case SPVA_Speak:
                case SPVA_Pronounce:
                default: {
                    // There is no way to map SAPI phoneme ids onto this engine's phoneme
                    // set, so a Pronounce fragment is spoken as the text it came from.
                    if (frag->ulTextLen == 0 || !frag->pTextStart) {
                        break;
                    }
                    bool sentence_due = want_sentence_events;
                    const auto speak_text = [&](const wchar_t* text, ULONG length, ULONG source) {
                        open_run();
                        if (sentence_due) {
                            // Carried by a mark like everything else, so the event lands at the
                            // audio offset where the fragment actually starts rather than at
                            // the top of the stream.
                            MarkInfo mark;
                            mark.kind = MarkInfo::Kind::Sentence;
                            mark.text_offset = frag->ulTextSrcOffset;
                            mark.text_length = frag->ulTextLen;
                            append_mark(current, std::move(mark));
                            sentence_due = false;
                        }
                        append_fragment(current, text, length, source, want_word_events_);
                    };
                    if (!engine_settings.control_tags) {
                        speak_text(frag->pTextStart, frag->ulTextLen, frag->ulTextSrcOffset);
                        break;
                    }
                    for (const tags::Token& token : tags::scan(frag->pTextStart, frag->ulTextLen)) {
                        const ULONG source = frag->ulTextSrcOffset + static_cast<ULONG>(token.begin);
                        switch (token.kind) {
                            case tags::Kind::Text:
                                speak_text(frag->pTextStart + token.begin,
                                           static_cast<ULONG>(token.length), source);
                                break;

                            case tags::Kind::Bookmark: {
                                MarkInfo mark;
                                mark.kind = MarkInfo::Kind::Bookmark;
                                mark.bookmark_text = token.value;
                                mark.bookmark_number = static_cast<LONG>(token.number);
                                mark.text_offset = source;
                                mark.text_length = static_cast<ULONG>(token.length);
                                open_run();
                                append_mark(current, std::move(mark));
                                break;
                            }

                            case tags::Kind::Voice: {
                                const VoiceDesc* chosen = voice_named(token, speaking);
                                if (!chosen && (!token.speaker.empty() || !token.language.empty())) {
                                    IVX_LOG_W("no installed voice answers to \\Vce=%s\\",
                                              log_narrow(token.value.c_str()).c_str());
                                } else if (chosen &&
                                           !same_text(chosen->token_name(), speaking.token_name())) {
                                    IVX_LOG_D("\\Vce= in the text: '%s' -> '%s'",
                                              log_narrow(speaking.token_name().c_str()).c_str(),
                                              log_narrow(chosen->token_name().c_str()).c_str());
                                    speaking = *chosen;
                                    speaking_settings = settings_of(speaking);
                                    ++speaking_serial;
                                    mode = language_voice();
                                    carried.forget(tags::effect::kPitch | tags::effect::kVoiceParams);
                                }
                                if (!token.tag.empty()) {
                                    open_run();
                                    append_tags(current, token.tag);
                                    carried.note(token.effects, token.tag);
                                }
                                break;
                            }

                            default:
                                open_run();
                                append_tags(current, token.tag);
                                carried.note(token.effects, token.tag);
                                break;
                        }
                    }
                    break;
                }
            }
        }
        if (have_current && !current.empty()) {
            current.end_mark = marks_.size();
            runs.push_back(std::move(current));
        }

        if (runs.empty()) {
            IVX_LOG_I("Speak: nothing to say");
            return S_OK;
        }

        ULONGLONG stream_offset = 0;
        for (const Run& run : runs) {
            const DWORD actions = pOutputSite->GetActions();
            if (actions & SPVES_ABORT) {
                break;
            }
            if (actions & SPVES_SKIP) {
                pOutputSite->CompleteSkip(0);
                break;
            }
            if (actions & SPVES_RATE) {
                pOutputSite->GetRate(&site_rate);
            }
            if (actions & SPVES_VOLUME) {
                pOutputSite->GetVolume(&site_volume);
            }

            VoiceRanges ranges;
            const HRESULT ranges_hr = ranges_for(run.mode, ranges);
            if (FAILED(ranges_hr)) {
                IVX_LOG_E("could not read the voice's adjustable ranges: %s",
                          hresult_string(ranges_hr).c_str());
                return ranges_hr;
            }

            SpeakParams params;
            params.mode = run.mode;

            // The voice's own rate and pitch are the neutral point that SAPI's -10..+10
            // moves around, so a voice configured to speak at 220 words per minute still
            // has the whole slider either side of 220 rather than either side of 150.
            const DWORD base_speed = ranges.speed.clamped(run.settings.rate);
            const DWORD base_pitch = ranges.pitch.clamped(run.settings.pitch);

            const auto reach = [](const AttrRange& range, DWORD base, double span, int step) {
                if (!range.supported) {
                    return -1;
                }
                return static_cast<int>(span > 0.0
                                            ? range.scaled_from(base, std::pow(span, step / 10.0))
                                            : range.stepped_from(base, step));
            };
            params.text = run.text;
            params.speed = reach(ranges.speed, base_speed, run.settings.rate_span, run.rate);
            params.pitch = reach(ranges.pitch, base_pitch, run.settings.pitch_span, run.pitch_adj);
            // The engine's own volume control was measured to do nothing - it accepts a
            // value and produces byte-identical audio - so it is pinned at maximum and the
            // gain is applied to the samples in SiteSink instead. Setting it anyway is
            // offered in the configuration utility for anyone who wants to see for
            // themselves.
            if (ranges.volume.supported) {
                params.volume = engine_settings.set_engine_volume
                                    ? static_cast<int>(ranges.volume.from_percent(
                                          run.volume_pct))
                                    : static_cast<int>(ranges.volume.max_value);
            } else {
                params.volume = -1;
            }
            params.realtime = engine_settings.realtime;

            // Whatever the user put in the voice's tag prefix, in front of every utterance.
            // The space matters: tagged text escapes a literal backslash as "\\", so a
            // prefix ending in one butted straight against a tag would swallow that tag.
            if (!run.settings.prefix.empty()) {
                params.text = run.settings.prefix + L" " + params.text;
            }

            const int gain = engine_settings.software_volume
                                 ? run.volume_pct * run.settings.volume / 100
                                 : 100;

            IVX_LOG_D("run: rate=%d -> speed=%d (base %lu), pitch=%d -> %d (base %lu), "
                      "gain=%d%%, %zu chars",
                      run.rate, params.speed, base_speed, run.pitch_adj, params.pitch,
                      base_pitch, gain, run.text.size());

            SiteSink sink(pOutputSite, backend_.get(), marks_, run.first_mark, run.end_mark,
                          stream_offset, gain, actual);
            const HRESULT run_hr = backend_->speak(params, sink);
            if (SUCCEEDED(run_hr) && !sink.aborted()) {
                sink.finish();
            }
            stream_offset += sink.bytes_written();

            if (FAILED(run_hr)) {
                IVX_LOG_E("run failed: %s", hresult_string(run_hr).c_str());
                return run_hr;
            }
            if (sink.aborted()) {
                break;
            }
        }

        IVX_LOG_I("Speak finished, %llu bytes", stream_offset);
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        IVX_LOG_E("Speak threw an unexpected exception");
        return E_UNEXPECTED;
    }
}

}  // namespace sapi
}  // namespace ivx
