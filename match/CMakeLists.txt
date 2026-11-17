# The matcher: analysis of a reference recording, the search for a matching
# preset, the EQ match, and the trained encoder that gives the search its
# starting point. Plain C++ like the engine: no JUCE.
add_library(synthmatch_match STATIC
    src/Encoder.cpp
    src/Features.cpp
    src/Matcher.cpp
    src/Modulation.cpp
    src/Optimizer.cpp
    src/Pitch.cpp
    src/SearchSpace.cpp
)
add_library(synthmatch::match ALIAS synthmatch_match)

target_include_directories(synthmatch_match PUBLIC include)
target_link_libraries(synthmatch_match PUBLIC synthmatch::engine)
set_target_properties(synthmatch_match PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
target_link_libraries(synthmatch_match PUBLIC Threads::Threads)

# Training the encoder. Kept in its own library so the plugin never links it.
add_library(synthmatch_train STATIC src/Train.cpp)
add_library(synthmatch::train ALIAS synthmatch_train)
target_link_libraries(synthmatch_train PUBLIC synthmatch::match)

if(MSVC)
    target_compile_options(synthmatch_match PRIVATE /W4 /permissive-)
    target_compile_options(synthmatch_train PRIVATE /W4 /permissive-)
else()
    target_compile_options(synthmatch_match PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(synthmatch_train PRIVATE -Wall -Wextra -Wpedantic)
endif()
