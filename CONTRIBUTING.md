# Contributing to SynthMatch

Thanks for helping. SynthMatch is a synth plugin that listens to a recording of
a synth and builds a matching preset, with a node editor and an assistant you
can talk to. Contributions of all sizes are welcome: bug reports, matching
results on real recordings, presets, docs, DSP, ML and interface work.

## Getting started

[docs/GUIDE.md](docs/GUIDE.md) walks you from building the project to your first
engine and ML changes, with exercises. [docs/ROADMAP.md](docs/ROADMAP.md) lists
what's done and what's next.

## Reporting a problem

Open an issue with:

- what you did, what you expected, and what happened;
- your system (Windows or macOS version), your DAW and its version, and the
  SynthMatch version (shown in the About box, the "i" button);
- for a disappointing match: the recording, if you're happy to share it and
  have the right to. A preset file (`.json`) helps for sound problems.

## Making a change

1. Fork the repository and create a branch.
2. Make the change, with a test that shows it works (see "How the tests work"
   in the guide).
3. Build and run all the tests:

   ```bash
   cmake --build build --config Release --parallel
   ```

   ```bash
   ctest --test-dir build -C Release --output-on-failure
   ```

4. Open a pull request saying what changed and why. CI builds and tests it on
   Windows, macOS and Linux.

The project's rules are in [CLAUDE.md](CLAUDE.md). The ones reviewers will check
most closely:

- **The engine, matcher and assistant stay free of JUCE**, so they run in the
  tools and tests.
- **The audio thread never allocates, locks or waits.**
- **Output is deterministic:** the same preset and notes give the same samples.
- **Parameter ids never change.** New parameters go at the end of the table.
- **A sound change comes with regenerated golden renders**, and an explanation
  of why the sound changed.

## Licence

SynthMatch is free software under the GNU Affero General Public License, version
3 or later ([LICENSE.md](LICENSE.md)). By contributing, you agree that your
contribution is licensed under the same terms.

Only contribute code you wrote or have the right to contribute. If you add a
third-party library, its licence must be compatible with the AGPLv3 (for
example MIT, BSD, Apache 2.0, zlib, or GPL version 3), and it must be added to
`THIRD_PARTY_NOTICES.txt`. For libraries vendored with JUCE, regenerate that file
with `python scripts/make-notices.py`.

## Training data and recordings

The trained model shipped with SynthMatch learns only from sounds the synth
renders itself. If you contribute recordings or grades for training, say where
the sounds came from and confirm you have the right to share them. Don't
contribute renders of commercial sample libraries or presets you aren't
allowed to redistribute.
