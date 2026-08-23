cmake_minimum_required(VERSION 3.20)

project(cgra_compiler_target_contract VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CGRA_BUILD_TESTS "Build compiler tests" ON)
option(CGRA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(CGRA_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(CGRA_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(CGRA_FETCH_DEPS "Fetch pinned CMake dependencies" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)
include(Testing)

if(CGRA_FETCH_DEPS)
  include(FetchContent)
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.10.5.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(nlohmann_json)

  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
else()
  find_package(nlohmann_json 3.10.5 EXACT REQUIRED)
  find_package(GTest QUIET)
endif()

add_library(cgra_target
  lib/Target/ControlLayout.cpp
  lib/Target/TargetModel.cpp
)
target_include_directories(cgra_target PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(cgra_target PUBLIC nlohmann_json::nlohmann_json)
cgra_enable_warnings(cgra_target)
cgra_enable_sanitizers(cgra_target)

add_executable(cgra-target-dump tools/target-dump/main.cpp)
target_link_libraries(cgra-target-dump PRIVATE cgra_target)
cgra_enable_warnings(cgra-target-dump)
cgra_enable_sanitizers(cgra-target-dump)
set_target_properties(cgra-target-dump PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

if(CGRA_BUILD_TESTS)
  enable_testing()

  cgra_add_test(
    NAME cgra-target-contract
    TYPE semantic
    SOURCES tests/Target/TargetModelTest.cpp
    LIBRARIES cgra_target
    DEFINITIONS CGRA_REPOSITORY_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
  )

  cgra_add_test(
    NAME cgra-test-support
    TYPE unit
    SOURCES tests/Support/SupportSmokeTest.cpp
            tests/support/TestArtifacts.cpp
            tests/support/TestSeed.cpp
    LIBRARIES nlohmann_json::nlohmann_json
    DEFINITIONS CGRA_REPOSITORY_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
  )
  cgra_add_test(
    NAME cgra-test-seed-env
    TYPE unit
    SOURCES tests/Support/SupportSmokeTest.cpp
            tests/support/TestArtifacts.cpp
            tests/support/TestSeed.cpp
    LIBRARIES nlohmann_json::nlohmann_json
    DEFINITIONS CGRA_REPOSITORY_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
    COMMAND_ARGS --expect-seed 42
    ENVIRONMENT CGRA_TEST_SEED=42
  )

  set(CGRA_GTEST_MAIN_TARGET)
  if(TARGET gtest_main)
    set(CGRA_GTEST_MAIN_TARGET gtest_main)
  elseif(TARGET GTest::gtest_main)
    set(CGRA_GTEST_MAIN_TARGET GTest::gtest_main)
  endif()
  if(CGRA_GTEST_MAIN_TARGET)
    cgra_add_test(
      NAME cgra-gtest-smoke
      TYPE unit
      SOURCES tests/Support/GTestSmokeTest.cpp
              tests/support/TestArtifacts.cpp
              tests/support/TestSeed.cpp
      LIBRARIES ${CGRA_GTEST_MAIN_TARGET} nlohmann_json::nlohmann_json
      DEFINITIONS CGRA_REPOSITORY_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
      GTEST
    )
  endif()
endif()
