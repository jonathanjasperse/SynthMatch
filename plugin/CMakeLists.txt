set(SYNTHMATCH_PLUGIN_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND SYNTHMATCH_PLUGIN_FORMATS AU)
endif()

# Optional: copy the built plugin into the per-user plugin folder after every
# build, so the DAW always loads the latest one. No admin rights needed.
option(SYNTHMATCH_INSTALL_PLUGIN "Copy the plugin to the per-user plugin folder after building" OFF)

set(SYNTHMATCH_COPY_ARGS COPY_PLUGIN_AFTER_BUILD ${SYNTHMATCH_INSTALL_PLUGIN})
if(WIN32 AND SYNTHMATCH_INSTALL_PLUGIN)
    # The per-user VST3 folder on Windows. JUCE's default is the system folder
    # under Program Files, which needs admin rights.
    file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/Programs/Common/VST3" SYNTHMATCH_VST3_DIR)
    list(APPEND SYNTHMATCH_COPY_ARGS VST3_COPY_DIR "${SYNTHMATCH_VST3_DIR}")
endif()

# The app icon: the background tile (plugin/assets/tile.png) when there is one.
# JUCE turns it into the Windows .ico and the macOS .icns.
set(SYNTHMATCH_ICON_ARGS "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/tile.png")
    set(SYNTHMATCH_ICON_ARGS ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/assets/tile.png")
endif()

juce_add_plugin(SynthMatch
    ${SYNTHMATCH_ICON_ARGS}
    PRODUCT_NAME "SynthMatch"
    COMPANY_NAME "Jonathan Jasperse"
    BUNDLE_ID "com.jonathanjasperse.synthmatch"
    PLUGIN_MANUFACTURER_CODE Jjasp
    PLUGIN_CODE Smt1
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    ${SYNTHMATCH_COPY_ARGS}
    VST3_CATEGORIES Instrument Synth
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    FORMATS ${SYNTHMATCH_PLUGIN_FORMATS})

target_sources(SynthMatch PRIVATE
    PluginProcessor.cpp
    PluginProcessor.h
    PluginEditor.cpp
    PluginEditor.h
    Session.cpp
    Session.h
    LlmClient.cpp
    LlmClient.h
    ui/Theme.cpp
    ui/Theme.h
    ui/Backdrop.cpp
    ui/Backdrop.h
    ui/Terminal.cpp
    ui/About.cpp
    ui/About.h
    ui/Terminal.h
    ui/MainView.cpp
    ui/MainView.h
    ui/NodeView.cpp
    ui/NodeView.h)

target_include_directories(SynthMatch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Files built into the plugin, so it needs nothing else at run time.
set(SYNTHMATCH_BINARY_DATA "")

# The trained encoder, so matching works with no extra files. Train it with
# synthmatch_train (see the README).
set(SYNTHMATCH_ENCODER_FILE "${PROJECT_SOURCE_DIR}/models/encoder.smm")
if(EXISTS "${SYNTHMATCH_ENCODER_FILE}")
    list(APPEND SYNTHMATCH_BINARY_DATA "${SYNTHMATCH_ENCODER_FILE}")
    target_compile_definitions(SynthMatch PRIVATE SYNTHMATCH_HAS_ENCODER=1)
else()
    message(STATUS "models/encoder.smm not found: the plugin will match without the encoder")
endif()

# The background: your own tile image and its settings, if present in
# plugin/assets (see plugin/assets/README.md). Without a tile.png the drawn
# metal pentagon is used. CONFIGURE_DEPENDS makes every build look again, so
# adding or deleting tile.png needs no manual CMake run.
file(GLOB SYNTHMATCH_ASSETS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/*.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/*.json")
set(SYNTHMATCH_TILE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/assets/tile.png")
if("${SYNTHMATCH_TILE_FILE}" IN_LIST SYNTHMATCH_ASSETS)
    list(APPEND SYNTHMATCH_BINARY_DATA "${SYNTHMATCH_TILE_FILE}")
    target_compile_definitions(SynthMatch PRIVATE SYNTHMATCH_HAS_TILE=1)
    message(STATUS "Background tile: plugin/assets/tile.png")
endif()
set(SYNTHMATCH_BACKDROP_FILE "${CMAKE_CURRENT_SOURCE_DIR}/assets/backdrop.json")
if("${SYNTHMATCH_BACKDROP_FILE}" IN_LIST SYNTHMATCH_ASSETS)
    list(APPEND SYNTHMATCH_BINARY_DATA "${SYNTHMATCH_BACKDROP_FILE}")
    target_compile_definitions(SynthMatch PRIVATE SYNTHMATCH_HAS_BACKDROP_SETTINGS=1)
endif()

if(SYNTHMATCH_BINARY_DATA)
    juce_add_binary_data(SynthMatchData SOURCES ${SYNTHMATCH_BINARY_DATA})
    set_target_properties(SynthMatchData PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(SynthMatch PRIVATE SynthMatchData)
endif()

target_compile_definitions(SynthMatch PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(SynthMatch
    PRIVATE
        synthmatch::engine
        synthmatch::match
        synthmatch::assist
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# The Windows installer (Inno Setup), as a target you can build from an IDE or
# with: cmake --build build --config Release --target SynthMatch_Installer
# Not part of a normal build. See packaging/windows/build-installer.cmake.
if(WIN32)
    add_custom_target(SynthMatch_Installer
        COMMAND "${CMAKE_COMMAND}"
            "-DCONFIG=$<CONFIG>"
            "-DVERSION=${SynthMatch_VERSION}"
            "-DBUILT=$<TARGET_FILE_DIR:SynthMatch_Standalone>/.."
            "-DISS=${PROJECT_SOURCE_DIR}/packaging/windows/SynthMatch.iss"
            -P "${PROJECT_SOURCE_DIR}/packaging/windows/build-installer.cmake"
        COMMENT "Building the Windows installer"
        VERBATIM)
    add_dependencies(SynthMatch_Installer SynthMatch_VST3 SynthMatch_Standalone)
endif()
