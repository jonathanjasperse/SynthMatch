# Roadmap

SynthMatch is a synth plugin that analyzes a reference recording of a synth and
builds a preset that sounds like it. Each stage below is small, ends in
something testable, and builds on the one before.

## Architecture rule

The engine (`engine/`) is plain C++ with no JUCE or GUI code. The same engine
runs in three places:

- the plugin (`plugin/`), which users load in a DAW
- the render CLI (`tools/render/`), which renders a preset and notes to a `.wav`
- Python bindings (Stage 5), used for matching, dataset generation and training

The parameter schema (`engine/src/Params.cpp`) defines every parameter once.
Never rename or reorder parameter ids, because presets and trained models refer
to them.

## Stages

### Stage 0: Scaffold
CMake project, JUCE 9 plugin (VST3 + Standalone, plus AU on macOS), a sine-wave
engine with an attack/release envelope, the render CLI, tests, and CI on
Windows, macOS and Linux (engine only on Linux).

Done when:
- [x] the engine, CLI and tests build and pass on Windows
- [ ] the plugin loads and plays in a DAW on Windows
- [x] CI passes on Windows, macOS and Ubuntu (2026-11-09, commit 72d750d)

### Stage 1: Engine core (current)
3 oscillators (saw, square, triangle, sine, noise, with detune and unison),
mixer, filter (state-variable / ZDF), amp and filter envelopes (ADSR), 2 LFOs,
and polyphony. Oscillators must be band-limited (e.g. PolyBLEP).
Done when: golden-render tests pass (render a fixed preset and compare it with a
saved reference).

Built in slices:
- [x] **1a** Oscillators and amp envelope: 3 band-limited oscillators (sine,
  triangle, saw, square with pulse width), coarse and fine tuning, mixer levels,
  white noise, ADSR amp envelope, golden-render tests.
- [x] **1b** Filter: state-variable (ZDF) low-pass, band-pass and high-pass with
  cutoff, resonance and key tracking, plus an ADSR filter envelope. The filter
  is always in the signal path; the default is a low-pass fully open at 20 kHz.
- [ ] **1c** Modulation and width: 2 LFOs with assignable targets, unison with
  detune and stereo spread, pitch bend.
- [ ] **1d** Polish: parameter smoothing (no zipper noise), click-free voice
  stealing, performance check with all 16 voices.

### Stage 2: Synth view and presets
A real editor with knobs bound to the schema. Presets saved and loaded as JSON.
The render CLI accepts a preset file.

### Stage 3: Node mode
Mode switch between the Synth view and the Node view. A graph editor with nodes,
ports and wires. Delay, reverb, EQ and mixer nodes. The graph is compiled on the
message thread and swapped atomically into the audio thread.

### Stage 4: Match EQ
Load a reference file, compare long-term average spectra, and apply a smoothed
correction curve with Amount, Smoothing and Range controls. Pure DSP. Separate
from the adjustable EQ node.

### Stage 5: DSP matching
Python bindings for the engine. Prototype analysis in Python (pitch, envelope,
harmonics, noise, reverb tails), then a CMA-ES search that renders through the
engine and minimizes a multi-resolution STFT + log-mel + envelope loss.
Done when: a random preset can be found again from its own render.

### Stage 6: ML encoder
Generate a large dataset of (render, parameters) pairs, train a small CNN that
maps spectrograms to parameters, export to ONNX, and use it in the plugin as the
starting point for the search.

Hardware: nothing in this project needs CUDA.
- Dataset generation runs the C++ engine on CPU cores. It's the bulk of the work
  and needs no GPU.
- Training: PyTorch's Intel XPU build runs on Intel Arc GPUs, including the
  integrated Arc 140V
  (`pip install torch --index-url https://download.pytorch.org/whl/xpu`). Plain
  CPU also works for a small model. Use rented GPUs (Colab, Kaggle, RunPod) only
  for large sweeps.
- Inference in the plugin uses ONNX Runtime on CPU, because end users won't have
  CUDA either. The OpenVINO execution provider can optionally use Intel GPUs and
  NPUs.

### Stage 7: Track isolator
Source separation (Demucs-class model through ONNX Runtime), run offline on a
background thread, plus manual cleanup: region select, band-pass, mid/side solo.

### Stage 8: Installers
One command builds release installers into `dist/` (which is not committed):

- **Windows:** `dist/SynthMatch-<version>-Windows.exe`, built with Inno Setup.
  Installs the VST3 to `C:\Program Files\Common Files\VST3`, the Standalone
  app, factory presets and the ONNX models, with an uninstaller.
- **macOS:** `dist/SynthMatch-<version>-macOS.pkg`, built with `pkgbuild` and
  `productbuild`. Installs the VST3 and AU as universal binaries (Apple Silicon
  and Intel).
- A `package` CMake target (or `scripts/package`) runs the Release build, then
  the installer step. CI builds both installers and uploads them on a version
  tag, since the macOS one can only be built on a Mac.
- Before public release: code signing on Windows, and Developer ID signing plus
  notarization on macOS (needs a paid Apple Developer account). Unsigned
  installers work for testing but show security warnings.

A basic unsigned installer can be added earlier, any time after Stage 2, so
testers can install builds without copying files by hand.
