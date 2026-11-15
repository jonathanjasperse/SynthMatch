"""Writes THIRD_PARTY_NOTICES.txt: the licences of the third-party code that ends
up inside SynthMatch's plugin and app.

    python scripts/make-notices.py

Run it after configuring the build (it reads JUCE from build/_deps/juce-src) and
again whenever JUCE is updated. It uses JUCE's own bill of materials
(JUCE.spdx.json) to find each library, and copies each library's own licence
text, so nothing is paraphrased.
"""

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
JUCE = ROOT / "build" / "_deps" / "juce-src"
OUT = ROOT / "THIRD_PARTY_NOTICES.txt"

# Parts of JUCE's tree that SynthMatch never builds: Android audio, the LV2,
# AAX and ASIO plugin formats, and the physics engine.
NOT_BUILT = {"Oboe", "AAX SDK", "ASIO SDK", "LV2", "lilv", "serd", "sord", "sratom", "Box2D", "Box2D testbed",
             "Projucer icons", "Gradle wrapper",
             "@juce-framework/webview"}  # the web view is off in this build, and is JUCE's own code

# "LICENSE", "COPYING.txt", "Flac Licence.txt", ...
LICENCE_NAMES = re.compile(r"(licen[cs]e|copying|notice)", re.IGNORECASE)

# The Chromium project's licence, which its vendored file refers to but which
# isn't vendored with it.
CHROMIUM_BSD = """Copyright 2014 The Chromium Authors

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

   * Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
   * Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following disclaimer
in the documentation and/or other materials provided with the
distribution.
   * Neither the name of Google LLC nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."""


def special_cases(name: str, folder: pathlib.Path):
    """Libraries whose licence isn't in a licence file or a comment block."""
    if name == "jpeglib":
        # The Independent JPEG Group's terms are the README's LEGAL ISSUES section.
        text = (folder / "README").read_text(encoding="utf-8", errors="replace")
        match = re.search(r"\nLEGAL ISSUES\n=+\n(.*?)\n\n[A-Z][A-Z ]+\n=+\n", text, re.DOTALL)
        return match.group(1).strip() if match else None
    if name == "Chromium cubic_bezier":
        return CHROMIUM_BSD
    if name == "pslextensions":
        header = next(iter(sorted(folder.glob("*.h"))), None)
        if header:
            lines = header.read_text(encoding="utf-8", errors="replace").splitlines()
            comment = [l.lstrip("/").strip() for l in lines[:12] if l.startswith("//") and l.strip("/*").strip()]
            return "\n".join(comment)
    if name == "QuickJS" and folder.is_file():
        # Kept in a string inside the header, not a comment.
        text = folder.read_text(encoding="utf-8", errors="replace")
        match = re.search(r"(Copyright \(c\) 2017.*?THE SOFTWARE\.)", text, re.DOTALL)
        return match.group(1) if match else None
    return None


def licence_files(folder: pathlib.Path):
    """The licence files at the top of a vendored folder (or one level down)."""
    found = [p for p in sorted(folder.iterdir()) if p.is_file() and LICENCE_NAMES.search(p.name) and p.name != "JUCE_UPSTREAM.txt"]
    if not found:
        for sub in sorted(p for p in folder.iterdir() if p.is_dir()):
            found += [p for p in sorted(sub.iterdir()) if p.is_file() and LICENCE_NAMES.search(p.name) and p.name != "JUCE_UPSTREAM.txt"]
    return found


def licence_comment(path: pathlib.Path):
    """The first comment block in a source file that carries a copyright and licence."""
    text = path.read_text(encoding="utf-8", errors="replace")
    for match in re.finditer(r"/\*(.*?)\*/", text, re.DOTALL):
        body = match.group(1)
        lower = body.lower()
        if "copyright" in lower and ("permission" in lower or "license" in lower or "licence" in lower):
            lines = [re.sub(r"^\s*\*? ?", "", line) for line in body.splitlines()]
            return "\n".join(lines).strip()
    return None


def header_licence(folder: pathlib.Path):
    """For libraries that keep their licence in a source header (zlib, the JPEG library)."""
    if folder.is_file():
        return licence_comment(folder), folder
    for name in ("zlib.h", "README", "jpeglib.h", "png.h"):
        for path in folder.rglob(name):
            text = licence_comment(path)
            if text:
                return text, path
    return None, None


def main():
    spdx_path = JUCE / "JUCE.spdx.json"
    if not spdx_path.exists():
        sys.exit("error: configure the build first (cmake -S . -B build), so JUCE is downloaded")
    spdx = json.loads(spdx_path.read_text(encoding="utf-8"))
    juce_version = next(p["versionInfo"] for p in spdx["packages"] if p["name"] == "JUCE")

    sections, missing = [], []
    for p in spdx["packages"]:
        name = p["name"]
        info = p.get("sourceInfo", "")
        where = re.search(r"Vendored at (\S+)", info)
        if name in NOT_BUILT or not where or name.startswith("juce_") or name in ("JUCE", "examples", "extras", "build-system"):
            continue
        folder = JUCE / where.group(1)
        if not folder.exists():
            missing.append(name)
            continue
        texts = []
        if folder.is_dir():
            for path in licence_files(folder):
                texts.append(path.read_text(encoding="utf-8", errors="replace").strip())
        if not texts:
            special = special_cases(name, folder)
            if special:
                texts.append(special)
        if not texts:
            text, path = header_licence(folder)
            if text:
                texts.append(text)
        if not texts:
            missing.append(name)
            continue
        version = p.get("versionInfo", "")
        title = f"{name} {version}".strip()
        licence = p.get("licenseConcluded", "")
        sections.append(f"{title}  ({licence})\n{p.get('downloadLocation', '')}\n\n" + "\n\n".join(texts))

    rule = "=" * 78
    head = f"""SynthMatch: third-party notices

SynthMatch is free software, licensed under the GNU Affero General Public
License version 3 or later (see LICENSE.md). It is built with the JUCE framework
{juce_version} (Copyright Raw Material Software Limited), which SynthMatch uses
under the AGPLv3. JUCE, and the libraries below that it includes, are used under
the licences that follow. Each notice is reproduced from the library's own
licence file.

Not included in SynthMatch, but used by its optional AI assistant when you
install them: Ollama (MIT licence, ollama.com) and the Qwen3 language model
(Apache License 2.0). The installers download them from their official sources.
"""
    OUT.write_text(head + "\n" + rule + "\n" + f"\n\n{rule}\n".join(sections) + "\n", encoding="utf-8")
    print(f"Wrote {OUT.name}: {len(sections)} components")
    if missing:
        print("No licence text found for (check by hand):", ", ".join(missing))


if __name__ == "__main__":
    main()
