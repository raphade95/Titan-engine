cmake_minimum_required(VERSION 3.16)
project(TitanCore CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Universal binary for Intel + Apple Silicon Macs (native builds only).
if(APPLE AND NOT EMSCRIPTEN)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Determinism matters more than the last few percent of speed: no fast-math.
if(MSVC)
    add_compile_options(/fp:precise)
else()
    add_compile_options(-fno-fast-math)
endif()

set(TITAN_SOURCES
    libTitanCore/src/TitanNoise.cpp
    libTitanCore/src/TerrainEngine.cpp
    libTitanCore/src/Erosion.cpp
    libTitanCore/src/Fluvial.cpp
    libTitanCore/src/Layers.cpp
    libTitanCore/src/Export.cpp
    libTitanCore/src/CAPI.cpp
)

add_library(TitanCore STATIC ${TITAN_SOURCES})
target_include_directories(TitanCore PUBLIC libTitanCore/include)

find_package(Threads REQUIRED)
target_link_libraries(TitanCore PUBLIC Threads::Threads)

# Test + benchmark binaries (native builds only).
if(NOT EMSCRIPTEN)
    add_executable(titan_tests tests/test_core.cpp)
    target_link_libraries(titan_tests PRIVATE TitanCore)

    add_executable(titan_bench tests/bench_core.cpp)
    target_link_libraries(titan_bench PRIVATE TitanCore)
endif()
