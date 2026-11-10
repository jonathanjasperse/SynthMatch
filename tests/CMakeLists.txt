add_executable(synthmatch_engine_tests engine_tests.cpp)
target_link_libraries(synthmatch_engine_tests PRIVATE synthmatch::engine)
target_compile_definitions(synthmatch_engine_tests PRIVATE
    SYNTHMATCH_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

add_test(NAME engine COMMAND synthmatch_engine_tests)

# Smoke tests for the render CLI.
add_test(NAME render_list_params COMMAND synthmatch_render --list-params)
add_test(NAME render_wav
         COMMAND synthmatch_render --out ${CMAKE_CURRENT_BINARY_DIR}/smoke.wav --note 60 --note 64 --note 67
                 --length 0.5 --tail 0.5 --set amp_release_ms=300 --set osc1_wave=square --set osc2_level=0.5)
add_test(NAME render_rejects_bad_param COMMAND synthmatch_render --set not_a_param=1)
set_tests_properties(render_rejects_bad_param PROPERTIES WILL_FAIL TRUE)
