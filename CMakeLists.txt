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

add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE third_party)

# --- tests ---
enable_testing()
add_executable(spky_tests
    tests/test_main.cpp
    tests/smoke.cpp
    tests/test_quantizer.cpp
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
    tests/test_wav.cpp
    host/render/scenario.cpp
    tests/test_scenario.cpp
)
target_link_libraries(spky_tests PRIVATE spky_engine doctest nlohmann_json)
target_include_directories(spky_tests PRIVATE host)
add_test(NAME spky_tests COMMAND spky_tests)

# --- render host CLI ---
add_executable(render
    host/render/main.cpp
    host/render/scenario.cpp
    engine/mod/lane.cpp
    engine/mod/super_modulator.cpp
    engine/parts/part.cpp
    engine/instrument.cpp
)
target_include_directories(render PRIVATE host engine)
target_link_libraries(render PRIVATE nlohmann_json)
