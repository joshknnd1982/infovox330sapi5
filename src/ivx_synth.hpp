// Types shared by both halves of the synthesiser: the 32-bit in-process engine and the
// 64-bit client that talks to the 32-bit helper. The SAPI 5 layer is written against these
// alone, so it compiles unchanged for either architecture.

#pragma once

#include <windows.h>
#include <objbase.h>
#include <mmreg.h>  // WAVEFORMATEX; windows.h omits it under WIN32_LEAN_AND_MEAN

#include <cstddef>
#include <string>

namespace ivx {

// One of the engine's three adjustable attributes, as the engine itself reports it.
struct AttrRange {
    bool supported = false;
    DWORD min_value = 0;
    DWORD max_value = 0;
    DWORD default_value = 0;

    [[nodiscard]] DWORD from_percent(int percent) const;
    [[nodiscard]] int to_percent(DWORD value) const;

    // Scales the default by factor, staying inside the engine's own limits. This is how a
    // SAPI rate or pitch adjustment becomes an engine value: both are logarithmic, and the
    // engine's default is the neutral point.
    [[nodiscard]] DWORD scaled(double factor) const;

    // The same, from a base the caller chose. A voice configured with its own rate makes
    // that rate the neutral point instead of the engine's, so SAPI's zero means what the
    // user set rather than what the engine shipped with.
    [[nodiscard]] DWORD scaled_from(DWORD base, double factor) const;

    // Brings a configured value inside the engine's limits. A negative value, which is how
    // "not configured" is stored, gives the engine's own default.
    [[nodiscard]] DWORD clamped(int value) const;
};

struct VoiceRanges {
    AttrRange speed;
    AttrRange pitch;
    AttrRange volume;
};

// Receives the synthesised stream. All calls happen on the thread that called speak().
class SynthSink {
public:
    virtual ~SynthSink() = default;

    // Returns false to abort the rest of the utterance.
    virtual bool on_audio(const void* data, DWORD size) = 0;

    // A "\mrk=id\" tag was reached, at audio_offset bytes into this utterance.
    virtual void on_bookmark(unsigned __int64 audio_offset, DWORD id) = 0;

    // The engine marks the start of every word it speaks. Marks that belong to a "\mrk\"
    // tag arrive through on_bookmark instead, so these are plain word boundaries.
    virtual void on_word_boundary(unsigned __int64 /*audio_offset*/) {}

    // Polled between chunks so a caller can abort without waiting for more audio.
    virtual bool should_abort() { return false; }
};

struct SpeakParams {
    GUID mode = GUID_NULL;
    std::wstring text;  // already carries "\mrk=..\", "\Pau=..\" and friends
    int speed = -1;     // engine-native value, -1 leaves the current setting
    int pitch = -1;     // engine-native value, -1 leaves the current setting
    int volume = -1;    // engine-native value, -1 leaves the current setting
    // SAPI 4's fourth attribute. The engine reports 0x7FFFFFFF for it and no SAPI 5 concept
    // maps onto it, so nothing sets it unless the user asks for it explicitly through the
    // configuration utility. -1 leaves it alone.
    int realtime = -1;
};

// The wave format the engine produces. Infovox 330 is fixed at 16 kHz / 16-bit / mono; this
// is confirmed against the format the engine actually asks for and logged if it differs.
inline constexpr DWORD kExpectedSampleRate = 16000;
inline constexpr WORD kExpectedBitsPerSample = 16;
inline constexpr WORD kExpectedChannels = 1;

void fill_expected_format(WAVEFORMATEX& wfx);

// Infovox 330 reports a volume attribute, accepts writes to it, and then produces
// byte-identical audio whatever it is set to - measured across the whole range. Volume
// therefore has to be applied to the samples here, or a SAPI volume slider would do
// nothing at all. Scaling is linear in amplitude, which is what SAPI's percentage means.
void apply_volume(void* samples, std::size_t bytes, int percent, WORD bits_per_sample);

// What the SAPI 5 layer talks to. Implemented in-process on 32-bit and over a pipe to the
// 32-bit helper on 64-bit.
class SynthBackend {
public:
    virtual ~SynthBackend() = default;

    virtual HRESULT ensure_ready() = 0;
    virtual void output_format(WAVEFORMATEX& out) = 0;
    virtual HRESULT get_ranges(const GUID& mode, VoiceRanges& out) = 0;
    virtual HRESULT speak(const SpeakParams& params, SynthSink& sink) = 0;

    // Safe to call from any thread, including while speak() is running.
    virtual void abort_current() = 0;
};

// Built by whichever backend this architecture was compiled with.
[[nodiscard]] SynthBackend* create_backend();

}  // namespace ivx
