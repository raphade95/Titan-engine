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

# Determinism matters more than the last few percent of speed.
#
# -fno-fast-math alone is NOT enough: it leaves floating-point *contraction*
# enabled, so `a * b + c` may be fused into a single-rounding FMA wherever the
# target has one. That makes results depend on the instruction set — arm64
# fuses, baseline x86_64 cannot — which showed up as the two slices of our own
# universal binary generating different terrain from the same seed. Contraction
# must be pinned off explicitly on every compiler.
if(MSVC)
    add_compile_options(/fp:precise /fp:contract-)
else()
    add_compile_options(-fno-fast-math -ffp-contract=off)
endif()

set(TITAN_SOURCES
    libTitanCore/src/TitanNoise.cpp
    libTitanCore/src/TerrainEngine.cpp
    libTitanCore/src/Erosion.cpp
    libTitanCore/src/Fluvial.cpp
    libTitanCore/src/Layers.cpp
    libTitanCore/src/Volcano.cpp
    libTitanCore/src/DemImport.cpp
    libTitanCore/src/Filters.cpp
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

    # Cross-platform golden hashes. Run in every CI matrix job — this is what
    # tests the "same seed, same terrain, every platform" promise.
    add_executable(titan_golden tests/test_golden.cpp)
    target_link_libraries(titan_golden PRIVATE TitanCore)

    add_executable(titan_bench tests/bench_core.cpp)
    target_link_libraries(titan_bench PRIVATE TitanCore)
endif()
