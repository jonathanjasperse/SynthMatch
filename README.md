# SynthMatch

A synth plugin that listens to a reference recording of a synth and builds a
preset that sounds like it. See [docs/ROADMAP.md](docs/ROADMAP.md) for the plan.

**Status:** Stage 1b. The plugin is a 16-voice synth with 3 band-limited
oscillators (sine, triangle, saw, square with pulse width), tuning, a mixer,
white noise, a resonant filter (low-pass, band-pass, high-pass) with key
tracking and its own envelope, and an ADSR amp envelope. LFOs and unison come
next.

## Layout

```
engine/        synth engine: plain C++20, no JUCE (parameters, voices, offline render, WAV)
plugin/        JUCE plugin wrapper (VST3, Standalone, AU on macOS)
tools/render/  synthmatch_render: render notes to a .wav without a DAW
tests/         engine tests, golden renders and CLI smoke tests (CTest)
docs/          roadmap
```

## Requirements

- CMake 3.22 or newer
- Windows: Visual Studio 2022 or 2026 with "Desktop development with C++"
- macOS: Xcode 15 or newer
- Git (CMake downloads JUCE 9.0.5 on the first configure)

## Build

```bash
cmake -S . -B build
```

```bash
cmake --build build --config Release --parallel
```

```bash
ctest --test-dir build -C Release --output-on-failure
```

The first configure takes a few minutes while JUCE downloads and builds its
helper tool. To build only the engine, CLI and tests without JUCE, add
`-DSYNTHMATCH_BUILD_PLUGIN=OFF`.

To have every build copy the plugin into your per-user plugin folder (no admin
rights needed), configure with `-DSYNTHMATCH_INSTALL_PLUGIN=ON`. On Windows the
VST3 goes to `%LOCALAPPDATA%\Programs\Common\VST3`; on macOS it goes to
`~/Library/Audio/Plug-Ins`. If your DAW doesn't scan that Windows folder by
default, add it to the DAW's VST3 paths once.

Built plugins end up in `build/plugin/SynthMatch_artefacts/Release/`:

| Format | Path | Install to |
|---|---|---|
| VST3 | `VST3/SynthMatch.vst3` | Windows: `C:\Program Files\Common Files\VST3`; macOS: `~/Library/Audio/Plug-Ins/VST3` |
| AU (macOS) | `AU/SynthMatch.component` | `~/Library/Audio/Plug-Ins/Components` |
| Standalone | `Standalone/` | Run directly |

## Golden renders

`tests/golden/` holds reference renders. The tests fail if the engine's output
drifts from them. After an intentional change to the sound, regenerate them and
commit the new files:

```bash
build/tests/Release/synthmatch_engine_tests --update-golden
```

## Render from the command line

```bash
build/tools/render/Release/synthmatch_render --note 60 --note 64 --note 67 --set osc1_wave=square --set osc2_level=0.6 --set osc2_fine=8 --out chord.wav
```

```bash
build/tools/render/Release/synthmatch_render --note 36 --set filter_cutoff_hz=120 --set filter_resonance=0.6 --set filter_env_amount=6 --set filter_decay_ms=900 --set filter_sustain=0.15 --out sweep.wav
```

```bash
build/tools/render/Release/synthmatch_render --list-params
```

(On macOS and Linux the binary is at `build/tools/render/synthmatch_render`.)

## License note

JUCE is dual-licensed (AGPLv3 or a commercial license). Check the current JUCE
license terms before distributing a closed-source build.
