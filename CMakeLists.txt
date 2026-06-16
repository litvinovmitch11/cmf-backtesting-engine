cmake_minimum_required(VERSION 3.28)

project(cmf_backtesting_engine LANGUAGES CXX)

# Modern C++23 with classic headers (no modules) — clang-21 + libc++ via CMakePresets.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic)
add_compile_options($<$<CONFIG:Debug>:-fsanitize=address,undefined> $<$<CONFIG:Debug>:-g>)
add_link_options($<$<CONFIG:Debug>:-fsanitize=address,undefined>)

# --- Catch2 v3 (built from source; needs network on first configure) ---
include(FetchContent)
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.7.1
  GIT_SHALLOW    ON)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")

# --- nlohmann/json (header-only; config loading) ---
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    ON)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# ---------------------------------------------------------------------------
# bt — the engine library (public headers in include/, definitions in src/)
# ---------------------------------------------------------------------------
add_library(bt STATIC
  src/bt/data/csv_convert.cpp
  src/bt/data/lob_source.cpp
  src/bt/data/trades_source.cpp
  src/bt/data/feed_merger.cpp
  src/bt/exec/execution_simulator.cpp
  src/bt/strategy/fixed_spread_quoter.cpp
  src/bt/metrics/pnl_tracker.cpp
  src/bt/engine/backtest_engine.cpp
  src/bt/engine/config.cpp)
target_include_directories(bt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(bt PUBLIC cxx_std_23)
# The packed binary records are mmapped and read in place via reinterpret_cast.
target_compile_options(bt PUBLIC -fno-strict-aliasing)
target_link_libraries(bt PRIVATE nlohmann_json::nlohmann_json)

# ---------------------------------------------------------------------------
# Apps
# ---------------------------------------------------------------------------
add_executable(convert_csv apps/convert_csv.cpp)
target_link_libraries(convert_csv PRIVATE bt)

add_executable(backtest apps/backtest.cpp)
target_link_libraries(backtest PRIVATE bt)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_executable(bt_tests tests/test_core.cpp tests/test_data.cpp tests/test_book.cpp
                        tests/test_exec.cpp tests/test_strategy.cpp tests/test_engine.cpp)
target_link_libraries(bt_tests PRIVATE bt Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(bt_tests)
