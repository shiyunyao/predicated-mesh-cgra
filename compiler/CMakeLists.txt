cmake_minimum_required(VERSION 3.20)

project(cgra_compiler_target_contract VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(nlohmann_json 3.10 REQUIRED)

add_library(cgra_target
  lib/Target/ControlLayout.cpp
  lib/Target/TargetModel.cpp
)
target_include_directories(cgra_target PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(cgra_target PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(cgra_target PRIVATE -Wall -Wextra -Wpedantic)

add_executable(cgra-target-dump tools/target-dump/main.cpp)
target_link_libraries(cgra-target-dump PRIVATE cgra_target)
set_target_properties(cgra-target-dump PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

include(CTest)
if(BUILD_TESTING)
  add_executable(cgra-target-tests tests/Target/TargetModelTest.cpp)
  target_link_libraries(cgra-target-tests PRIVATE cgra_target)
  target_compile_definitions(cgra-target-tests PRIVATE
    CGRA_REPOSITORY_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
  )
  target_compile_options(cgra-target-tests PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME cgra-target-contract COMMAND cgra-target-tests)
endif()
