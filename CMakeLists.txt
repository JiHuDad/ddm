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

# --- Phase 4: reference generation tools & integration tests ---------------
# Default OFF. Requires python3. When ON:
#   make_reference.py --self-test generates a reference.json from synthetic data;
#   check_reference (C++) loads it via the core, completing the roundtrip.
option(DRIFTMON_ENABLE_TOOLS "Build reference tools and roundtrip integration test" OFF)

if(DRIFTMON_ENABLE_TOOLS)
  add_executable(check_reference tools/check_reference.cpp)
  target_link_libraries(check_reference PRIVATE driftmon)

  find_program(PYTHON3 python3)
  if(PYTHON3)
    # Step 1: Python tool generates reference.json from synthetic data.
    add_test(NAME ref_generate
      COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tools/make_reference.py
              --self-test --output ${CMAKE_BINARY_DIR}/test_ref.json
    )
    set_tests_properties(ref_generate PROPERTIES FIXTURES_SETUP ref_json_fixture)

    # Step 2: C++ loader round-trips the generated JSON via json_min (SPEC §3).
    add_test(NAME ref_load_cpp
      COMMAND $<TARGET_FILE:check_reference> ${CMAKE_BINARY_DIR}/test_ref.json
    )
    set_tests_properties(ref_load_cpp PROPERTIES FIXTURES_REQUIRED ref_json_fixture)

    # Step 3: Python unit tests for the tool itself.
    add_test(NAME test_make_reference
      COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_make_reference.py
    )
  else()
    message(WARNING "python3 not found — tools integration tests skipped")
  endif()
endif()

# --- Phase 5: integration examples -----------------------------------------
# Default OFF. Requires python3 (for the DeepMIMO end-to-end demo).
# When ON, builds demo_driver and adds a ctest that runs run_demo.py.
option(DRIFTMON_ENABLE_EXAMPLES "Build integration examples (DeepMIMO demo)" OFF)

if(DRIFTMON_ENABLE_EXAMPLES)
  # C++ command-line drift detection driver for the DeepMIMO demo.
  add_executable(demo_driver examples/deepmimo_demo/demo_driver.cpp)
  target_link_libraries(demo_driver PRIVATE driftmon)

  find_program(PYTHON3_EX python3)
  if(PYTHON3_EX)
    # End-to-end test: gen_zones → reference.json → Zone A (STABLE) + Zone E (SIGNIFICANT).
    add_test(NAME deepmimo_e2e
      COMMAND ${PYTHON3_EX}
              ${CMAKE_SOURCE_DIR}/examples/deepmimo_demo/run_demo.py
              --build-dir ${CMAKE_BINARY_DIR}
    )
    set_tests_properties(deepmimo_e2e PROPERTIES TIMEOUT 60)
  else()
    message(WARNING "python3 not found — deepmimo_e2e test skipped")
  endif()
endif()
