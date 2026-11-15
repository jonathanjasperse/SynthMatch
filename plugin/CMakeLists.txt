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

juce_add_plugin(SynthMatch
    PRODUCT_NAME "SynthMatch"
    COMPANY_NAME "Bedrot Audio"
    BUNDLE_ID "com.bedrotaudio.synthmatch"
    PLUGIN_MANUFACTURER_CODE Bdrt
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

# The trained encoder ships inside the plugin, so matching works with no
# extra files. Train it with synthmatch_train (see the README).
set(SYNTHMATCH_ENCODER_FILE "${PROJECT_SOURCE_DIR}/models/encoder.smm")
if(EXISTS "${SYNTHMATCH_ENCODER_FILE}")
    juce_add_binary_data(SynthMatchData SOURCES "${SYNTHMATCH_ENCODER_FILE}")
    set_target_properties(SynthMatchData PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(SynthMatch PRIVATE SynthMatchData)
    target_compile_definitions(SynthMatch PRIVATE SYNTHMATCH_HAS_ENCODER=1)
else()
    message(STATUS "models/encoder.smm not found: the plugin will match without the encoder")
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
