# Infovox 330 SAPI 5

Sixteen Infovox 330 voices, in twelve languages, exposed to every SAPI 5 application on
Windows — 32-bit and 64-bit — without the SAPI 4 runtime and without the engine ever
touching the registry.

| | |
|---|---|
| Voices | 16 |
| Languages | 12 (`da nl en-US en-GB fi fr de is it no es sv`) |
| Audio | 16 kHz, 16-bit, mono |
| Architectures | x86 (engine in-process) and x64 (engine in a helper process) |

## What it is

Infovox 330 is a SAPI **4** engine, 32-bit only, and there will never be a 64-bit build of
it. This project wraps it as a SAPI **5** engine so that anything using Windows speech can
use these voices.

Three things make that awkward, and each has an answer here:

**The engine expects to be installed and registered.** It is not. `infovox_host.dll` — a
prebuilt shim from the NVDA add-on this project grew out of — hooks the engine's imports of
the `advapi32` registry functions and answers them from a synthetic hive built by parsing
`Voices Ivx330\VoiceDescriptions.txt`. The engine reads its configuration back, concludes it
is properly installed, and enumerates its voices. Nothing is read from the real registry.

**SAPI 4 is a different API.** Nothing here depends on Microsoft's SAPI 4 runtime being
present. The handful of interfaces the engine needs — `ITTSEnumW`, `ITTSCentralW`,
`ITTSAttributesW`, `IAudio`, `IAudioDest` and their sinks — are declared locally in
[`src/ivx_sapi4.hpp`](src/ivx_sapi4.hpp) and the engine's class factory is called directly.

**A 64-bit process cannot load a 32-bit engine.** `ISpTTSEngine` and `ISpTTSEngineSite` are
in-process interfaces with no registered proxy/stub, so the engine cannot be a
`LocalServer32` and let COM bridge the gap — the site would have to marshal *back* into
SAPI. The 64-bit DLL is therefore a genuine in-process COM object that runs
`Infovox330Server.exe` (32-bit) and pulls PCM back over a pipe. The helper never opens an
audio device; it produces bytes, and the 64-bit DLL writes them into SAPI inside the host
application's own process.

```
32-bit app ──► SAPI ──► Infovox330SAPI5.dll ──► infovox_host.dll ──► Ivx330nt.dll
64-bit app ──► SAPI ──► x64\Infovox330SAPI5.dll ──┐
                                                  │ named pipe
                          Infovox330Server.exe ◄──┘ ──► infovox_host.dll ──► Ivx330nt.dll
```

## Building

Needs CMake 3.20+, Visual Studio 2022 (Build Tools are enough), the Windows SDK, and
Inno Setup 6 for the installer. No `vcvarsall` call is required — CMake drives MSVC through
the Visual Studio generator.

```bash
powershell -ExecutionPolicy Bypass -File tools\build_all.ps1
```

That configures and builds both architectures, stages the install layout in `output\`, and
compiles `dist\Infovox330SAPI5_Setup_<version>.exe`. Add `-SkipInstaller` while iterating,
or `-Clean` to start from nothing.

`tools\stage.ps1` links the engine and the ~250 MB of voice data into `output\` by directory
junction rather than copying, so restaging is instant. Pass `-Copy` for real files.

## Installing

Run `dist\Infovox330SAPI5_Setup_<version>.exe`. It needs administrator rights, because SAPI
reads its voice list only from `HKLM`.

Exactly three things go into the registry: two `CLSID` entries for the COM classes, and one
key under `Software\Microsoft\Speech\Voices\TokenEnums` pointing SAPI at our voice
enumerator. The voices themselves are never written to the registry — SAPI asks the
enumerator, and the enumerator reads `VoiceDescriptions.txt`. Adding or removing a voice is
a matter of editing that file.

The components page lets any language be left out. A voice whose data files are absent is
skipped at enumeration, so a partial install stays consistent.

## Checking it works

```bash
ivx_speak.exe --list                        # every SAPI 5 voice Windows can see
ivx_speak.exe --voice Lucy "Hello there."   # speak out loud through the registered stack
```

`ivx_speak` goes the whole way round — SAPI enumerates, loads the engine, plays the audio —
so it is the check that registration actually worked. The installer offers to run it on the
final page.

Two more tools, installed with the "Diagnostic tools" component:

```bash
ivx_render.exe --combined                          # a WAV per voice, straight from the engine
ivx_sapitest.exe Infovox330SAPI5.dll --all         # every voice, no registration needed
ivx_sapitest.exe Infovox330SAPI5.dll --stress 500  # interrupt, switch and prosody rounds
```

`ivx_sapitest` loads the DLL and asks it for its class objects, standing in for SAPI with
its own `ISpTTSEngineSite`. It needs no administrator rights and writes nothing to the
registry, which makes it the right tool for testing a build before installing it. On x64 it
exercises the entire pipe round trip to the helper.

## Logs

Every binary logs to `%LOCALAPPDATA%\Infovox330 SAPI5\Logs`, one file per component per
process, flushed on every line so a crash cannot lose the lines that explain it.

| Variable | Effect |
|---|---|
| `INFOVOX330_LOG_LEVEL` | `off`, `error`, `warn`, `info`, `debug` (default), `trace` |
| `INFOVOX330_LOG_DIR` | write logs somewhere else |
| `INFOVOX330_DATA_DIR` | override where the engine and voice data are found |

`trace` adds a line per audio buffer and per word mark; it is large but it is the level that
answers "why did that utterance sound wrong".

The installer writes its own log and keeps a copy as `install.log` beside the program.

## What SAPI 5 features map onto, and what does not

| SAPI 5 | Here |
|---|---|
| Rate, `<rate>` | The engine's speed attribute. SAPI's −10…+10 is logarithmic and so is the engine's 45…499 wpm range around a default of 150, so ±10 lands almost exactly on ⅓× and 3×. |
| Pitch, `<pitch>` | The engine's pitch attribute, ±1 octave over the same −10…+10. |
| Volume, `<volume>` | **Applied in software.** The engine reports a volume attribute, accepts writes to it, and then produces byte-identical audio at every setting — measured across the whole range. Left to the engine, a volume slider would do nothing. |
| `<silence msec>` | `\Pau=N\` |
| `<spell>` | `\RmS=1\` … `\RmS=0\` |
| Bookmarks | `\mrk=N\`, reported back with an exact audio offset |
| `SPEI_WORD_BOUNDARY` | A `\mrk\` inserted before each **whitespace-delimited run**, never inside one. The event still reports the trimmed word, so a highlight lands on `world` rather than `world,`. See the note below on why marks must not go inside a run. |
| `SPEI_SENTENCE_BOUNDARY` | Same mechanism, one mark per fragment |
| `<lang langid>` | Switches to a voice that speaks that language for as long as the tag lasts, preferring one of the same gender |
| Abort / stop | Polled between 4 KB writes, and on x64 signalled to the helper through a named event rather than the pipe, so a stop is immediate even with audio queued |
| `<emph>` | **Not supported by this engine.** `\Emp\` produces byte-identical audio, so nothing is emitted rather than pretending. |
| `<pron>` phonemes | **No mapping exists** between SAPI phoneme ids and this engine's phoneme set. The text is spoken as written. |
| Skip | The engine has no skip; `CompleteSkip(0)` is reported and the utterance ends. |
| Unrecognised XML tags | Passed to the engine verbatim when the tag body is engine tagged text, so an application can reach an Infovox control SAPI has no concept of. Anything else is ignored. |
| Viseme / phoneme events | Declined through `GetEventInterest`. |

SAPI 4 sets rate, pitch and volume once per utterance, so consecutive fragments that agree
about prosody are merged into one engine call and a change starts a new one. That is as
fine-grained as the engine can be driven.

The engine has one further attribute, SAPI 4's `RealTime`. It reads back `0x7FFFFFFF` and
SAPI 5 has no concept that maps onto it, so it is not exposed; the value is logged at
startup so it is on the record. Anything else the engine understands is reachable through
the unrecognised-tag route above.

### Word marks must never land inside a token

Word-boundary events are implemented by writing `\mrk=N\` into the text the engine is given,
which is only sound if those marks are acoustically free. Splitting on alphanumeric runs
looked reasonable and was not: it put a mark between `145.` and `2`, and between the pieces
of `1.0.0`. That destroys the engine's own number handling — the line

```
dist\Infovox330SAPI5_Setup_1.0.0.exe, 145.2 MB.
```

took **77 seconds** instead of 12, heard as the speech rate collapsing part-way through a
sentence and recovering on the next line. It was 71 seconds of speech, not silence: the
engine was reading the broken-up numbers as something vastly longer.

Marks therefore go only at the start of whitespace-delimited runs. The trade-off is coarser
word events on things like paths — `C:\Program Files\x64` reports two words, not five —
which is the right way round: a slightly coarse highlight is a nuisance, unintelligible
speech is not.

`ivx_sapitest --regress` guards this. It speaks fifteen awkward strings — versions,
decimals, currency, times, dates, paths, URLs, contractions — twice, once asking for word
events and once not, and fails if the marks move the audio by more than 5%. The engine's own
run-to-run variation is around 0.3%, so the gate has plenty of margin and the original bug
would have shown up as 13×.

## Robustness

A screen reader interrupts speech on almost every keystroke, so the interrupt path gets more
use than the speak path. `ivx_sapitest --stress` exercises it: each round picks a random
text, rate and volume, and either runs to completion, stops part-way, or stops on the very
first buffer — the case most likely to leave the engine wedged. Voices are rebound
periodically, and `--concurrent K` holds several engine objects open at once, the way two
applications using speech would.

Measured on this machine:

| Run | Result |
|---|---|
| x86, 1000 rounds, 2 engines | 7.1 s, 89 MB of audio, slowest round 16 ms, no failures |
| x86, 300 rounds, 1 engine | 0.7 s, slowest round 31 ms, no failures |
| x64, 300 rounds through the helper | 0.9 s, slowest round 93 ms, no failures |
| x64, 200 rounds, 3 engines, helper killed part-way | no failures — every client reconnected |

`--kill-helper` terminates the 32-bit helper mid-run. That found a real bug: each 64-bit
client lost one utterance before reconnecting. Since a dropped announcement is exactly the
failure that matters here, the client now retries once when the connection breaks before any
audio has been delivered, and when it breaks mid-stream it reports what it managed to
deliver rather than failing the whole utterance. Both are logged.

## Layout

```
src/
  ivx_sapi4.hpp      SAPI 4 interfaces, declared locally
  ivx_engine.*       the engine driver: COM thread, message pump, audio sink   (x86 only)
  ivx_synth.hpp      the types both architectures share, and the backend interface
  ivx_pipe*.cpp      the wire protocol, the 64-bit client and the 32-bit helper
  ivx_voices.*       VoiceDescriptions.txt, and the voice catalogue it produces
  ivx_tts_engine.*   ISpTTSEngine and ISpObjectWithToken
  ivx_enum_tokens.*  the voice enumerator SAPI reaches through TokenEnums
  ivx_token.*        one SAPI 5 voice token per catalogue entry
  ivx_main.cpp       DllMain, DllGetClassObject, DllRegisterServer
  ivx_log.*          file logging
  ivx_paths.*        finding the engine and the voice data without the registry
tools/
  build_all.ps1            build both architectures, stage, compile the installer
  stage.ps1                assemble output\
  ivx_render.cpp           a WAV per voice, straight from the engine
  ivx_sapitest.cpp         drive the DLL without registering it
  ivx_speak.cpp            speak through the registered SAPI 5 stack
  check_installer_a11y.ps1 read every wizard control through MSAA
installer/
  infovox330_sapi5.iss     the Inno Setup script
  before_install.txt       what the installer shows before installing
```

## Accessibility

The installer is meant to be usable by the people most likely to want these voices, so its
accessibility is measured rather than assumed. `tools\check_installer_a11y.ps1` launches a
probe build — the same wizard, without elevation or payload — walks every page through
MSAA, and reports any focusable control that would be announced as nothing but its type.
It uses oleacc rather than UI Automation deliberately: PowerShell's UIA client reports
almost every Win32 control as a generic pane, which makes it useless for this. oleacc
reports what NVDA and JAWS actually see.

That check found one real defect — the components tree, on the page where languages are
chosen, had no accessible name — which is why `InitializeWizard` sets one. The current
result is every focusable control on every page named.

## Provenance and licensing

The engine (`Ivx330nt.dll`, `Sx32w.dll`, `cryput.dll`), the voice data, and
`infovox_host.dll` are not part of this source tree and are not covered by it. The engine
and voices are commercial software belonging to their publisher; this project only wraps
them. Install it only where you are entitled to use Infovox 330.

`infovox_host.dll` is a prebuilt 32-bit binary carried over from the NVDA add-on. Its C
source is not available anywhere; the interface it exposes is three functions wide and
documented in [`src/ivx_engine.cpp`](src/ivx_engine.cpp), so reimplementing it is tractable
if that ever matters.
