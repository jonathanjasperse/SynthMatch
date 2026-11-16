# Builds SynthMatch in Release, runs the tests, and writes the Windows installer
# to dist\SynthMatch-<version>-Windows.exe.
#
#   powershell -ExecutionPolicy Bypass -File scripts\package-windows.ps1
#
# Needs Inno Setup 6.3 or newer:  winget install JRSoftware.InnoSetup

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$version = (Select-String -Path CMakeLists.txt -Pattern 'project\(SynthMatch VERSION ([0-9.]+)').Matches[0].Groups[1].Value
Write-Host "SynthMatch $version"

cmake -S . -B build
if ($LASTEXITCODE -ne 0) { exit 1 }
cmake --build build --config Release --parallel
if ($LASTEXITCODE -ne 0) { exit 1 }
ctest --test-dir build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { exit 1 }

# Finds Inno Setup and packages the Release build (packaging\windows\build-installer.cmake).
cmake --build build --config Release --target SynthMatch_Installer
if ($LASTEXITCODE -ne 0) { exit 1 }
Write-Host "Done: dist\SynthMatch-$version-Windows.exe"
