function(cgra_enable_warnings target)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "Unknown target for warnings: ${target}")
  endif()

  target_compile_options("${target}" PRIVATE -Wall -Wextra -Wpedantic)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
     CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "13")
    # GCC 13+ diagnoses stable references returned by nlohmann::json::at()
    # through our checked accessors as dangling, although the owning root JSON
    # remains alive for the entire parse operation.
    target_compile_options("${target}" PRIVATE -Wno-dangling-reference)
  endif()
  if(CGRA_WARNINGS_AS_ERRORS)
    target_compile_options("${target}" PRIVATE -Werror)
  endif()
endfunction()
