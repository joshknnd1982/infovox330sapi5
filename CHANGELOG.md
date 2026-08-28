# Changelog

## 1.0.1

Adds a configuration utility, and with it voices you define yourself. Also corrects two
things the previous release's documentation claimed about the engine that turned out not to
be true — see [What the engine actually does](#what-the-engine-actually-does) below, because
it is the reason this release is shaped the way it is.

### Infovox 330 Configuration

A new program, `Infovox330Config.exe`, in the Start menu and — if the box on the installer's
Select Additional Tasks page is ticked — on the desktop.

It needs no administrator rights. It writes one file,
`%LOCALAPPDATA%\Infovox330 SAPI5\config.ini`, and the engine notices a change within a
second, in applications that are already running: there is no need to restart a screen
reader to hear an adjustment.

It is built 32-bit and drives the engine in process, so **Speak the preview** speaks with
whatever is on screen, saved or not. These settings are chosen by ear, so previewing an
unsaved change is the whole point.

### Voices you define

A voice you define is a SAPI 5 voice in its own right. It appears in Windows speech settings
and in every speech application beside the sixteen built-in ones, under a name of your own,
and it speaks through whichever built-in voice you pick. One built-in voice can carry any
number of your voices.

| Setting | |
|---|---|
| Speaking rate | In the engine's own words per minute. It becomes the voice's neutral point, so an application's rate slider keeps its full range either side of it — a voice set to 220 wpm is not a voice stuck near the top of the slider. |
| Pitch | The same, in the engine's own pitch units. |
| Volume | 0 to 200 percent, applied to the audio. |
| Reach of the rate and pitch sliders | How far ±10 in an application actually goes. The default is ⅓× to 3× speed and an octave of pitch; narrow it to keep a voice inside a band. |
| Pronunciations | Whole-word substitutions applied before the text reaches the engine, ignoring case. This is the only pronunciation control that exists — see `<pron>` below. |
| Language, gender and age | What the voice reports to applications, which is what they choose by. |
| Engine tags | Sent verbatim before every utterance: the escape hatch for anything the engine understands that this project does not model. |

The built-in sixteen can be adjusted the same way, but not renamed or removed — they belong
to the engine. To get a voice under a name of your own, make one. **Offer only the voices I
defined** hides the built-in sixteen from the voice list without affecting the voices yours
speak through.

### What the engine actually does

`tools/ivx_probe.cpp` is new. It speaks a fixed sentence, speaks it again with one thing
changed, and compares the audio. Two things make a naive version of that lie, and both are
handled: the engine's tagged prosody persists from one utterance to the next, so a table of
trials run back to back measures the previous tag rather than the current one; and how much
silence the engine puts in front of an utterance varies enough between runs to throw a
sample-by-sample comparison out of alignment entirely.

What it found:

| | |
|---|---|
| `\Spd\`, `SpeedSet` | Works. 45–499 words per minute around 150, for the American English voice. |
| `\Pit\`, `PitchSet` | Works. 30–250 around 101. |
| `\Rst\` | Works, and resets the attributes as well as the tags. |
| `\mrk\` | Works, and is acoustically free — which is what word and sentence events are built on. |
| `\Vol\`, `VolumeSet` | Accepted, ignored. Volume is applied to the samples instead, as before. |
| `RealTimeSet` | Accepted, ignored. |
| `\Pau\`, `\RmS\`, `\RmW\`, `\Emp\`, `\Chr\`, `\Ctx\`, `\Prt\`, `\Vce\`, `\Com\`, `\Eng\`, `\Prn\` | Parsed out of the text and ignored. Harmless — they are not spoken aloud — but they do nothing. |

**Corrections to the 1.0.0 documentation.** The README used to say `<silence msec>` maps to
`\Pau=N\` and `<spell>` to `\RmS=1\`…`\RmS=0\`. The tags are emitted, but this engine ignores
them, so **`<silence>` produces no silence and `<spell>` does not spell out**. That was true
in 1.0.0 as well; it was simply not known. Both could be done in software the way volume
already is, and neither is done yet.

**The sixteen voices are fixed.** A section added to `VoiceDescriptions.txt` by hand is never
enumerated, and changing an existing section's `Pitch`, `Dynamic`, `Aspiration`, `FormantNo`,
`SpeakerName` or even `DiphoneFile` changes nothing about what the engine produces — the shim
parses those keys, but neither the mode list nor the voice data behind it comes from that
file. This is why a voice you define is a SAPI 5 voice in front of one of the sixteen rather
than a seventeenth engine mode. Removing a section still removes the voice.

The settings the engine ignores are still offered, on the page that says they are ignored.
Hiding a control is not the same as reporting what it does.

### Accessibility

`tools/check_config_a11y.ps1` is new, and does for the configuration utility what
`check_installer_a11y.ps1` already did for the installer: it walks every control on all three
tab pages through MSAA — not UI Automation, which reports most Win32 controls as a generic
pane — and fails if any focusable control would be announced as nothing but its type. The
result is 88 controls, every focusable one named.

It found two defects, both of which looked fine on screen:

* **The real-time value had no accessible name.** A Win32 control takes its name from the
  static text before it in the tab order, and the thing before that one was a check box,
  which keeps its caption for itself. It went unreported on the first run because the check
  excused disabled controls; it no longer does, since a control that is unnamed while greyed
  out is unnamed when it is switched on.
* **The tab order ran backwards through the tab control.** Pages parented to the dialog have
  to sit above the tab control in the z-order to be painted at all, and the dialog manager
  builds the tab order from the z-order — so every control on a page came *before* the tab
  control that selects the page. Parenting the pages to the tab control puts them where a
  keyboard user expects them.

The installer's new Select Additional Tasks page passes the existing walk, and its task list
now carries an explicit accessible name the way the components tree already did.

### Installer

* The configuration utility ships as its own component, so it can be left out.
* A Start menu shortcut, and an optional desktop shortcut on the Select Additional Tasks
  page — offered rather than assumed, and unchecked by default.
* The final page offers to open the utility. It does so with `runasoriginaluser`: setup is
  elevated, and without that flag the utility would write its settings into the
  administrator's profile rather than into the profile of the person who will use the voices.
* The utility holds the engine open while it runs, so it is stopped before files are
  replaced or removed, as the helper process already was.

### Under the hood

* `src/ivx_config.*` — the settings file, shared by both architectures and every tool.
* A voice's settings are re-read when the file changes, checked at most once a second, so
  the speaking path pays almost nothing for it.
* `AttrRange::scaled_from` and `AttrRange::clamped`, so a voice's own rate can be the neutral
  point that SAPI's ±10 moves around, rather than the engine's.
* The pipe protocol between the 64-bit engine and the 32-bit helper gained a field and its
  version went from 1 to 2. The helper lingers for a minute after its last client, so an
  update can meet a helper from the previous version still running; the version check already
  rejected that, and now it moves when the wire format does.
* `INFOVOX330_CONFIG` overrides the settings file path.

### Verified

* Word and sentence events still describe the original text when a substitution has changed
  what the engine is given.
* `ivx_sapitest --regress`, the gate that keeps word marks from moving the audio: 15 strings,
  no failures.
* `--stress`: 300 rounds across two engine objects on x86, 200 rounds through the helper on
  x64, no failures.
* A voice defined at 70 words per minute speaks for 8.82 s where the same voice at the
  engine's own rate speaks for 4.05 s, and produces byte-identical audio through the 64-bit
  helper as it does in process.

## 1.0.0

First release. Sixteen Infovox 330 voices in twelve languages, exposed to every SAPI 5
application on Windows, 32-bit and 64-bit, without the SAPI 4 runtime and without the engine
ever touching the registry.
