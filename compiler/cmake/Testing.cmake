include(CMakeParseArguments)

function(cgra_add_test)
  cmake_parse_arguments(ARG "GTEST" "NAME;TYPE" "SOURCES;LIBRARIES;DEFINITIONS;COMMAND_ARGS;ENVIRONMENT" ${ARGN})
  if(NOT ARG_NAME OR NOT ARG_TYPE OR NOT ARG_SOURCES)
    message(FATAL_ERROR "cgra_add_test requires NAME, TYPE, and SOURCES")
  endif()

  set(timeout 10)
  if(ARG_TYPE STREQUAL "semantic")
    set(timeout 30)
  elseif(ARG_TYPE STREQUAL "e2e")
    set(timeout 120)
  elseif(NOT ARG_TYPE STREQUAL "unit")
    message(FATAL_ERROR "Unsupported test type: ${ARG_TYPE}")
  endif()

  set(test_environment
    "CGRA_TEST_SEED=0;CGRA_TEST_ARTIFACT_DIR=${CMAKE_BINARY_DIR}/failures")
  if(ARG_ENVIRONMENT)
    list(APPEND test_environment ${ARG_ENVIRONMENT})
  endif()

  add_executable("${ARG_NAME}" ${ARG_SOURCES})
  if(ARG_LIBRARIES)
    target_link_libraries("${ARG_NAME}" PRIVATE ${ARG_LIBRARIES})
  endif()
  if(ARG_DEFINITIONS)
    target_compile_definitions("${ARG_NAME}" PRIVATE ${ARG_DEFINITIONS})
  endif()
  target_include_directories("${ARG_NAME}" PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests"
  )
  cgra_enable_warnings("${ARG_NAME}")
  cgra_enable_sanitizers("${ARG_NAME}")

  if(ARG_GTEST)
    include(GoogleTest)
    gtest_discover_tests("${ARG_NAME}"
      DISCOVERY_TIMEOUT 30
      PROPERTIES
        LABELS "${ARG_TYPE}"
        TIMEOUT "${timeout}"
        ENVIRONMENT "${test_environment}"
    )
  else()
    add_test(NAME "${ARG_NAME}" COMMAND "${ARG_NAME}" ${ARG_COMMAND_ARGS})
    set_tests_properties("${ARG_NAME}" PROPERTIES
      LABELS "${ARG_TYPE}"
      TIMEOUT "${timeout}"
      ENVIRONMENT "${test_environment}"
    )
  endif()
endfunction()
