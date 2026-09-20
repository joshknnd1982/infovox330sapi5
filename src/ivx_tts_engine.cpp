#include "ivx_tts_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <new>

#include "ivx_log.hpp"
#include "ivx_paths.hpp"

namespace ivx {
namespace sapi {
namespace {

// SAPI's rate runs -10..+10 on a logarithmic scale that works out to roughly a third of
// normal speed at one end and three times at the other. The engine's own speed range
// (45..499 words per minute around a default of 150) covers exactly that, so the voice's
// rate is simply scaled. Both spans are per-voice settings now - a user who wants the whole
// slider to stay inside a narrow band, or to reach further, sets them in the configuration
// utility - and these are the defaults an unconfigured voice keeps.
constexpr double kDefaultRateSpanFactor = 3.0;
constexpr double kDefaultPitchSpanFactor = 2.0;

// Audio is handed to SAPI in pieces this size so a stop request is noticed promptly rather
// than after a whole sentence has already been passed over.
constexpr ULONG kWriteChunkBytes = 4096;

[[nodiscard]] bool is_word_char(wchar_t c)
{
    return iswalnum(static_cast<wint_t>(c)) != 0 || c == L'\'' || c == L'-';
}

[[nodiscard]] int clamp_rate(int value)
{
    return (std::max)(-10, (std::min)(10, value));
}

// Feeds one utterance into ISpTTSEngineSite, turning marks back into SAPI events.
class SiteSink : public SynthSink
{
public:
    SiteSink(ISpTTSEngineSite* site, SynthBackend* backend, const std::vector<MarkInfo>& marks,
             std::size_t first_mark, std::size_t end_mark, ULONGLONG stream_base,
             int volume_percent, WORD bits_per_sample)
        : site_(site),
          backend_(backend),
          marks_(marks),
          first_mark_(first_mark),
          reported_(end_mark - first_mark, false),
          stream_base_(stream_base),
          volume_percent_(volume_percent),
          bits_per_sample_(bits_per_sample)
    {
    }

    bool on_audio(const void* data, DWORD size) override
    {
        const BYTE* p = static_cast<const BYTE*>(data);
        ULONG remaining = size;

        // The engine ignores its own volume attribute, so the gain is applied here. A copy
        // is needed because the buffer belongs to the engine.
        if (volume_percent_ < 100) {
            scratch_.assign(p, p + size);
            apply_volume(scratch_.data(), scratch_.size(), volume_percent_, bits_per_sample_);
            p = scratch_.data();
        }

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
        const ULONGLONG offset = stream_base_ + audio_offset;

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
                on_bookmark(bytes_written_, static_cast<DWORD>(first_mark_ + i + 1));
            }
        }
    }

    [[nodiscard]] ULONGLONG bytes_written() const { return bytes_written_; }
    [[nodiscard]] bool aborted() const { return aborted_; }
    [[nodiscard]] bool skipped() const { return skipped_; }

private:
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
    int volume_percent_ = 100;
    WORD bits_per_sample_ = 16;
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

const std::wstring* ISpTTSEngineImpl::substitution_for(const wchar_t* word,
                                                      ULONG length) const
{
    if (settings_.substitutions.empty() || length == 0) {
        return nullptr;
    }
    for (const auto& sub : settings_.substitutions) {
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
void ISpTTSEngineImpl::append_fragment(std::wstring& out, const SPVTEXTFRAG* frag,
                                       bool with_marks)
{
    const wchar_t* text = frag->pTextStart;
    const ULONG length = frag->ulTextLen;

    if (!with_marks && settings_.substitutions.empty()) {
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
            mark.text_offset = frag->ulTextSrcOffset + word_start;
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
                ? substitution_for(text + word_start, word_end - word_start)
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
    settings_ = settings_for_voice(voice_.token_name());
    if (settings_.rate_span <= 0.0) {
        settings_.rate_span = kDefaultRateSpanFactor;
    }
    if (settings_.pitch_span <= 0.0) {
        settings_.pitch_span = kDefaultPitchSpanFactor;
    }
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

        for (const SPVTEXTFRAG* frag = pTextFragList; frag; frag = frag->pNext) {
            const int rate = clamp_rate(static_cast<int>(site_rate) + frag->State.RateAdj);
            const int pitch_adj = clamp_rate(frag->State.PitchAdj.MiddleAdj);
            const int volume_pct =
                (std::max)(0, (std::min)(100, static_cast<int>(site_volume) *
                                                  static_cast<int>(frag->State.Volume) / 100));

            // A <lang> tag naming a language this voice does not speak switches to one that
            // does, for as long as the tag lasts. Without this an English voice would be
            // asked to read French, which it will happily do very badly.
            GUID mode = voice_.mode_guid;
            if (frag->State.LangID != 0 && frag->State.LangID != voice_.sapi_lcid()) {
                const VoiceDesc* alternate =
                    find_voice_for_language(frag->State.LangID, voice_.gender);
                if (alternate) {
                    mode = alternate->mode_guid;
                    IVX_LOG_D("language %04x -> voice '%s'", frag->State.LangID,
                              log_narrow(alternate->speaker.c_str()).c_str());
                } else {
                    IVX_LOG_D("no voice speaks language %04x; staying with '%s'",
                              frag->State.LangID, log_narrow(voice_.speaker.c_str()).c_str());
                }
            }

            if (!have_current || !IsEqualGUID(current.mode, mode) || current.rate != rate ||
                current.pitch_adj != pitch_adj || current.volume_pct != volume_pct) {
                if (have_current && !current.empty()) {
                    current.end_mark = marks_.size();
                    runs.push_back(std::move(current));
                }
                current = Run{};
                current.mode = mode;
                current.rate = rate;
                current.pitch_adj = pitch_adj;
                current.volume_pct = volume_pct;
                current.first_mark = marks_.size();
                have_current = true;
            }

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
                        wchar_t tag[32];
                        _snwprintf_s(tag, _TRUNCATE, L"\\Pau=%u\\", frag->State.SilenceMSecs);
                        current.text.append(tag);
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
                    if (want_sentence_events) {
                        // Carried by a mark like everything else, so the event lands at the
                        // audio offset where the fragment actually starts rather than at
                        // the top of the stream.
                        MarkInfo mark;
                        mark.kind = MarkInfo::Kind::Sentence;
                        mark.text_offset = frag->ulTextSrcOffset;
                        mark.text_length = frag->ulTextLen;
                        wchar_t tag[32];
                        _snwprintf_s(tag, _TRUNCATE, L"\\mrk=%lu\\",
                                     next_mark_id(std::move(mark)));
                        current.text.append(tag);
                    }
                    append_fragment(current.text, frag, want_word_events_);
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
            const DWORD base_speed = ranges.speed.clamped(settings_.rate);
            const DWORD base_pitch = ranges.pitch.clamped(settings_.pitch);

            params.text = run.text;
            params.speed = ranges.speed.supported
                               ? static_cast<int>(ranges.speed.scaled_from(
                                     base_speed, std::pow(settings_.rate_span,
                                                          run.rate / 10.0)))
                               : -1;
            params.pitch = ranges.pitch.supported
                               ? static_cast<int>(ranges.pitch.scaled_from(
                                     base_pitch, std::pow(settings_.pitch_span,
                                                          run.pitch_adj / 10.0)))
                               : -1;
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
            if (!settings_.prefix.empty()) {
                params.text = settings_.prefix + L" " + params.text;
            }

            const int gain = engine_settings.software_volume
                                 ? run.volume_pct * settings_.volume / 100
                                 : 100;

            IVX_LOG_D("run: rate=%d -> speed=%d (base %lu), pitch=%d -> %d (base %lu), "
                      "gain=%d%%, %zu chars",
                      run.rate, params.speed, base_speed, run.pitch_adj, params.pitch,
                      base_pitch, gain, run.text.size());

            SiteSink sink(pOutputSite, backend_.get(), marks_, run.first_mark, run.end_mark,
                          stream_offset, gain,
                          actual.wBitsPerSample ? actual.wBitsPerSample : kExpectedBitsPerSample);
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
