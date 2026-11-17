# Contributing to SynthMatch

Thanks for helping. SynthMatch is a synth plugin that listens to a recording of
a synth and builds a matching preset, with a node editor and an assistant you
can talk to. Contributions of all sizes are welcome: bug reports, matching
results on real recordings, presets, docs, DSP, ML and interface work.

## Building

You need CMake 3.22 or newer, Git, and Visual Studio 2022 or 2026 with "Desktop
development with C++" on Windows, or Xcode 15+ on macOS. CMake downloads JUCE
9.0.5 the first time.

```bash
cmake -S . -B build
```

```bash
cmake --build build --config Release --parallel
```

```bash
ctest --test-dir build -C Release --output-on-failure
```

Add `-DSYNTHMATCH_BUILD_PLUGIN=OFF` to build only the libraries, tools and tests
(fast, no JUCE). Add `-DSYNTHMATCH_INSTALL_PLUGIN=ON` to copy the plugin into
your per-user plugin folder after every build: `%LOCALAPPDATA%\Programs\Common\VST3`
on Windows (add it to your DAW's VST3 paths once), `~/Library/Audio/Plug-Ins` on
macOS. Built plugins land in `build/plugin/SynthMatch_artefacts/Release/`.

In Visual Studio, open `build/SynthMatch.slnx` (`.sln` with VS 2022), or use File
> Open > Folder, which picks up `CMakePresets.json` (x64 Debug, x64 Release and a
"Windows installer" preset). VS Code with the CMake Tools extension reads the same
presets.

## How the code is laid out

```
engine/     the synth: oscillators, filter, envelopes, LFOs, unison, EQ, delay,
            reverb, match EQ; presets (JSON); WAV files. Plain C++20, no JUCE.
match/      matching: note detection, spectrograms, the CMA-ES search, the EQ
            match, the encoder network and its trainer. Plain C++20.
assist/     the assistant: built-in rules, the edit format, and the language
            model's prompt and answer handling. Plain C++20.
plugin/     the JUCE plugin (VST3, Standalone, AU on macOS): the interface, and
            Session, which runs matching and the assistant in the background.
tools/      command-line tools (below)
models/     the trained encoder, built into the plugin
packaging/  installer scripts (Inno Setup on Windows, pkgbuild on macOS)
scripts/    one-step build, test and package scripts
tests/      engine, matcher and assistant tests, and golden renders
```

## Command-line tools

On Windows they're in `build/tools/<name>/Release/`, elsewhere in
`build/tools/<name>/`. Match a recording, write the preset and a render of it:

```bash
build/tools/match/Release/synthmatch_match reference.wav --out matched.json --render matched.wav
```

Edit a preset in words, or render one:

```bash
build/tools/edit/Release/synthmatch_edit matched.json "darker and add some delay"
```

```bash
build/tools/render/Release/synthmatch_render --preset matched.json --note 57 --out a3.wav
```

`synthmatch_render --list-params` prints every parameter. To teach the matcher
with other synths and your own ears: `synthmatch_host` renders notes from a VST3
synth you own, `synthmatch_batch` matches each a few times, `python
tools/grade/serve.py BATCH` lets you grade the matches in your browser, and
`synthmatch_train --graded BATCH` trains on the ones you rated close.

## Preset files

Grouped by node, with names for choices. Anything missing keeps its default, so a
file can be as short as the settings you care about:

```json
{
  "format": "synthmatch-preset",
  "version": 1,
  "name": "Warm pad",
  "nodes": {
    "osc1":   { "wave": "saw", "level": 1, "coarse": 0, "fine": 0, "pw": 0.5 },
    "filter": { "type": "lowpass", "cutoff_hz": 1800, "resonance": 0.2 },
    "reverb": { "on": "on", "mix": 0.3, "decay_s": 3.5 }
  }
}
```

A saved file also holds `matcheq.gains_db`, the 31 band gains of the EQ match.

## The trained encoder

The network that gives the matcher its first guess is trained on sounds the synth
renders itself, on the CPU:

```bash
build/tools/train/Release/synthmatch_train --samples 100000 --epochs 30 --out models/encoder.smm
```

Most of the time goes into rendering the notes, not training on them. Add
`--save-data notes.smd` to keep them, and `--load-data notes.smd` trains another
size or more epochs on the same notes without rendering them again.

Measure a model on sounds it has never seen (leave out `--model` to compare
without one):

```bash
build/tools/train/Release/synthmatch_train --benchmark 20 --budget 8000 --model models/encoder.smm
```

Retrain whenever the synth's sound or the search space changes, then rebuild so
the plugin embeds the new file.

## Golden renders and the background tile

`tests/golden/` holds reference renders, and the tests fail if the engine's output
drifts from them. After an intentional change to the sound, regenerate them with
`build/tests/Release/synthmatch_engine_tests --update-golden`, check the change is
what you expect, and commit them with an explanation.

The tiled background can be any PNG, made in Blender with a ready-made scene. See
[plugin/assets/README.md](plugin/assets/README.md).

## Installers

`scripts/package-windows.ps1` (needs Inno Setup) and `scripts/package-macos.sh`
build them into `dist/`. Pushing a `v*` tag builds both on GitHub and attaches
them to a draft release.

## Reporting a problem

Open an issue saying what you did, what you expected and what happened. Include
your system (Windows or macOS version), your DAW and its version, and the
SynthMatch version from the About box (the "i" button). For a disappointing
match, attach the recording if you're happy to share it and have the right to; a
preset file (`.json`) helps with sound problems.

## Making a change

Fork the repository, make a branch, and make the change with a test that shows it
works: `tests/engine_tests.cpp` for the synth, `tests/match_tests.cpp` for the
matcher and network, `tests/assist_tests.cpp` for the assistant. Build, run all
the tests, and open a pull request saying what changed and why. CI builds and
tests it on Windows, macOS and Linux.

The project's rules, the ones reviewers will check most closely:

- **The engine, matcher and assistant stay free of JUCE**, so they run in the
  tools and tests.
- **The audio thread never allocates, locks or waits.**
- **Output is deterministic:** the same preset and notes give the same samples.
- **Parameter ids never change.** New parameters go at the end of the table.
- **A sound change comes with regenerated golden renders**, and an explanation
  of why the sound changed.
- **Changing the search space or the encoder's features means retraining**
  `models/encoder.smm` with `synthmatch_train` (the loader rejects a stale one).
- **The assistant only talks to the model server in its settings**, which is
  this computer by default. Nothing is sent to a remote service by default.

## Licence

SynthMatch is free software under the GNU Affero General Public License, version
3 or later ([LICENSE.md](LICENSE.md)). By contributing, you agree that your
contribution is licensed under the same terms.

Only contribute code you wrote or have the right to contribute. A third-party
library must have a licence compatible with the AGPLv3 (for example MIT, BSD,
Apache 2.0, zlib, or GPL version 3), and must be added to
`THIRD_PARTY_NOTICES.txt`. For libraries vendored with JUCE, regenerate that file
with `python scripts/make-notices.py`.

## Training data and recordings

The shipped model learns only from sounds the synth renders itself. If you
contribute recordings or grades for training, say where the sounds came from and
confirm you have the right to share them. Don't contribute renders of commercial
sample libraries or presets you aren't allowed to redistribute.
