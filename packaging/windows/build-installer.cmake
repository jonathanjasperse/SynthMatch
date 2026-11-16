# Builds the Windows installer with Inno Setup. Run by the SynthMatch_Installer
# target (plugin/CMakeLists.txt), which passes CONFIG, VERSION, BUILT (the
# folder holding VST3/ and Standalone/) and ISS (the Inno Setup script).
#
#   cmake --build build --config Release --target SynthMatch_Installer

foreach(var CONFIG VERSION BUILT ISS)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "build-installer.cmake needs -D${var}=...")
    endif()
endforeach()

if(NOT CONFIG STREQUAL "Release")
    message(FATAL_ERROR "The installer packages the Release build, but this is a ${CONFIG} build. "
                        "Switch to the Release configuration (\"x64 Release\" in Visual Studio) and build "
                        "SynthMatch_Installer again.")
endif()

# Looked up on every run, so installing Inno Setup needs no CMake re-run.
set(pf86 "ProgramFiles(x86)")
find_program(iscc ISCC
    PATHS "$ENV{${pf86}}/Inno Setup 6" "$ENV{ProgramFiles}/Inno Setup 6" "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6"
    NO_CACHE)
if(NOT iscc)
    message(FATAL_ERROR "Inno Setup 6.3 or newer is needed to build the installer. Install it with:\n"
                        "    winget install JRSoftware.InnoSetup")
endif()

file(TO_NATIVE_PATH "${BUILT}" built)
message(STATUS "Packaging SynthMatch ${VERSION} from ${built}")
execute_process(COMMAND "${iscc}" /Q "/DAppVersion=${VERSION}" "/DBuilt=${built}" "${ISS}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Inno Setup failed (exit code ${result}).")
endif()

get_filename_component(root "${ISS}/../../.." ABSOLUTE)
message(STATUS "Installer: ${root}/dist/SynthMatch-${VERSION}-Windows.exe")
