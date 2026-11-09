set(SYNTHMATCH_PLUGIN_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND SYNTHMATCH_PLUGIN_FORMATS AU)
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
    COPY_PLUGIN_AFTER_BUILD FALSE
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
