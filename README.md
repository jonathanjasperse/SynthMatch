# SynthMatch

A synth plugin that listens to a reference recording of a synth and builds a
preset that sounds like it. See [docs/ROADMAP.md](docs/ROADMAP.md) for the plan.

**Status:** Stage 0. The plugin is a sine-wave synth with gain, attack and
release, which proves out the build, the engine/plugin split and the offline
renderer.

## Layout

```
engine/        synth engine: plain C++20, no JUCE (parameters, voices, offline render, WAV)
plugin/        JUCE plugin wrapper (VST3, Standalone, AU on macOS)
tools/render/  synthmatch_render: render notes to a .wav without a DAW
tests/         engine tests and CLI smoke tests (CTest)
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

Built plugins end up in `build/plugin/SynthMatch_artefacts/Release/`:

| Format | Path | Install to |
|---|---|---|
| VST3 | `VST3/SynthMatch.vst3` | Windows: `C:\Program Files\Common Files\VST3`; macOS: `~/Library/Audio/Plug-Ins/VST3` |
| AU (macOS) | `AU/SynthMatch.component` | `~/Library/Audio/Plug-Ins/Components` |
| Standalone | `Standalone/` | Run directly |

## Render from the command line

```bash
build/tools/render/Release/synthmatch_render --note 60 --note 64 --note 67 --set amp_release_ms=400 --out chord.wav
```

```bash
build/tools/render/Release/synthmatch_render --list-params
```

(On macOS and Linux the binary is at `build/tools/render/synthmatch_render`.)

## License note

JUCE is dual-licensed (AGPLv3 or a commercial license). Check the current JUCE
license terms before distributing a closed-source build.
