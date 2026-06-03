# driftmon — Phase 0 build skeleton.
# Zero external dependencies. Standard library only. C++17.
cmake_minimum_required(VERSION 3.14)
project(driftmon LANGUAGES CXX VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Treat warnings seriously in the core (kept clean per SPEC.md).
if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

# --- Core library (inference-engine agnostic) ------------------------------
add_library(driftmon STATIC
  src/driftmon.cpp
  src/json_min.cpp
)
target_include_directories(driftmon PUBLIC include)
target_include_directories(driftmon PRIVATE src)

# --- Tests (zero-dependency mini runner) -----------------------------------
enable_testing()
add_executable(driftmon_tests tests/test_driftmon.cpp)
target_link_libraries(driftmon_tests PRIVATE driftmon)
target_include_directories(driftmon_tests PRIVATE tests)
add_test(NAME driftmon_tests COMMAND driftmon_tests)

# --- Phase 3: optional Prometheus export adapter ---------------------------
# Default OFF: when OFF the core library and its tests are entirely unaffected.
# The export module depends on the core (reuses driftmon_classify), never the
# reverse. It is dependency-free (renders Prometheus text exposition format).
option(DRIFTMON_ENABLE_PROMETHEUS "Build the Prometheus export adapter" OFF)

if(DRIFTMON_ENABLE_PROMETHEUS)
  add_library(driftmon_export STATIC export/driftmon_export.cpp)
  target_include_directories(driftmon_export PUBLIC export)
  target_link_libraries(driftmon_export PUBLIC driftmon)  # export -> core only

  add_executable(driftmon_export_tests tests/test_export.cpp)
  target_link_libraries(driftmon_export_tests PRIVATE driftmon_export)
  target_include_directories(driftmon_export_tests PRIVATE tests)
  add_test(NAME driftmon_export_tests COMMAND driftmon_export_tests)
endif()
