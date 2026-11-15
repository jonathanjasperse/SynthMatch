# The assistant: turns typed requests ("make it brighter") into preset edits,
# with built-in rules and the prompt/answer format for a small language model.
# Plain C++ like the engine: no JUCE.
add_library(synthmatch_assist STATIC
    src/Assistant.cpp
    src/Edits.cpp
)
add_library(synthmatch::assist ALIAS synthmatch_assist)

target_include_directories(synthmatch_assist PUBLIC include)
target_link_libraries(synthmatch_assist PUBLIC synthmatch::engine)
set_target_properties(synthmatch_assist PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(synthmatch_assist PRIVATE /W4 /permissive-)
else()
    target_compile_options(synthmatch_assist PRIVATE -Wall -Wextra -Wpedantic)
endif()
