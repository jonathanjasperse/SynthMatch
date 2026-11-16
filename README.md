# SynthMatch

SynthMatch listens to a recording of a synth and builds a preset that sounds like
it. Drop a sound in and it finds the notes, searches for the closest patch, and
EQ matches whatever's left. Then tweak it in the node editor, or just tell it what
you want, like "make it brighter" or "add more reverb".

It's free and open source, and runs as a VST3 or a standalone app on Windows and
macOS (plus Audio Unit on Mac).

## Install

Grab the installer for your system from the
[Releases page](https://github.com/jonathanjasperse/SynthMatch/releases) and run
it: `SynthMatch-<version>-Windows.exe` or `SynthMatch-<version>-macOS.pkg`. Rescan
plugins in your DAW and it shows up as an instrument.

The installer can also set up the AI assistant. That part downloads Ollama and a
small language model (about 2.5 GB) from their official sites, and it all runs on
your own computer. You can skip it. The assistant still gets plain requests like
"brighter" or "cutoff 2 kHz" without it, and you can type `/download` later to
get the model.

The installers aren't code-signed yet, so you'll get a warning the first time. On
Windows click More info, then Run anyway. On Mac go to System Settings > Privacy &
Security and click Open Anyway.

## Using it

Hit the + at the top, or drop a WAV, AIFF, FLAC, OGG or MP3 anywhere on the
window. Matching starts right away and takes about ten seconds. Reference plays
the recording, Synth plays the same notes through the match, and the spectrum
shows the two on top of each other.

Not close enough? Hit Match again. It searches from new starting points and only
swaps in the new sound if it's closer. Replace, next to the file name, loads a
different recording.

Edit opens the node view, where every part of the synth is a node in signal
order. Click a node to edit it, click empty space to get the main controls back,
and click a node's light to turn it on or off. Back takes you home.

The terminal along the bottom is the assistant. Type what you want in normal
words, like "darker and a bit wider", "more reverb" or "it should feel
underwater". It shows exactly what it changed, and `undo` (or Ctrl+Z, Cmd+Z on
Mac) takes it back. Type `help` for examples and `/help` for commands.

Presets are plain JSON files. Open and save them with the buttons at the top.

## Getting good matches

Use one synth playing single notes or a simple line, as dry as you've got it.
Chords, drums and full mixes throw off the note detection, and the plugin will
tell you when it can't hear a clear pitch.

## Building it yourself

See [CONTRIBUTING.md](CONTRIBUTING.md) for building from source, the
command-line tools, and how to help out.

## License

SynthMatch is free software under the GNU Affero General Public License v3 or
later ([LICENSE.md](LICENSE.md)), with no warranty. It's built with JUCE and a few
other open-source libraries, listed in
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
