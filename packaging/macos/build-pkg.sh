#!/bin/bash
# Builds the macOS installer: dist/SynthMatch-<version>-macOS.pkg
#
#   packaging/macos/build-pkg.sh 0.2.0
#
# Build the plugin first, ideally as a universal binary:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
#   cmake --build build --config Release --parallel
#
# The installer has four parts the person can choose from: the VST3, the Audio
# Unit, the standalone app, and the AI assistant (which downloads Ollama from
# ollama.com and the language model; nothing is bundled).
#
# Signing and notarization happen only when these are set (see the README):
#   APP_SIGN_ID        "Developer ID Application: Your Name (TEAMID)"
#   INSTALLER_SIGN_ID  "Developer ID Installer: Your Name (TEAMID)"
#   NOTARY_APPLE_ID, NOTARY_TEAM_ID, NOTARY_PASSWORD (an app-specific password)

set -euo pipefail

VERSION="${1:?usage: build-pkg.sh VERSION}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILT="${BUILD_DIR:-$ROOT/build}/plugin/SynthMatch_artefacts/Release"
OUT="$ROOT/dist"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

for bundle in "VST3/SynthMatch.vst3" "AU/SynthMatch.component" "Standalone/SynthMatch.app"; do
    [ -e "$BUILT/$bundle" ] || { echo "error: $BUILT/$bundle not found; build the plugin first" >&2; exit 1; }
done

# Lay out each part as it will appear on disk.
mkdir -p "$STAGE/vst3/Library/Audio/Plug-Ins/VST3" "$STAGE/au/Library/Audio/Plug-Ins/Components" "$STAGE/app/Applications"
ditto "$BUILT/VST3/SynthMatch.vst3" "$STAGE/vst3/Library/Audio/Plug-Ins/VST3/SynthMatch.vst3"
ditto "$BUILT/AU/SynthMatch.component" "$STAGE/au/Library/Audio/Plug-Ins/Components/SynthMatch.component"
ditto "$BUILT/Standalone/SynthMatch.app" "$STAGE/app/Applications/SynthMatch.app"

# SynthMatch is free software (AGPLv3): the licence and the third-party notices
# always ship with it, alongside the VST3 (which can't be unticked).
DOCS="$STAGE/vst3/Library/Application Support/Bedrot Audio/SynthMatch"
mkdir -p "$DOCS"
cp "$ROOT/LICENSE.md" "$DOCS/License.txt"
cp "$ROOT/THIRD_PARTY_NOTICES.txt" "$DOCS/THIRD_PARTY_NOTICES.txt"

if [ -n "${APP_SIGN_ID:-}" ]; then
    echo "Signing the plugin and app"
    for bundle in "$STAGE/vst3/Library/Audio/Plug-Ins/VST3/SynthMatch.vst3" \
                  "$STAGE/au/Library/Audio/Plug-Ins/Components/SynthMatch.component" \
                  "$STAGE/app/Applications/SynthMatch.app"; do
        codesign --force --timestamp --options runtime --entitlements "$HERE/entitlements.plist" \
                 --sign "$APP_SIGN_ID" "$bundle"
    done
fi

# Plugin bundles must stay where they are put, even if a copy exists elsewhere.
for part in vst3 au app; do
    pkgbuild --analyze --root "$STAGE/$part" "$STAGE/$part.plist" >/dev/null
    plutil -replace 0.BundleIsRelocatable -bool NO "$STAGE/$part.plist"
    pkgbuild --root "$STAGE/$part" --component-plist "$STAGE/$part.plist" \
             --identifier "com.bedrotaudio.synthmatch.$part" --version "$VERSION" \
             --install-location / "$STAGE/synthmatch-$part.pkg"
done

# The assistant: no files, just a script that installs Ollama and fetches the model.
chmod +x "$HERE/assistant-scripts/postinstall"
pkgbuild --nopayload --scripts "$HERE/assistant-scripts" \
         --identifier com.bedrotaudio.synthmatch.assistant --version "$VERSION" \
         "$STAGE/synthmatch-assistant.pkg"

sed "s/@VERSION@/$VERSION/g" "$HERE/distribution.xml" > "$STAGE/distribution.xml"
mkdir -p "$OUT"
PKG="$OUT/SynthMatch-$VERSION-macOS.pkg"
SIGN_ARGS=()
if [ -n "${INSTALLER_SIGN_ID:-}" ]; then
    SIGN_ARGS=(--sign "$INSTALLER_SIGN_ID" --timestamp)
fi
productbuild --distribution "$STAGE/distribution.xml" --package-path "$STAGE" \
             --resources "$HERE/resources" ${SIGN_ARGS[@]+"${SIGN_ARGS[@]}"} "$PKG"

if [ -n "${NOTARY_APPLE_ID:-}" ] && [ -n "${INSTALLER_SIGN_ID:-}" ]; then
    echo "Notarizing (this usually takes a few minutes)"
    xcrun notarytool submit "$PKG" --apple-id "$NOTARY_APPLE_ID" --team-id "$NOTARY_TEAM_ID" \
                                   --password "$NOTARY_PASSWORD" --wait
    xcrun stapler staple "$PKG"
fi

echo "Wrote $PKG"
