// Driving the Infovox 330 engine: 32-bit only.
//
// Everything the engine touches lives on one dedicated thread that owns the COM objects and
// runs a message pump, because SAPI 4 requires one. Callers stay on their own thread and
// receive audio through a queue, which keeps ISpTTSEngineSite calls on SAPI's thread where
// they belong.

#pragma once

#include <windows.h>
#include <objbase.h>

#include <memory>
#include <string>
#include <vector>

#include "ivx_sapi4.hpp"
#include "ivx_synth.hpp"

namespace ivx {

struct ModeInfo {
    GUID mode_guid = GUID_NULL;
    std::wstring mode_name;
    std::wstring product_name;
    std::wstring mfg_name;
    std::wstring speaker;
    WORD language_id = 0;
    WORD gender = 0;
    WORD age = 0;
    DWORD features = 0;
};

class Engine {
public:
    // One engine per process: the Infovox DLL keeps global state and the shim patches it
    // in place, so a second instance would fight the first.
    static Engine& instance();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Loads infovox_host.dll, initialises the engine and enumerates its modes. Repeated
    // calls after a success are cheap no-ops; after a failure they retry.
    HRESULT ensure_initialised();

    [[nodiscard]] bool initialised() const;

    // Empty until ensure_initialised() succeeds.
    [[nodiscard]] std::vector<ModeInfo> modes();

    // Selects the mode if necessary and reports what it can adjust. Cached per mode.
    HRESULT get_ranges(const GUID& mode, VoiceRanges& out);

    // The format the engine asked for, once a mode has been selected. Falls back to the
    // expected format before then.
    void output_format(WAVEFORMATEX& out);

    // Synthesises one utterance, blocking until the engine is done or the sink aborts.
    HRESULT speak(const SpeakParams& params, SynthSink& sink);

    // Asks the engine to stop the utterance in progress. Safe from any thread.
    void abort_current();

    void shutdown();

private:
    Engine();
    ~Engine();

    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ivx
