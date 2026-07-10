cmake_minimum_required(VERSION 3.16)
project(spky LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Portable engine: header-only for now; carries the include dir.
add_library(spky_engine INTERFACE)
target_include_directories(spky_engine INTERFACE engine)

# --- third-party (vendored single headers under third_party/, desktop only) ---
add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE third_party)

# --- tests ---
enable_testing()
add_executable(spky_tests
    tests/test_main.cpp
    tests/smoke.cpp
    tests/test_rng.cpp
    tests/test_waveforms.cpp
    tests/test_range.cpp
    engine/mod/lane.cpp
    tests/test_lane.cpp
    tests/test_step.cpp
    tests/test_evolve.cpp
    engine/mod/super_modulator.cpp
    tests/test_super_modulator.cpp
    engine/parts/part.cpp
    tests/test_part.cpp
    engine/instrument.cpp
    tests/test_instrument.cpp
)
target_link_libraries(spky_tests PRIVATE spky_engine doctest)
add_test(NAME spky_tests COMMAND spky_tests)
