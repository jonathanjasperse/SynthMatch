add_library(synthmatch_engine STATIC
    src/Params.cpp
    src/Engine.cpp
    src/Json.cpp
    src/Preset.cpp
    src/Render.cpp
    src/Wav.cpp
)
add_library(synthmatch::engine ALIAS synthmatch_engine)

target_include_directories(synthmatch_engine PUBLIC include)
target_compile_features(synthmatch_engine PUBLIC cxx_std_20)

# PIC so the engine can later be linked into the Python extension module.
set_target_properties(synthmatch_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(synthmatch_engine PRIVATE /W4 /permissive-)
else()
    target_compile_options(synthmatch_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()
