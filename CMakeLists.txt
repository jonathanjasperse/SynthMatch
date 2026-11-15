cmake_minimum_required(VERSION 3.22)

# Must be set before project().
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(SynthMatch VERSION 0.1.0 LANGUAGES C CXX)

option(SYNTHMATCH_BUILD_PLUGIN "Build the JUCE plugin (downloads JUCE)" ON)
option(SYNTHMATCH_BUILD_TESTS "Build the tests" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static MSVC runtime so the plugin doesn't need the Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(engine)
add_subdirectory(match)
add_subdirectory(assist)
add_subdirectory(tools/render)
add_subdirectory(tools/match)
add_subdirectory(tools/train)
add_subdirectory(tools/edit)

if(SYNTHMATCH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SYNTHMATCH_BUILD_PLUGIN)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 9.0.5
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)

    add_subdirectory(plugin)
endif()
