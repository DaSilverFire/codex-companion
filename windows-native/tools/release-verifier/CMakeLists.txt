find_package(Git QUIET)

set(COMPANION_RELEASE_SOURCE_COMMIT "")
set(COMPANION_RELEASE_SOURCE_TREE "")
set(COMPANION_RELEASE_WEBSOCKETS_COMMIT "")
set(COMPANION_RELEASE_MONOCYPHER_COMMIT "")

function(
  companion_release_git_value
  working_directory
  output_variable
)
  if(NOT Git_FOUND)
    set(${output_variable} "" PARENT_SCOPE)
    return()
  endif()

  execute_process(
    COMMAND
      "${GIT_EXECUTABLE}"
      -C "${working_directory}"
      rev-parse "${ARGN}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE value
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(result EQUAL 0 AND value MATCHES "^[0-9a-fA-F]{40,64}$")
    string(TOLOWER "${value}" value)
    set(${output_variable} "${value}" PARENT_SCOPE)
  else()
    set(${output_variable} "" PARENT_SCOPE)
  endif()
endfunction()

companion_release_git_value(
  "${PROJECT_SOURCE_DIR}/.."
  COMPANION_RELEASE_SOURCE_COMMIT
  HEAD
)
companion_release_git_value(
  "${PROJECT_SOURCE_DIR}/.."
  COMPANION_RELEASE_SOURCE_TREE
  "HEAD^{tree}"
)
companion_release_git_value(
  "${PROJECT_SOURCE_DIR}/.deps/source/qtwebsockets"
  COMPANION_RELEASE_WEBSOCKETS_COMMIT
  HEAD
)
companion_release_git_value(
  "${PROJECT_SOURCE_DIR}/.deps/source/monocypher"
  COMPANION_RELEASE_MONOCYPHER_COMMIT
  HEAD
)

set(
  COMPANION_RELEASE_COMPILER_VERSION
  "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
)
set(
  COMPANION_RELEASE_WINDOWS_SDK_VERSION
  "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
)
if(
  COMPANION_RELEASE_WINDOWS_SDK_VERSION STREQUAL ""
  AND DEFINED ENV{WindowsSDKVersion}
)
  set(
    COMPANION_RELEASE_WINDOWS_SDK_VERSION
    "$ENV{WindowsSDKVersion}"
  )
  string(
    REGEX REPLACE
    "[/\\\\]+$"
    ""
    COMPANION_RELEASE_WINDOWS_SDK_VERSION
    "${COMPANION_RELEASE_WINDOWS_SDK_VERSION}"
  )
endif()

add_library(companion_release_verifier STATIC
  ReleaseVerifier.cpp
  ReleaseVerifier.h
  ReleaseVerifierProduction.cpp
  ReleaseVerifierProduction.h
)
target_include_directories(companion_release_verifier PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(companion_release_verifier PUBLIC
  companion_update
  Qt6::Core
)
target_compile_definitions(companion_release_verifier PRIVATE
  COMPANION_RELEASE_SOURCE_COMMIT="${COMPANION_RELEASE_SOURCE_COMMIT}"
  COMPANION_RELEASE_SOURCE_TREE="${COMPANION_RELEASE_SOURCE_TREE}"
  COMPANION_RELEASE_COMPILER_VERSION="${COMPANION_RELEASE_COMPILER_VERSION}"
  COMPANION_RELEASE_QT_VERSION="${Qt6Core_VERSION}"
  COMPANION_RELEASE_INNO_VERSION=""
  COMPANION_RELEASE_WINDOWS_SDK_VERSION="${COMPANION_RELEASE_WINDOWS_SDK_VERSION}"
  COMPANION_RELEASE_WEBSOCKETS_COMMIT="${COMPANION_RELEASE_WEBSOCKETS_COMMIT}"
  COMPANION_RELEASE_MONOCYPHER_COMMIT="${COMPANION_RELEASE_MONOCYPHER_COMMIT}"
)
companion_enable_warnings(companion_release_verifier)

qt_add_executable(companion-release-verifier
  main.cpp
)
target_link_libraries(companion-release-verifier PRIVATE
  companion_release_verifier
)
companion_enable_warnings(companion-release-verifier)
