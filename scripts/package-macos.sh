#!/bin/bash
# Builds SynthMatch as a universal binary (Apple Silicon and Intel), runs the
# tests, and writes dist/SynthMatch-<version>-macOS.pkg.
#
#   scripts/package-macos.sh
#
# Needs Xcode (for pkgbuild and productbuild). See packaging/macos/build-pkg.sh
# for signing and notarization.

set -euo pipefail
cd "$(dirname "$0")/.."

VERSION="$(sed -n 's/^project(SynthMatch VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
echo "SynthMatch $VERSION"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure

packaging/macos/build-pkg.sh "$VERSION"
