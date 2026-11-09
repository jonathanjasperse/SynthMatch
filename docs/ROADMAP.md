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

### Stage 0: Scaffold (current)
CMake project, JUCE 9 plugin (VST3 + Standalone, plus AU on macOS), a sine-wave
engine with an attack/release envelope, the render CLI, tests, and CI on
Windows, macOS and Linux (engine only on Linux).

Done when:
- [x] the engine, CLI and tests build and pass on Windows
- [ ] the plugin loads and plays in a DAW on Windows
- [ ] CI passes on macOS (needs the repo on GitHub)

### Stage 1: Engine core
3 oscillators (saw, square, triangle, sine, noise, with detune and unison),
mixer, filter (state-variable / ZDF), amp and filter envelopes (ADSR), 2 LFOs,
and polyphony. Oscillators must be band-limited (e.g. PolyBLEP).
Done when: golden-render tests pass (render a fixed preset and compare it with a
saved reference).

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
