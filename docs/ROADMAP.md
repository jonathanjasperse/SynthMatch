# Roadmap

SynthMatch is a synth plugin that analyzes a reference recording of a synth and
builds a preset that sounds like it. Each stage below ends in something
testable and builds on the one before.

## Architecture rule

The synth (`engine/`), the matcher (`match/`) and the assistant (`assist/`) are
plain C++ with no JUCE or GUI code. The same code runs in:

- the plugin (`plugin/`), which users load in a DAW
- the command-line tools (`tools/`): render, match, edit, train

The parameter schema (`engine/src/Params.cpp`) defines every parameter once.
Never rename or reorder parameter ids, because presets and trained models refer
to them.

## Stages

### Stage 0: Scaffold (done)
CMake project, JUCE 9 plugin (VST3 + Standalone, plus AU on macOS), the render
CLI, tests, and CI on Windows, macOS and Linux (no plugin on Linux).

- [x] the engine, CLI and tests build and pass on Windows
- [ ] the plugin loads and plays in a DAW on Windows (needs a person with a DAW)
- [x] CI passes on Windows, macOS and Ubuntu

### Stage 1: Engine core (done, except 1d)
- [x] **1a** 3 band-limited oscillators (sine, triangle, saw, square with pulse
  width), tuning, mixer, white noise, ADSR amp envelope, golden-render tests.
- [x] **1b** State-variable (ZDF) low-pass, band-pass and high-pass filter with
  resonance, key tracking and its own ADSR envelope.
- [x] **1c** 2 LFOs (sine, triangle, saw, square, random) on pitch, cutoff, amp
  or pulse width; unison up to 7 voices with detune and stereo spread; pitch
  bend; tempo from the host.
- [ ] **1d** Polish: parameter smoothing (automating a knob can zipper), click-free
  voice stealing. Performance is fine: the worst case (16 voices, all 3
  oscillators with 7-voice unison, every effect on) uses about 9% of one CPU
  core on the dev laptop.

### Stage 2: Main view and presets (done)
- [x] Presets as readable JSON grouped by node; partial files work; the render
  tool reads and writes them.
- [x] Main view: drop or choose a recording, match progress, score, waveform,
  spectrum comparison, reference and synth playback.
- [x] The look: dark gunmetal tiled with glossy metal pentagons, a metal
  waveform as the mark.

### Stage 3: Node mode (done for a fixed signal path)
- [x] Edit button to a node view and Back to the main view.
- [x] Nodes for the 3 oscillators, noise, mixer, filter, amp, 2 LFOs, EQ, delay,
  reverb, match EQ and output, wired in signal order, with an inspector strip
  that shows the key controls when no node is selected.
- [x] Delay with many settings (time or tempo sync, feedback, ping-pong, spread,
  low and high cut in the feedback, modulation), reverb (feedback delay
  network), 4-band EQ.
- [ ] Re-routable wiring (reorder effects, parallel paths, extra mixers). The
  engine's path is fixed today; this needs a graph compiled on the message
  thread and swapped into the audio thread.

### Stage 4: Match EQ (done)
31 third-octave bands fitted to the difference between the reference and the
synth's long-term spectra, kept only where it improves the match, with an
Amount control. Separate from the adjustable EQ node.

### Stage 5: DSP matching (done)
Note detection (YIN pitch tracking, onsets, legato and repeated notes), a log-mel
spectrogram distance with an envelope term, and a CMA-ES search over 43
dimensions that renders candidates through the real engine on all cores but one.
Done in C++ rather than Python, so the plugin runs the same code.

- [x] a sound made by the synth is found again from its own render (tested)
- [x] search from the best three distinct starting points, then a final polish
- [x] note detection that isn't fooled by vibrato, stacked oscillators an octave
  or a fifth apart, unison beating or reverb tails
- [ ] better results on sounds the synth can't make exactly, and on chords
- [ ] the octave of a sound with a strong sub-oscillator is ambiguous; the
  matcher picks the lower one

### Stage 6: ML encoder (done, first version)
A neural network (multi-layer perceptron) that maps a summary of a note
(spectrum over time, harmonics, wobble, noisiness) to a starting preset. It is
trained in C++ on notes rendered on the fly, on the CPU, and embedded in the
plugin. No GPU, no Python, no ML runtime.

- [x] trainer, model file, loader that refuses a mismatched model
- [x] trained model: choices right 69% of the time, continuous settings off by
  15% of their range (a blind guess is off by about 25%)
- [x] learning from other synths and your ears: render references from VST3
  synths you own (`synthmatch_host`), batch-match them, grade the matches in the
  browser (`tools/grade`), and train on the approved ones (`--graded`)
- [ ] use the grades to tune the matcher's distance so its favourite agrees
  with yours
- [ ] make it improve the final match, not just the start: more data, a bigger
  model, or a spectrogram CNN

### Stage 7: Track isolator (not started)
Source separation (a Demucs-class model through ONNX Runtime), run offline on a
background thread, plus manual cleanup: region select, band-pass, mid/side solo.

### Stage 8: Installers (scripts done, first release pending)
One command builds release installers into `dist/` (which is not committed):

- **Windows:** `dist/SynthMatch-<version>-Windows.exe`, built with Inno Setup.
  Installs the VST3 to `C:\Program Files\Common Files\VST3`, the Standalone
  app and factory presets, with an uninstaller.
- **macOS:** `dist/SynthMatch-<version>-macOS.pkg`, built with `pkgbuild` and
  `productbuild`. Installs the VST3 and AU as universal binaries (Apple Silicon
  and Intel).
- A `package` CMake target (or `scripts/package`) runs the Release build, then
  the installer step. CI builds both installers and uploads them on a version
  tag, since the macOS one can only be built on a Mac.
- Before public release: code signing on Windows, and Developer ID signing plus
  notarization on macOS (needs a paid Apple Developer account). Unsigned
  installers work for testing but show security warnings.

- [x] Inno Setup script, macOS pkgbuild/productbuild script, one-step scripts
- [x] optional "AI assistant" part: installs Ollama from ollama.com and starts
  the model download; `/download` and `/start` in the plugin as the fallback
- [x] Release workflow: builds both on a version tag, drafts a GitHub release;
  signs and notarizes on macOS when the secrets are set
- [ ] first real run of the installers (needs Inno Setup locally, or the
  workflow), and a test install on a clean machine
- [ ] code signing, and a public place to download from
### Stage 9: Assistant terminal (done)
- [x] A terminal along the bottom of the window: type a change, see exactly what
  changed, undo it.
- [x] Built-in rules: a vocabulary of sound words (brighter, warmer, wider,
  punchier, plucky, pad, vibrato, ...), named settings with units ("cutoff
  2 kHz"), switches, waveforms and commands. Instant and exact.
- [x] A small local language model (Ollama, or any OpenAI-style server) for
  requests the rules don't fully understand. It reads the current settings and
  answers with edits as JSON constrained by a schema; answers are checked and
  limited before they are applied. Default `qwen3:1.7b`, or the smallest model
  installed.
- [x] The preset JSON can be shown in the terminal (`/json`) and pasted back in.

## Hardware notes

Nothing in this project needs CUDA. Rendering, matching and training run on CPU
cores. The language model runs in Ollama, which uses the CPU on this laptop
(about 2 seconds per request for a 1.7B model once it is loaded).
