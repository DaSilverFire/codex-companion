if(NOT DEFINED PROJECT_BUILD_DIR OR PROJECT_BUILD_DIR STREQUAL "")
  message(FATAL_ERROR "PROJECT_BUILD_DIR is required.")
endif()
if(NOT DEFINED VERIFY_SCRIPT OR VERIFY_SCRIPT STREQUAL "")
  message(FATAL_ERROR "VERIFY_SCRIPT is required.")
endif()
if(NOT DEFINED POWERSHELL_EXECUTABLE OR POWERSHELL_EXECUTABLE STREQUAL "")
  message(FATAL_ERROR "POWERSHELL_EXECUTABLE is required.")
endif()
if(NOT DEFINED UPDATER_EXECUTABLE OR UPDATER_EXECUTABLE STREQUAL "")
  message(FATAL_ERROR "UPDATER_EXECUTABLE is required.")
endif()
if(NOT DEFINED QT_CORE_RUNTIME OR QT_CORE_RUNTIME STREQUAL "")
  message(FATAL_ERROR "QT_CORE_RUNTIME is required.")
endif()
if(NOT DEFINED QWEBP_RUNTIME OR QWEBP_RUNTIME STREQUAL "")
  message(FATAL_ERROR "QWEBP_RUNTIME is required.")
endif()
if(
  NOT DEFINED MSVC_RUNTIME_DIRECTORY
  OR MSVC_RUNTIME_DIRECTORY STREQUAL ""
)
  message(FATAL_ERROR "MSVC_RUNTIME_DIRECTORY is required.")
endif()
if(
  NOT DEFINED MSVC_DEBUG_RUNTIME_DIRECTORY
  OR MSVC_DEBUG_RUNTIME_DIRECTORY STREQUAL ""
)
  message(FATAL_ERROR "MSVC_DEBUG_RUNTIME_DIRECTORY is required.")
endif()

set(
  test_directory
  "${PROJECT_BUILD_DIR}/verify-deployed-app-failure"
)
file(REMOVE_RECURSE "${test_directory}")
file(MAKE_DIRECTORY "${test_directory}")

set(
  invalid_executable
  "${test_directory}/invalid-companion.exe"
)
file(WRITE "${invalid_executable}" "not a Windows executable")

file(MAKE_DIRECTORY "${test_directory}/plugins/imageformats")
file(
  COPY_FILE
  "${QWEBP_RUNTIME}"
  "${test_directory}/plugins/imageformats/qwebp.dll"
  ONLY_IF_DIFFERENT
)
file(
  COPY_FILE
  "${UPDATER_EXECUTABLE}"
  "${test_directory}/CodexCompanionUpdater.exe"
  ONLY_IF_DIFFERENT
)
get_filename_component(qt_core_runtime_name "${QT_CORE_RUNTIME}" NAME)
file(
  COPY_FILE
  "${QT_CORE_RUNTIME}"
  "${test_directory}/${qt_core_runtime_name}"
  ONLY_IF_DIFFERENT
)
if(NOT qt_core_runtime_name STREQUAL "Qt6Core.dll")
  file(
    COPY_FILE
    "${QT_CORE_RUNTIME}"
    "${test_directory}/Qt6Core.dll"
    ONLY_IF_DIFFERENT
  )
endif()

foreach(runtime_file IN ITEMS
  msvcp140.dll
  msvcp140_1.dll
  msvcp140_atomic_wait.dll
  vcruntime140.dll
  vcruntime140_1.dll
)
  file(
    COPY_FILE
    "${MSVC_RUNTIME_DIRECTORY}/${runtime_file}"
    "${test_directory}/${runtime_file}"
    ONLY_IF_DIFFERENT
  )
endforeach()

if(TEST_CONFIG STREQUAL "Debug")
  file(
    GLOB debug_runtime_files
    "${MSVC_DEBUG_RUNTIME_DIRECTORY}/*.dll"
  )
  foreach(runtime_file IN LISTS debug_runtime_files)
    get_filename_component(runtime_name "${runtime_file}" NAME)
    file(
      COPY_FILE
      "${runtime_file}"
      "${test_directory}/${runtime_name}"
      ONLY_IF_DIFFERENT
    )
  endforeach()
endif()

execute_process(
  COMMAND
    "${POWERSHELL_EXECUTABLE}"
    -NoProfile
    -ExecutionPolicy Bypass
    -File "${VERIFY_SCRIPT}"
    -ExecutablePath "${invalid_executable}"
    -StartupTimeoutMilliseconds 1000
  RESULT_VARIABLE verify_result
  OUTPUT_VARIABLE verify_stdout
  ERROR_VARIABLE verify_stderr
)

if(verify_result EQUAL 0)
  message(FATAL_ERROR
    "verify-deployed-app accepted an invalid executable."
  )
endif()

string(
  CONCAT verify_output
  "${verify_stdout}"
  "${verify_stderr}"
)
if(NOT verify_output MATCHES "invalid-companion.exe")
  message(FATAL_ERROR
    "The original process-start diagnostic was masked: ${verify_output}"
  )
endif()
if(verify_output MATCHES
    "No process is associated|Process has not been started"
)
  message(FATAL_ERROR
    "Cleanup replaced the original process-start diagnostic: ${verify_output}"
  )
endif()

file(REMOVE_RECURSE "${test_directory}")
