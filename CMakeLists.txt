# driftmon — drift monitoring library.
# Zero external dependencies. Standard library only. C++17.
cmake_minimum_required(VERSION 3.14)
project(driftmon LANGUAGES CXX VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Required early: CMAKE_INSTALL_INCLUDEDIR/LIBDIR used in target_include_directories below.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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
target_include_directories(driftmon PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
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
  target_include_directories(driftmon_export PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/export>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )
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

# --- driftmon-cpp: C/C++ serving drift-monitoring framework (Phase 1) --------
# Default OFF. Air-gapped: standard library + POSIX (rt/pthread) only, no
# external packages. When OFF the core library and every other option above are
# byte-for-byte unaffected (mirrors the PROMETHEUS/TOOLS pattern).
option(DRIFTMON_ENABLE_CPP "Build the C++ tap/worker drift-monitoring framework" OFF)

if(DRIFTMON_ENABLE_CPP)
  find_package(Threads REQUIRED)
  find_library(RT_LIB rt)   # POSIX shm_open; folded into libc on some systems

  # Hot path in its own object so the symbol audit can inspect just this TU.
  add_library(driftmon_tap_hot OBJECT cpp/src/tap_hot.cpp)
  target_include_directories(driftmon_tap_hot PRIVATE cpp/include cpp/src src)

  add_library(driftmon_cpp STATIC
    cpp/src/shm_arena.cpp
    cpp/src/tap_init.cpp
    cpp/src/bundle.cpp
    cpp/src/psi_detector.cpp
    cpp/src/ks_detector.cpp
    cpp/src/cusum_detector.cpp
    cpp/src/adwin_detector.cpp
    cpp/src/export.cpp
    cpp/src/affinity.cpp
    cpp/src/worker.cpp
    $<TARGET_OBJECTS:driftmon_tap_hot>
  )
  target_include_directories(driftmon_cpp
    PUBLIC  cpp/include
    PRIVATE cpp/src src)
  target_link_libraries(driftmon_cpp PUBLIC driftmon Threads::Threads)  # driftmon: reuse dm::JsonParser
  if(RT_LIB)
    target_link_libraries(driftmon_cpp PUBLIC ${RT_LIB})
  endif()

  add_executable(driftmon_worker cpp/src/worker_main.cpp)
  target_link_libraries(driftmon_worker PRIVATE driftmon_cpp)

  # --- tests ---
  foreach(t test_shm_abi test_bundle test_psi_detector test_warmup_guard
            test_tap_noop test_arena_concurrency test_ks_detector test_detect
            test_multimodel test_fault_isolation test_streaming_detectors
            test_cpp_export)
    add_executable(${t} cpp/tests/${t}.cpp)
    target_link_libraries(${t} PRIVATE driftmon_cpp)
    target_include_directories(${t} PRIVATE cpp/src tests)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()

  # Latency bench: built, not a gating ctest.
  add_executable(bench_tap_latency cpp/tests/bench_tap_latency.cpp)
  target_link_libraries(bench_tap_latency PRIVATE driftmon_cpp)
  target_include_directories(bench_tap_latency PRIVATE cpp/src tests)

  # Static proof: tap hot path references no malloc/new/throw/lock/syscall.
  add_test(NAME tap_symbol_audit
    COMMAND ${CMAKE_COMMAND}
            -DOBJ=$<TARGET_OBJECTS:driftmon_tap_hot>
            -DNM=${CMAKE_NM}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cpp/tests/symbol_audit.cmake)

  # --- Cross-process end-to-end demo (real tap process + worker process) ---
  # tap_driver replays a CSV through the real tap; run_demo.py drives the worker
  # and asserts Zone A=STABLE / Zone E=SIGNIFICANT via the Prometheus export.
  add_executable(tap_driver cpp/examples/deepmimo_cpp/tap_driver.cpp)
  target_link_libraries(tap_driver PRIVATE driftmon_cpp)
  target_include_directories(tap_driver PRIVATE cpp/src)

  find_program(PYTHON3_CPP python3)
  if(PYTHON3_CPP)
    add_test(NAME deepmimo_cpp_e2e
      COMMAND ${PYTHON3_CPP} ${CMAKE_SOURCE_DIR}/cpp/examples/deepmimo_cpp/run_demo.py
              --build-dir ${CMAKE_BINARY_DIR})
    set_tests_properties(deepmimo_cpp_e2e PROPERTIES TIMEOUT 120)
  else()
    message(WARNING "python3 not found — deepmimo_cpp_e2e demo test skipped")
  endif()
endif()

# --- Install / CMake package export ------------------------------------------
# Allows downstream projects to use:
#   find_package(driftmon 1.0 CONFIG REQUIRED)
#   target_link_libraries(my_app PRIVATE driftmon::driftmon)

# Core library and public header.
install(TARGETS driftmon
    EXPORT driftmonTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES include/driftmon.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Optional Prometheus export adapter.
if(DRIFTMON_ENABLE_PROMETHEUS)
    install(TARGETS driftmon_export
        EXPORT driftmonTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    install(FILES export/driftmon_export.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endif()

# CMake package config files (for find_package support).
install(EXPORT driftmonTargets
    FILE driftmonTargets.cmake
    NAMESPACE driftmon::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/driftmon
)
configure_package_config_file(
    cmake/driftmonConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/driftmonConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/driftmon
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/driftmonConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/driftmonConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/driftmonConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/driftmon
)
