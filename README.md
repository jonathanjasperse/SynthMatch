# SynthMatch

A synth plugin that listens to a recording of a synth and builds a preset that
sounds like it. Drop a file in, and it finds the notes, searches for the
matching sound with the help of a small trained network, and adds an EQ match
for whatever the synth can't reach. Then shape it in the node editor, or just
type what you want: "make it brighter", "add more reverb".

See [docs/ROADMAP.md](docs/ROADMAP.md) for the plan and what is done.

## Using it

- **Add a recording:** press **+** at the top, or drop a WAV, AIFF, FLAC, OGG or
  MP3 file anywhere on the window. Matching starts by itself and takes about
  6 seconds on all CPU cores but one. **Stop** keeps the closest sound found so
  far; **Match again** searches from a different starting point.
- **Compare:** **Reference** plays the recording; **Synth** plays the same notes
  through the matched preset. The spectrum plot overlays the two.
- **Edit:** opens the node view. Every part of the synth is a node, wired in
  signal order. Click a node to edit it in the strip below, drag nodes to
  arrange them (double-click empty space to tidy them), and click a node's light
  to switch it on or off. **Back** returns to the main view.
- **Assistant** (the terminal along the bottom): describe a change in words. It
  lists exactly what it changed, and **undo** takes it back (Ctrl+Z works too).
  Type **help** for examples and **/help** for commands. **/json** shows the
  preset as it is saved; you can also paste JSON in that format to set values.
- **Presets** are readable JSON files. Use the folder and arrow buttons at the
  top to open and save them.

What works best: one synth, single notes or a simple line, as dry as you have
it. Chords, drums or a full mix confuse the note detection; the plugin says so
when the pitch isn't clear. A track isolator for mixes is planned (Stage 7).

## What you need to do

**Nothing, to get started.** The trained encoder is built into the plugin, and
the assistant works without a language model (built-in rules).

**For the language model in the assistant (optional, recommended):**

1. Install Ollama from https://ollama.com. You already have it on the
   development machine.
2. Pull a small model. `qwen3:1.7b` (1.4 GB) is the default and is what was
   tested:

   ```bash
   ollama pull qwen3:1.7b
   ```

3. That's it: the plugin finds it on `127.0.0.1:11434` automatically. If the
   default isn't installed, it uses the smallest model that is (up to 5 GB).

The built-in rules answer requests made entirely of words they know ("brighter",
"more reverb", "warm analog pad", "cutoff 2 kHz") instantly. Anything else ("make
it sound like a cheap 80s organ", "it should feel underwater") goes to the model,
which takes about 2 seconds (about 12 the first time, while it loads). Commands:

- `/model NAME` picks a model.
- `/server URL` points at another server. Anything with an OpenAI-style API
  works, such as LM Studio or llama.cpp's server.
- `/llm always` sends every request that isn't an exact setting to the model.
- `/llm off` uses the rules only.

Nothing is sent anywhere except that server address.

**Training the encoder (optional, to improve it):** the network that gives the
matcher its starting guess is trained on sounds rendered by the synth itself, on
the CPU. No GPU and no recordings needed. The included model was trained on
100,000 notes for 30 passes (about 9 minutes on the dev laptop):

```bash
build/tools/train/Release/synthmatch_train --samples 100000 --epochs 30 --out models/encoder.smm
```

Then rebuild, so the plugin embeds the new file. More notes (300,000 or more)
and `--hidden 768` should help, at the cost of time. Check the effect with:

```bash
build/tools/train/Release/synthmatch_train --benchmark 20 --model models/encoder.smm
```

This matches 20 sounds the model has never seen; run it without `--model` to
compare. You **must** retrain when the synth's sound or the matcher's search
space changes. The plugin refuses a model that no longer fits, and says why.

Current numbers, on 16 unseen random sounds at a budget of 10,000 tries:

| | Without the encoder | With the encoder |
|---|---|---|
| Starting distance | 15.2 dB | 12.6 dB |
| Final distance | 6.1 dB | 6.1 dB |

So the encoder gives the search a head start but doesn't yet change where it
ends up; a larger model trained on more data is the next step there.

## Layout

```
engine/   the synth: oscillators, filter, envelopes, LFOs, unison, EQ, delay,
          reverb, match EQ; presets (JSON); WAV files. Plain C++20, no JUCE.
match/    matching: note detection, spectrograms, the CMA-ES search, the EQ
          match, the encoder network and its trainer. Plain C++20.
assist/   the assistant: built-in rules, the edit format, and the prompt and
          answer handling for a language model. Plain C++20.
plugin/   JUCE plugin (VST3, Standalone, AU on macOS): the interface, and
          Session, which runs matching and the assistant in the background.
tools/    command-line tools (below)
models/   the trained encoder (embedded in the plugin at build time)
tests/    engine, matcher and assistant tests, golden renders (CTest)
docs/     roadmap
```

## Build

Requirements: CMake 3.22+, Visual Studio 2022 or 2026 with "Desktop development
with C++" (Windows) or Xcode 15+ (macOS), and Git. CMake downloads JUCE 9.0.5 on
the first configure.

```bash
cmake -S . -B build
```

```bash
cmake --build build --config Release --parallel
```

```bash
ctest --test-dir build -C Release --output-on-failure
```

Add `-DSYNTHMATCH_BUILD_PLUGIN=OFF` to build only the libraries, tools and tests,
without JUCE. Add `-DSYNTHMATCH_INSTALL_PLUGIN=ON` to have every build copy the
plugin into your per-user plugin folder (no admin rights needed): on Windows
`%LOCALAPPDATA%\Programs\Common\VST3`, on macOS `~/Library/Audio/Plug-Ins`. If
your DAW doesn't scan that Windows folder, add it to the DAW's VST3 paths once.

Built plugins end up in `build/plugin/SynthMatch_artefacts/Release/` (`VST3/`,
`Standalone/`, and `AU/` on macOS).

## Command-line tools

On Windows they are in `build/tools/<name>/Release/`; on macOS and Linux in
`build/tools/<name>/`.

Match a recording and write the preset (and a render of it):

```bash
build/tools/match/Release/synthmatch_match reference.wav --out matched.json --render matched.wav
```

Edit a preset in words:

```bash
build/tools/edit/Release/synthmatch_edit matched.json "darker and add some delay"
```

Render a preset or parameters to a WAV:

```bash
build/tools/render/Release/synthmatch_render --preset matched.json --note 57 --out a3.wav
```

```bash
build/tools/render/Release/synthmatch_render --list-params
```

## Preset files

Grouped by node, with names for choices. Anything missing keeps its default, so
a file can be as short as the settings you care about:

```json
{
  "format": "synthmatch-preset",
  "version": 1,
  "name": "Warm pad",
  "nodes": {
    "osc1":   { "wave": "saw", "level": 1, "coarse": 0, "fine": 0, "pw": 0.5 },
    "filter": { "type": "lowpass", "cutoff_hz": 1800, "resonance": 0.2 },
    "reverb": { "on": "on", "mix": 0.3, "decay_s": 3.5 },
    "matcheq": { "amount": 1 }
  }
}
```

A saved file also holds `matcheq.gains_db`: the 31 band gains of the EQ match.

## Golden renders

`tests/golden/` holds reference renders. The tests fail if the engine's output
drifts from them. After an intentional change to the sound, regenerate them and
commit the new files:

```bash
build/tests/Release/synthmatch_engine_tests --update-golden
```

## License note

JUCE is dual-licensed (AGPLv3 or a commercial license). Check the current JUCE
license terms before distributing a closed-source build.
