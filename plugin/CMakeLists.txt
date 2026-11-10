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
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    ${SYNTHMATCH_COPY_ARGS}
    VST3_CATEGORIES Instrument Synth
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    FORMATS ${SYNTHMATCH_PLUGIN_FORMATS})

target_sources(SynthMatch PRIVATE
    PluginProcessor.cpp
    PluginProcessor.h)

target_compile_definitions(SynthMatch PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(SynthMatch
    PRIVATE
        synthmatch::engine
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
