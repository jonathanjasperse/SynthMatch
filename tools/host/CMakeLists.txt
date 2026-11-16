# synthmatch_host: renders sounds from other synth plugins (VST3, and AU on
# macOS) to WAV files, as references for training and grading. Needs JUCE, so
# it is only built with the plugin.
juce_add_console_app(synthmatch_host PRODUCT_NAME "synthmatch_host")

target_sources(synthmatch_host PRIVATE main.cpp)

target_compile_definitions(synthmatch_host PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(synthmatch_host
    PRIVATE
        synthmatch::match
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Hosting Audio Units on macOS uses Apple's CoreAudioKit (for the generic
# plugin view), which JUCE doesn't link by itself for a console app.
if(APPLE)
    target_link_libraries(synthmatch_host PRIVATE "-framework CoreAudioKit")
endif()
