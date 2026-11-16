# Launch plan: SynthMatch 1.0, free

How to get from today (a working 0.2 that has only run on one PC) to a free 1.0
that strangers can download, install and trust. Weeks are counted from the day
you start; adjust to your pace.

This covers licensing as it applies to the code and files in this repository.
It is practical guidance, not legal advice. Where money or obligations depend
on it, read the actual licence text (linked below) or ask a lawyer.

---

## The four decisions to make first

| # | Decision | Recommendation | Why |
|---|---|---|---|
| 1 | Open source, or free but closed? | **Decided: open source, AGPLv3 or later** | No JUCE licence needed, free code signing for open-source projects, public GitHub releases, and contributors can help. See "Licensing" below. |
| 2 | The name | Keep **SynthMatch** if it's clear | Search trademark registers (USPTO, EUIPO) and KVR/plugin stores for "SynthMatch" before you print it anywhere. Register the domain and social handles. |
| 3 | Where people download it | **GitHub Releases** on a public repo, linked from a simple site | Free, reliable, versioned. A private repo's releases are private. |
| 4 | Pay for signing? | **Yes for macOS** ($99/year), free route for Windows | Unsigned macOS installers are blocked until people dig into System Settings; most won't. Windows warns but lets people continue. |

---

## Licensing

### What's in the plugin, and what each licence asks of you

Checked against the files in this build (JUCE 9.0.5 and its bill of materials, `JUCE.spdx.json`):

| Component | Licence | What it asks of a free release |
|---|---|---|
| **JUCE 9** (the framework) | AGPLv3, **or** the commercial JUCE 9 licence | Either release SynthMatch's source under the AGPLv3, or accept the JUCE 9 licence terms (juce.com/legal/juce-9-licence; check its plans, fees and any revenue limits). |
| **VST3 SDK 3.8** (Steinberg) | MIT | Include its copyright notice. Using the "VST" name or logo has rules in `VST3_Usage_Guidelines.pdf` (inside JUCE's VST3_SDK folder). |
| **Audio Unit SDK** (Apple) | Apache 2.0 | Include its notice. |
| **Libraries JUCE compiles in**: zlib, libpng, JPEG, WebP, HarfBuzz, SheenBidi, LunaSVG, PlutoVG, FLAC, Ogg, Vorbis, Opus | Zlib, libpng, IJG, BSD-3, MIT, Apache 2.0 | All allow free distribution; most require their copyright notices to ship with the plugin. |
| **SynthMatch's own code** | Yours to choose (see below) | |
| **The trained encoder** (`models/encoder.smm`) | Yours: trained only on sounds the synth itself rendered | No third-party data in it, so no obligations. License it with the project. If you retrain it on sounds rendered from other companies' synths (the grading loop in the guide), check those synths' licence agreements before shipping that model. |
| **Ollama** | MIT | Not bundled: the installers download it from ollama.com, so you don't redistribute it. Credit it. |
| **Qwen3 1.7B** (the language model) | Apache 2.0 | Not bundled: downloaded through Ollama. Credit it. |
| Fonts | System fonts (Segoe UI, Helvetica Neue) | Not bundled, nothing to do. |

### Option A (recommended): open source under the AGPLv3

- The repository becomes public and gets a `LICENSE` file with the AGPLv3 text.
  Every source file can carry a short header saying so.
- Anyone may use, change and share SynthMatch, but if they distribute a changed
  version (or run it as a network service), they must share their source too.
  That protects your work from being taken closed.
- You don't need a JUCE licence, and there's no revenue limit, so donations
  (Ko-fi, GitHub Sponsors) are fine.
- Open-source projects can get free Windows code signing (SignPath Foundation),
  and GitHub Releases and Pages are free and public.
- If you might ever want to sell a closed version later, keep that door open:
  either stay the only author, or ask contributors to agree that you may
  relicense their contributions (a simple contributor agreement).

### Option B: free, closed source

- Keep the repository private and accept the JUCE 9 licence (read its current
  plans and limits at juce.com before deciding).
- Write a short end-user licence agreement (EULA) for SynthMatch: free to use,
  no reverse engineering, no warranty.
- You still must ship the third-party notices above.
- Windows code signing costs money (no open-source discount).

### Files to add before release

- [x] `LICENSE.md`: the AGPLv3 text.
- [x] `THIRD_PARTY_NOTICES.txt`: every library's own licence text, generated
  from JUCE's bill of materials by `scripts/make-notices.py`, and installed by
  both installers.
- [x] An About box (the "i" button): version, copyright, the no-warranty notice,
  credits, and buttons for the source code and the licence.
- [x] `CONTRIBUTING.md`: how to contribute, and that contributions are under the
  same licence.
- [ ] Make the repository public (Settings, or `gh repo edit`), after a last
  look through it. Consider the name: `ml-synthmatch` could become `synthmatch`
  before people start linking to it (the About box and README use the URL).
- [ ] Optional: a short licence header (`SPDX-License-Identifier:
  AGPL-3.0-or-later`) at the top of each source file.
- [ ] A short privacy statement for the website: SynthMatch collects nothing.
  The plugin only connects to a model server on your own computer; the
  installers download Ollama and the model from their official sites. (If you
  later add an email list or update checks, update this.)

### Things to say carefully

- **Trademarks:** VST is Steinberg's, Audio Units is Apple's, Ollama and Qwen
  belong to their owners. Name them plainly to say what works with what; don't
  use their logos unless their guidelines allow it.
- **Matching other people's sounds:** describe it as "match the sound of a
  recording", and make demos with your own recordings. Avoid marketing like
  "copy [artist]'s sound" or "steal presets from [product]".

---

## The plan

### Phase 0: Decide (week 1)

- [ ] Make the four decisions above.
- [ ] Define 1.0's scope: what's in, what waits for 1.1 (suggested below).
- [ ] Set a budget (see "Costs").

### Phase 1: Make it release-quality (weeks 1 to 4)

The plugin works; these make it dependable in other people's studios.

**Must have for 1.0**

- [ ] **Parameter smoothing** (roadmap 1d): automating or turning knobs quickly
  must not crackle.
- [ ] **Click-free voice stealing** when more than 16 notes are held.
- [ ] **pluginval in CI**: Tracktion's free plugin validator, run on both
  platforms at a high strictness level. It catches most crashes hosts would hit.
- [ ] **auval** (Apple's Audio Unit validator) in the macOS CI job.
- [ ] **DAW test pass** on real hosts. At least: Reaper, Ableton Live, FL Studio
  and Bitwig on Windows; Logic, GarageBand and Ableton on macOS. For each: load,
  play, automate, save the project, reopen it (the preset and match must come
  back), bounce offline, close.
- [ ] **Edge cases**: sample rates 44.1 to 192 kHz, tiny and huge buffer sizes,
  several instances at once, no Ollama installed, Ollama stopped mid-request,
  very long or silent or stereo-weird reference files.
- [ ] **Factory presets**: 20 to 40 good starting sounds (bass, lead, pad,
  pluck, keys), in a folder the open button finds.
- [ ] **About box and notices** (see Licensing).
- [ ] **First-run help**: a one-screen "how it works" and what recordings work
  best (single notes, dry, one instrument).

**Good to have**

- [ ] A preset browser (list and click) instead of only open/save.
- [ ] Interface scaling (75% to 150%) for small and very large screens.
- [ ] Retrained encoder on more data (see `docs/GUIDE.md`, M3 to M5) if it
  measurably improves the benchmark.

**Leave for 1.1 or later**: the track isolator, re-routable node wiring, chord
matching.

### Phase 2: Installers, signing, clean-machine tests (weeks 3 to 5)

- [ ] Run the Release workflow by hand once (Actions tab) and download both
  installers.
- [ ] **Windows clean install**: use Windows Sandbox (built into Windows 11 Pro)
  for a fresh Windows every time. Install with and without the AI assistant
  part, open the plugin, match a file, uninstall, check nothing is left behind.
- [ ] **macOS clean install**: needs a Mac (a friend's, a cheap used Mac mini,
  or a rented cloud Mac). Same checks, on Apple Silicon at least.
- [ ] **Signing**:
  - macOS: join the Apple Developer Program, create "Developer ID Application"
    and "Developer ID Installer" certificates, and add the secrets listed in the
    README. The workflow then signs and notarizes automatically.
  - Windows: if open source, apply to SignPath Foundation; otherwise Azure
    Trusted Signing or a code-signing certificate.
- [ ] **Antivirus check**: an installer that downloads and runs another
  installer (Ollama) can look suspicious to antivirus heuristics, especially
  unsigned. Signing helps most. Only check public release builds with online
  scanners such as VirusTotal, since uploads there are shared. If it's flagged,
  make the assistant part open ollama.com instead of downloading for the person.
- [ ] Ship `LICENSE` and `THIRD_PARTY_NOTICES.txt` inside both installers.

### Phase 3: A home on the web (weeks 4 to 6)

- [ ] **Landing page** (GitHub Pages is free): one sentence on what it does, a
  30 to 60 second video (drop a recording, the match, a few words typed into the
  assistant), screenshots, download buttons for Windows and macOS, system
  requirements, FAQ, licence, privacy statement.
- [ ] **Domain** (optional, about $12 a year).
- [ ] **KVR Audio**: a free developer account and a product page; it's where
  many people look for plugins.
- [ ] **Support**: GitHub Issues (if public) plus a Discord server for questions
  and the beta.

### Phase 4: Beta (weeks 5 to 8)

- [ ] Recruit 20 to 50 producers (your network, Discord communities, KVR, Reddit
  threads that allow it).
- [ ] Give them a short list of what to try and a feedback form.
- [ ] **Ask for recordings, with permission**: real-world files where matching
  went well or badly are the best test set there is, and, with explicit consent
  to use them for training, the best training data too (see the guide's
  augmentation and real-audio ideas). Keep a record of who agreed to what.
- [ ] Fix the top problems; release 1.0 release candidates until a week passes
  with no serious bug.

### Phase 5: Launch (weeks 8 to 9)

- [ ] Bump the version to 1.0.0 in `CMakeLists.txt`, tag `v1.0.0`, publish the
  release the workflow drafts.
- [ ] Update the landing page and KVR.
- [ ] Announce:
  - KVR news, and Bedroom Producers Blog (it covers free plugins);
  - Reddit, following each subreddit's self-promotion rules: r/synthesizers,
    r/edmproduction (its free-stuff threads), r/WeAreTheMusicMakers;
  - a YouTube demo of your own, and a polite note to creators who review free
    plugins;
  - short before/after clips (reference, then the match) for Instagram,
    TikTok or YouTube Shorts.
- [ ] Be around for the first 48 hours to answer questions and fix anything
  urgent with a 1.0.1.

### Phase 6: After launch

- [ ] A patch release every couple of weeks while reports come in, then a
  steadier rhythm.
- [ ] Track the matching benchmark over time; grow it with beta recordings.
- [ ] Roadmap for 1.1+: the track isolator, re-routable nodes, chord matching, a
  better encoder.
- [ ] Optional: donations (Ko-fi or GitHub Sponsors). Fine under the AGPLv3; under
  the JUCE licence, check how its terms treat revenue.

---

## Costs

| Item | Cost | Needed? |
|---|---|---|
| JUCE | $0 under the AGPLv3; check juce.com for the JUCE 9 licence | Depends on decision 1 |
| Apple Developer Program | $99 a year | Strongly recommended |
| Windows code signing | $0 (SignPath Foundation, open source) to roughly $10 a month (Azure Trusted Signing) | Recommended |
| Domain | about $12 a year | Optional |
| Hosting, CI, website | $0 (GitHub; public repos get free CI minutes, including macOS) | |
| A Mac to test on | $0 (borrowed) to a few hundred dollars (used Mac mini), or a rented cloud Mac | One test pass at least |

Prices change; check them when you buy.

---

## Risks

| Risk | What to do |
|---|---|
| A host crashes with the plugin | pluginval and auval in CI, the DAW test pass, the beta |
| Matching disappoints on real recordings | Say clearly what works best; collect beta recordings; keep improving the benchmark |
| The assistant download is big (about 2.5 GB) or fails | It's optional, the plugin works without it, and `/download` retries |
| Antivirus flags the installer | Sign it; if needed, link to ollama.com instead of downloading |
| macOS users can't open it | Notarize (Apple Developer Program) |
| Licence slip (missing notices, JUCE terms) | The Licensing checklist above, before every release |

---

## What can be done in this repo next

These are ready to pick up:

1. `LICENSE`, `THIRD_PARTY_NOTICES.txt`, and an About box.
2. Parameter smoothing and click-free voice stealing (also exercise E4 in the guide).
3. pluginval and auval in CI.
4. A factory preset folder and a simple preset browser.
5. A landing page on GitHub Pages.
