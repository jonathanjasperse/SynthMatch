# Contributor guide

A step-by-step path from "I can build it" to "I'm shipping engine and ML changes".
Work through it in order: each step builds on the last, and each ends with
something you can run and see.

Commands are written for Git Bash on Windows (run them from the repository
folder). In PowerShell they work the same, with `\` instead of `/` in paths if
you prefer. On Windows the programs end in `.exe`; you can leave that off.

---

## Part 1: Get set up (an afternoon)

### Step 1. Build everything and run the tests

```bash
cmake -S . -B build -DSYNTHMATCH_INSTALL_PLUGIN=ON
```

```bash
cmake --build build --config Release --parallel
```

```bash
ctest --test-dir build -C Release --output-on-failure
```

You should see `100% tests passed out of 8`. What just happened:

- **CMake** read `CMakeLists.txt` (the root one and one per folder) and wrote a
  Visual Studio solution into `build/`. You only re-run the first command when
  you add or remove files or change CMake options.
- **The build** compiled four libraries (`engine`, `match`, `assist`, and the
  plugin's shared code), the command-line tools, the tests, and the plugin
  itself (VST3 and Standalone), and copied the VST3 into your plugin folder.
- **CTest** ran the test programs in `tests/`.

### Step 2. Use the tools

These are the fastest way to see what each part does, without a DAW.

```bash
build/tools/render/Release/synthmatch_render --note 57 --set osc1_wave=square --set filter_cutoff_hz=800 --set reverb_on=on --out a.wav
```

That rendered a note through the engine to `a.wav`. Listen to it. Now match it:

```bash
build/tools/match/Release/synthmatch_match a.wav --out a.json --render a-matched.wav
```

Open `a.json` (a preset, readable) and listen to `a-matched.wav`. Then edit it in
words:

```bash
build/tools/edit/Release/synthmatch_edit a.json "darker and a bit wider"
```

And see every parameter the synth has:

```bash
build/tools/render/Release/synthmatch_render --list-params
```

### Step 3. Run the plugin

Open `build/plugin/SynthMatch_artefacts/Release/Standalone/SynthMatch.exe`.
Drop `a.wav` on it, press **Synth** and **Reference** to compare, open **Edit**,
type in the terminal. Then load the VST3 in your DAW (Reaper: add
`%LOCALAPPDATA%\Programs\Common\VST3` to the VST paths and re-scan).

### Step 4. Open it in Visual Studio and step through code

Open `build/SynthMatch.sln`. In Solution Explorer, right-click
`synthmatch_engine_tests` and choose **Set as Startup Project**. Open
`engine/src/Engine.cpp`, click in the margin next to the first line of
`Engine::process` to set a breakpoint, switch the configuration at the top from
Release to **Debug**, and press F5. When it stops, step with F10 (over) and F11
(into), and hover over variables to see their values. This is how you'll
understand any part of the code you're unsure about.

---

## Part 2: How the code is organised (read this once, refer back often)

```
engine/   the synth (no JUCE)
match/    the matcher and the neural network (no JUCE)
assist/   the assistant's rules and language-model prompt (no JUCE)
plugin/   the JUCE plugin: audio wrapper, interface, Session
tools/    command-line programs
tests/    tests and golden renders
models/   the trained network
```

The first three are plain C++ so they can be tested quickly, run in the tools,
and never depend on the interface. The plugin uses them; they never use the
plugin.

### The life of a sound (engine)

1. **Parameters** ([engine/src/Params.cpp](../engine/src/Params.cpp)). One table
   lists every parameter: id, name, range, default, curve. Everything else (the
   plugin's knobs, presets, the matcher, the assistant) reads this table.
2. **Note on** ([engine/src/Engine.cpp](../engine/src/Engine.cpp), `noteOn`). A
   free voice (out of 16) is claimed, its envelopes start, its oscillator phases
   reset.
3. **Every sample**, for each sounding voice (`renderVoices`):
   - the amp and filter envelopes advance (`dsp/Envelope.h`);
   - the LFOs advance and say how far to move pitch, cutoff, level or pulse width
     (`dsp/Lfo.h`);
   - each oscillator's unison copies produce a sample (`dsp/Oscillator.h`; the
     PolyBLEP trick there removes aliasing), panned left and right;
   - the mix goes through the filter (`dsp/Filter.h`), then is multiplied by the
     amp envelope, velocity and gain.
4. **After the voices** (`process`): EQ (`dsp/Eq.h`), delay (`dsp/Delay.h`),
   reverb (`dsp/Reverb.h`), match EQ (`dsp/MatchEq.h`).
5. **Offline rendering** ([engine/src/Render.cpp](../engine/src/Render.cpp))
   drives the same engine from a list of notes; the tools, tests and matcher all
   use it.

### The life of a match (match)

Read [docs/ROADMAP.md](ROADMAP.md) Stage 5 and 6, then follow `matchSound` in
[match/src/Matcher.cpp](../match/src/Matcher.cpp) from the top:

1. `detectNotes` ([match/src/Pitch.cpp](../match/src/Pitch.cpp)) finds the notes.
2. The `Objective` class renders a candidate preset on those notes and scores it
   with `spectralDistance` ([match/src/Features.cpp](../match/src/Features.cpp)).
3. Starting points: measured guesses plus the encoder's guess
   ([match/src/Encoder.cpp](../match/src/Encoder.cpp)).
4. The `Search` class runs CMA-ES ([match/src/Optimizer.cpp](../match/src/Optimizer.cpp)),
   choice sweeps and a polish over the settings listed in
   [match/src/SearchSpace.cpp](../match/src/SearchSpace.cpp).
5. The EQ match is fitted and the preset returned.

### The plugin

- [plugin/PluginProcessor.cpp](../plugin/PluginProcessor.cpp): the audio thread.
  Turns host parameters and MIDI into engine calls. Never allocates or waits.
- [plugin/Session.cpp](../plugin/Session.cpp): everything that isn't audio, on
  the message thread: loading files, running the match and the language model
  on background threads, undo, presets, the terminal.
- [plugin/ui/](../plugin/ui): the interface. `Theme` (colours, knobs),
  `Backdrop` (the pentagons), `MainView`, `NodeView`, `Terminal`.

### The rules, and why

These are in [CLAUDE.md](../CLAUDE.md). The ones that bite:

- **Real-time safety.** The audio thread must finish each block in a few
  milliseconds, every time. Allocating memory or waiting on a lock can take
  longer, and the result is clicks and dropouts. So `process()` and anything it
  calls must not allocate, lock, or touch files.
- **Determinism.** Same preset and notes, same samples, always. The matcher
  compares renders, the tests compare against saved renders, and the network is
  trained on renders; randomness would break all three. Noise uses a seeded
  generator for this reason.
- **Parameter ids never change.** Presets and the trained network refer to
  parameters by id and position. Add new ones at the end.

---

## Part 3: The workflow for any change

1. **Branch:** `git switch -c my-change`
2. **Change the code.**
3. **Add or update a test** for what you changed (see below).
4. **Build and test:** the two commands from Step 1.
5. **If the sound changed on purpose**, regenerate the golden renders, listen
   or look at why they changed, and commit them:

   ```bash
   build/tests/Release/synthmatch_engine_tests --update-golden
   ```

   Never regenerate them just to make a failing test pass: a failing golden test
   means the sound changed, and you need to know why.
6. **Commit** with a message that says what changed and why.
7. **Push** and check CI (GitHub Actions) passes on Windows, macOS and Linux.
   macOS minutes are expensive on a private repository, so push in batches.

### How the tests work

The tests are plain programs: [tests/engine_tests.cpp](../tests/engine_tests.cpp),
[tests/match_tests.cpp](../tests/match_tests.cpp) and
[tests/assist_tests.cpp](../tests/assist_tests.cpp). Each `testSomething()`
function checks facts with `CHECK(condition)` and `CHECK_NEAR(value, expected,
tolerance)`, and `main` calls them all. To add a test, write a function and add
a call to it in `main`. Good engine tests measure something physical: a pitch, a
level, a decay time, the gain at a frequency.

---

## Part 4: Engineering exercises

Do these in order. Each one teaches a part of the system; the later ones are
real roadmap items.

### E1. Change a default and watch the safety net catch it

Change the default `filter_cutoff_hz` in `engine/src/Params.cpp` from 20000 to
5000, build, and run the engine tests. Some golden renders fail: the default
sound changed. Change it back. Now you know what golden tests are for.

### E2. Add a word to the assistant

In [assist/src/Assistant.cpp](../assist/src/Assistant.cpp), find `kVocabulary`
and add a word to an existing entry (for example "glassy" to `Treble`, or
"woolly" to `Dark`). Add a check to `testRulesVocabulary` in
`tests/assist_tests.cpp`, such as
`CHECK(v(after("make it woolly", p), param::FilterCutoffHz) < 1500.0f);`.
Build, test, then try it in the plugin's terminal.

### E3. Add a parameter: filter drive

Saturation before the filter is a classic synth control. This touches every
layer:

1. In `engine/include/synthmatch/Params.h`, add `FilterDrive,` just before
   `Count`. In `engine/src/Params.cpp`, add a row at the end of the table:
   `{"filter_drive", "Filter Drive", 0.0f, 1.0f, 0.0f, Curve::Linear, ""},`.
   (The id starts with `filter_`, so it appears in the Filter node automatically.)
2. In `Engine.h` add `double filterDrive_ = 0.0;`, set it in `updateDerived()`,
   and in `renderVoices` shape the mix before the filter, for example
   `mixL = std::tanh(mixL * (1.0 + 9.0 * drive)) / std::tanh(1.0 + 9.0 * drive);`
   (and the same for `mixR`), only when drive is above zero so the default sound
   doesn't change.
3. In `tests/engine_tests.cpp`, change `CHECK(kNumParams == 72)` to 73, and add a
   test: with drive at 1, a sine comes out with a strong third harmonic
   (use `amplitudeAt`, like `testOscillatorHarmonics`).
4. Build and test. The golden renders should still pass (drive defaults to 0).
   The plugin, presets, node view and assistant pick the new parameter up by
   themselves: check it in the Filter node.

The trained network still loads, because the matcher doesn't search drive yet.
Adding it to `SearchSpace` would mean retraining (see M4).

### E4. Parameter smoothing (roadmap 1d)

Turn the cutoff knob quickly while a note plays: you can hear zipper noise,
because the value jumps once per block. Fix it by moving the engine's derived
values (gain, cutoff, levels) towards their targets a little each sample, a
one-pole smoother: `current += (target - current) * coefficient`, with the
coefficient set for about 20 ms. Keep it out of the per-sample path when
nothing is moving, so it costs nothing at rest. Test: a block-size test like
`testBlockSizeIndependence` should still pass, and a sudden gain change should
no longer produce a jump.

### E5. Something in the interface

Add a mini picture for the Delay node in `NodeView.cpp` (`drawGlyph`): a row of
fading dots spaced by the delay time. Check it with the screenshot hook:

```bash
SYNTHMATCH_SNAPSHOT_DIR=build/snapshots build/plugin/SynthMatch_artefacts/Release/Standalone/SynthMatch
```

---

## Part 5: ML engineering exercises

### M1. Learn to read the benchmark

```bash
build/tools/train/Release/synthmatch_train --benchmark 12 --budget 8000 --model models/encoder.smm
```

It makes 12 random sounds the network has never seen, matches each, and prints:

- **found:** the notes detected (note number, how many, held length). Compare
  with **note** and **held**, which are the truth.
- **oracle:** how close the *true* preset gets, played on the notes that were
  found. This is the best the search could possibly do. If it's high, note
  detection is the problem, not the search.
- **guess:** the network's guess, with no search at all.
- **start / search / final:** the best starting point, after the search, and
  after the EQ match. Distances are in dB; lower is closer; under 3 is very
  close, over 10 is a different sound.

Set `SYNTHMATCH_BENCH_VERBOSE=1` to also print each sound's true settings. Run
it once and write down the means: that's your baseline. Every ML change you make
should be measured against it, on the same sounds (the benchmark always uses
the same seeds).

### M2. Train a small model and see over- and under-fitting

```bash
build/tools/train/Release/synthmatch_train --samples 20000 --epochs 15 --hidden 256 --out models/small.smm
```

Watch two numbers each epoch:

- **training loss:** how wrong the network is on the examples it learns from;
- **validation loss:** how wrong it is on examples it never trains on.

Both falling: it's learning. Training falling but validation flat or rising:
it's memorising (overfitting); it needs more data or a smaller network. Both
high and flat: it's too small or the inputs don't carry the information. The
table at the end shows, per parameter, how well it does; key tracking stays near
a blind guess, because it can't be heard from one note. Benchmark the small
model with `--model models/small.smm` and compare with M1.

How the training works, in [match/src/Train.cpp](../match/src/Train.cpp):

1. `randomSound` draws a random preset and note; it's rendered, and
   `encoderFeatures` turns it into 625 numbers (the network's input). The preset
   becomes the answer (`makeTargets`). Settings that can't be heard are masked.
2. Inputs are standardised (each feature shifted and scaled to mean 0, spread 1).
3. Each step takes 128 examples: `forward` computes the network's outputs,
   `outputLoss` measures the error and its slope, `accumulate` runs
   backpropagation (the chain rule, layer by layer, backwards), and `Adam`
   nudges every weight downhill.

If backpropagation is new to you, watch Karpathy's "micrograd" video (see the
reading list) first; `accumulate` will then read like a direct translation.

### M3. More data

```bash
build/tools/train/Release/synthmatch_train --samples 1000000 --epochs 10 --hidden 768 --out models/big.smm
```

About 40 minutes and 3 GB of memory. Benchmark it. If it beats the current
model, copy it over `models/encoder.smm`, rebuild (the plugin embeds it), and
commit it.

### M4. Train on detected notes (the most valuable fix)

The network trains on the true note (exact start and length), but in the plugin
it gets the *detected* note, which can be off. Train it the way it's used.

In `trainEncoder` (Train.cpp), inside the `parallelFor` that renders examples,
after `render(...)`:

1. Make the mono mix, and call `detectNotes(mono, options.sampleRate)`.
2. If it found notes: take the audio from `found.startSample` on, and compute
   features with `found.notes.front()` and, if there's a second note, its start
   as `endSec`. This mirrors what `matchSound` does before calling
   `options.encoder->predict`. If nothing was found, fall back to the true note.
3. Retrain (M3 settings), benchmark against your baseline.

Include `synthmatch/match/Pitch.h`. The features and outputs don't change, so the
model file format stays the same.

### M5. Augmentation: make training sounds look like recordings

The network has only heard this synth, perfectly clean. Before computing
features, randomly (say half the time each) add a low noise floor, a gentle tilt
EQ, a little saturation (`tanh`), or a short room reverb (the engine's own
reverb on a separate pass works). Use the example's seed so it stays
deterministic. Retrain, benchmark, and also try a few real recordings in the
plugin: augmentation's benefit shows most on real audio.

### M6. Generate on the fly

Right now every example is rendered up front and kept in memory, which caps the
dataset size. Instead, render a fresh batch on worker threads while the network
trains on the current one, so it never sees an example twice. This is a
producer/consumer pattern: a queue of ready batches, producers filling it,
the trainer taking from it.

### M7. Improve the search

Read `matchSound` and the `Search` class, then experiment, measuring each change
with the benchmark (same budget):

- the budget split between stages (`shares`, the polish budget);
- the CMA-ES population size and starting step (`spread`);
- the distance: `envelopeWeight` and `rangeDb` in `DistanceConfig`;
- using the network's top two or three choices as separate starting points.

Change one thing at a time, and look at the per-sound rows, not just the mean:
an improvement on average that ruins one kind of sound isn't one.

### M8. Learn from other synths, graded by your ears

The network has only ever heard SynthMatch. Real recordings come from other
synths, so give it some, and use your own judgement as the teacher. The loop:
render sounds from synths you own, let the matcher try each one a few times,
pick and rate the best match by ear, and train on the matches you approved.
This is learning from human feedback by filtering for reward (only the
examples you rated well are learned from), the simplest form of reinforcement
from human preferences.

**1. Render references from a synth you own.** `synthmatch_host` loads a VST3
instrument and plays notes through it. Random variations of its default sound
work with any synth; `--only` keeps the variations to the parts of the sound you
care about. On Windows the tool is at
`build/tools/host/synthmatch_host_artefacts/Release/synthmatch_host.exe`:

```bash
synthmatch_host "C:/Program Files/Common Files/VST3/Arturia/Mini V4.vst3" --list
```

```bash
synthmatch_host "C:/Program Files/Common Files/VST3/Arturia/Mini V4.vst3" --random 40 --only "Osc,Filter,VCF,VCA,LFO,Drive,Noise,Mixer" --notes 45,52 --out references/MiniV4
```

`--list` shows its presets and parameters (the ones marked "not randomised" are
skipped: MIDI proxies, volume, tuning, arpeggiators). `--programs` renders the
presets it exposes to hosts, and `--presets DIR` renders `.vstpreset` files.
Silent and unpitched results are dropped. Any folder of WAV files works too,
for example notes you bounce from your DAW.

**2. Make a batch.** Each reference is matched several times, from different
starting points:

```bash
build/tools/batch/Release/synthmatch_batch --refs references/MiniV4 --out batches/minimoog-1
```

About 2 seconds per match at the default budget (`--budget 6000`), so 40
references with 3 candidates take around 4 minutes. `--random N` makes
references from SynthMatch's own sounds instead, where the true preset is known.

**3. Grade it.**

```bash
python tools/grade/serve.py batches/minimoog-1
```

A page opens in your browser. For each reference: **Space** plays it, **A**,
**S**, **D** play the candidates, and **1** to **5** rate the candidate you
played last, which also makes it your pick (1 is a different sound, 5 is
"can't tell apart"). **N** means none are close; **arrow keys** move between
items. The matcher's own scores stay hidden until you've graded, so they don't
sway you. Grades save to `grades.json` as you go, and you can stop and come
back. The header shows how often the matcher's favourite agrees with yours, a
direct measure of how well its distance matches your ears.

**4. Train on what you approved.**

```bash
build/tools/train/Release/synthmatch_train --graded batches/minimoog-1 --samples 1000000 --epochs 10 --hidden 768 --out models/graded.smm
```

Every match you rated 4 or 5 (`--graded-min`) becomes a training example: the
real recording as the input, the preset you picked as the answer. Each counts as
much as 20 synthetic notes (`--graded-repeat`), since there are few of them.
`--graded` can be given several times. Benchmark the result as usual, and also
make a fresh batch from the same synth: if training worked, the new batch's
first candidates should already be closer, and you'll rate them higher.

**Things to watch:**

- **Variety beats volume.** Grade sounds from several synths (Jup-8, Prophet-5,
  Jun-6, MS-20, Pigments) rather than hundreds from one, or the network learns
  that synth's quirks.
- **Only approved matches teach.** Low ratings and "none" are not used as
  training answers, but they're kept, and they're useful: they show which
  sounds the synth can't reach yet.
- **The rights to what you train on.** Rendering a synth you own for your own
  training is normal use. Before you *distribute* a model trained on another
  company's synth, check that synth's licence agreement for anything about
  using its output, and keep the shipped model to sources you're sure of.

A next step from here: use the grades to tune the matcher's distance itself.
Each graded item says which of the candidates you preferred, so the weights in
`DistanceConfig` (and new terms you might add) can be fitted so that its
favourite agrees with yours more often.

### M9. Bigger models in PyTorch (advanced)

To try a convolutional network on spectrograms, add a `--dump PATH` option to
the trainer that writes the inputs and targets to a file, train in Python with
PyTorch (CPU, or the Intel XPU build on your laptop's Arc GPU), and write a
loader for its weights. Keep the C++ trainer as the baseline to beat.

---

## Part 6: Glossary

- **dB (decibel):** a log scale of level; +6 dB is about twice the amplitude,
  −60 dB is a thousandth.
- **FFT:** turns a short slice of audio into the strength of each frequency.
- **Spectrogram:** FFTs of successive slices, so frequency content over time.
  **Mel** spacing makes bands narrow in the bass and wide in the treble, like
  hearing.
- **Aliasing:** frequencies above half the sample rate fold back as harsh
  inharmonic tones. **PolyBLEP** removes most of it from saw and square waves.
- **ADSR:** attack, decay, sustain, release: the shape of a note over time.
- **SVF / ZDF:** a state-variable filter built so it stays stable and accurate
  even when its cutoff moves every sample.
- **Golden render:** a saved render that tests compare against.
- **YIN:** the pitch-tracking method: find the shift at which a signal best
  matches itself.
- **CMA-ES:** a search method that samples a cloud of candidates, moves it
  towards the better ones, and learns which directions matter.
- **MLP:** multi-layer perceptron, the simplest neural network: layers of
  weighted sums with a ReLU (`max(0, x)`) between them.
- **Sigmoid / softmax:** squash outputs to 0..1, or to probabilities over
  choices.
- **Loss:** a number for how wrong the network is. **Cross-entropy** for
  choices, **squared error** for amounts.
- **Backpropagation:** the chain rule, applied backwards through the layers, to
  find how each weight affects the loss. **Adam** uses that to update weights.
- **Epoch:** one pass over the training data.
- **Overfitting:** learning the training examples rather than the pattern;
  shows as validation loss stopping while training loss keeps falling.
- **Augmentation:** making varied copies of training examples so the network
  copes with conditions it would otherwise never see.
- **Oracle:** the score of the true answer under realistic conditions; the
  ceiling on what any search could achieve.

---

## Part 7: Reading list

**C++ and plugins**
- *A Tour of C++* by Bjarne Stroustrup: modern C++ in a short book.
- The JUCE tutorials at juce.com/learn/tutorials, and The Audio Programmer
  (YouTube and its Discord) for the plugin world.
- *Designing Software Synthesizer Plugins in C++* by Will Pirkle.

**DSP**
- *The Scientist and Engineer's Guide to Digital Signal Processing* by Steven W.
  Smith, free at dspguide.com: the gentlest start.
- Julius O. Smith's free online books (ccrma.stanford.edu/~jos): filters,
  spectral analysis, physical audio.
- *The Art of VA Filter Design* by Vadim Zavalishin (free PDF), and Andrew
  Simper's SVF papers at cytomic.com: the filter in this engine.
- Robert Bristow-Johnson's "Audio EQ Cookbook": the EQ and match EQ filters.
- Välimäki and Huovilainen, "Antialiasing Oscillators in Subtractive
  Synthesis" (IEEE Signal Processing Magazine, 2007): PolyBLEP and friends.

**Pitch, search and ML**
- de Cheveigné and Kawahara, "YIN, a fundamental frequency estimator for speech
  and music" (JASA, 2002).
- Nikolaus Hansen, "The CMA Evolution Strategy: A Tutorial" (arXiv:1604.00772).
- Andrej Karpathy, "Neural Networks: Zero to Hero" (YouTube), starting with
  micrograd: backpropagation from scratch, exactly what `Train.cpp` does.
- *Dive into Deep Learning* (free at d2l.ai).
- Yee-King, Fedden and d'Inverno, "Automatic Programming of VST Sound
  Synthesizers Using Deep Networks and Other Techniques" (IEEE TETCI, 2018):
  the closest published work to this project.
- Engel et al., "DDSP: Differentiable Digital Signal Processing" (ICLR 2020): a
  different route, where the synth itself is differentiable.
