add_executable(synthmatch_engine_tests engine_tests.cpp)
target_link_libraries(synthmatch_engine_tests PRIVATE synthmatch::engine)
target_compile_definitions(synthmatch_engine_tests PRIVATE
    SYNTHMATCH_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

add_test(NAME engine COMMAND synthmatch_engine_tests)

# Smoke tests for the render CLI.
add_test(NAME render_list_params COMMAND synthmatch_render --list-params)
add_test(NAME render_wav
         COMMAND synthmatch_render --out ${CMAKE_CURRENT_BINARY_DIR}/smoke.wav --note 60 --note 64 --note 67
                 --length 0.5 --tail 0.5 --set amp_release_ms=300 --set osc1_wave=square --set osc2_level=0.5
                 --set filter_type=bandpass --set filter_cutoff_hz=1200 --set filter_env_amount=2)
# Save a preset, then render from it.
add_test(NAME render_save_preset
         COMMAND synthmatch_render --out ${CMAKE_CURRENT_BINARY_DIR}/smoke2.wav --length 0.2 --tail 0.3
                 --set reverb_on=on --set delay_on=on --set delay_sync=1/8 --tempo 140 --set unison_voices=3
                 --save-preset ${CMAKE_CURRENT_BINARY_DIR}/smoke_preset.json)
add_test(NAME render_load_preset
         COMMAND synthmatch_render --out ${CMAKE_CURRENT_BINARY_DIR}/smoke3.wav --length 0.2 --tail 0.3
                 --preset ${CMAKE_CURRENT_BINARY_DIR}/smoke_preset.json)
set_tests_properties(render_load_preset PROPERTIES DEPENDS render_save_preset)
add_test(NAME render_rejects_bad_param COMMAND synthmatch_render --set not_a_param=1)
set_tests_properties(render_rejects_bad_param PROPERTIES WILL_FAIL TRUE)

# The matcher: analysis, note detection, the search, the EQ match, the encoder.
add_executable(synthmatch_match_tests match_tests.cpp)
target_link_libraries(synthmatch_match_tests PRIVATE synthmatch::train)
add_test(NAME match COMMAND synthmatch_match_tests)

# The assistant: rules, edits, and the language-model prompt and answers.
add_executable(synthmatch_assist_tests assist_tests.cpp)
target_link_libraries(synthmatch_assist_tests PRIVATE synthmatch::assist)
add_test(NAME assist COMMAND synthmatch_assist_tests)
