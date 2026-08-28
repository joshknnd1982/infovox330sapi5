#include "ivx_synth.hpp"

#include <algorithm>
#include <cmath>

namespace ivx {

DWORD AttrRange::from_percent(int percent) const
{
    if (!supported || max_value <= min_value) {
        return default_value;
    }
    percent = (std::max)(0, (std::min)(100, percent));
    const double span = static_cast<double>(max_value) - static_cast<double>(min_value);
    return min_value + static_cast<DWORD>(span * percent / 100.0 + 0.5);
}

int AttrRange::to_percent(DWORD value) const
{
    if (!supported || max_value <= min_value) {
        return 50;
    }
    value = (std::max)(min_value, (std::min)(max_value, value));
    const double span = static_cast<double>(max_value) - static_cast<double>(min_value);
    return static_cast<int>((value - min_value) * 100.0 / span + 0.5);
}

DWORD AttrRange::scaled(double factor) const
{
    return scaled_from(default_value, factor);
}

DWORD AttrRange::scaled_from(DWORD base, double factor) const
{
    if (!supported || max_value <= min_value) {
        return default_value;
    }
    const double value = static_cast<double>(base) * factor + 0.5;
    if (value <= static_cast<double>(min_value)) {
        return min_value;
    }
    if (value >= static_cast<double>(max_value)) {
        return max_value;
    }
    return static_cast<DWORD>(value);
}

DWORD AttrRange::clamped(int value) const
{
    if (!supported || value < 0) {
        return default_value;
    }
    const DWORD v = static_cast<DWORD>(value);
    if (v < min_value) {
        return min_value;
    }
    if (v > max_value) {
        return max_value;
    }
    return v;
}

void apply_volume(void* samples, std::size_t bytes, int percent, WORD bits_per_sample)
{
    if (percent >= 100 || bytes < sizeof(short)) {
        return;
    }
    if (bits_per_sample != 16) {
        // Nothing here produces anything but 16-bit PCM. If that ever changes this needs a
        // matching branch rather than quietly ignoring the volume.
        return;
    }
    if (percent < 0) {
        percent = 0;
    }

    auto* p = static_cast<short*>(samples);
    const std::size_t count = bytes / sizeof(short);
    for (std::size_t i = 0; i < count; ++i) {
        const int scaled = (static_cast<int>(p[i]) * percent) / 100;
        p[i] = static_cast<short>((std::max)(-32768, (std::min)(32767, scaled)));
    }
}

void fill_expected_format(WAVEFORMATEX& wfx)
{
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = kExpectedChannels;
    wfx.nSamplesPerSec = kExpectedSampleRate;
    wfx.wBitsPerSample = kExpectedBitsPerSample;
    wfx.nBlockAlign = static_cast<WORD>(wfx.nChannels * wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;
}

}  // namespace ivx
