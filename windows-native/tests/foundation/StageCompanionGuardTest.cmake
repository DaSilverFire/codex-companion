if(NOT DEFINED PROJECT_BUILD_DIR OR PROJECT_BUILD_DIR STREQUAL "")
  message(FATAL_ERROR "PROJECT_BUILD_DIR is required.")
endif()
if(NOT DEFINED STAGE_SCRIPT OR STAGE_SCRIPT STREQUAL "")
  message(FATAL_ERROR "STAGE_SCRIPT is required.")
endif()
if(NOT DEFINED TEST_CONFIG OR TEST_CONFIG STREQUAL "")
  message(FATAL_ERROR "TEST_CONFIG is required.")
endif()

set(
  nested_stage_dir
  "${PROJECT_BUILD_DIR}/stage/${TEST_CONFIG}/nested"
)
file(MAKE_DIRECTORY "${nested_stage_dir}")
set(sentinel "${nested_stage_dir}/must-survive.txt")
file(WRITE "${sentinel}" "sentinel")

execute_process(
  COMMAND
    "${CMAKE_COMMAND}"
    "-DBUILD_DIR=${PROJECT_BUILD_DIR}"
    "-DSTAGE_DIR=${nested_stage_dir}"
    "-DCONFIG=${TEST_CONFIG}"
    -P "${STAGE_SCRIPT}"
  RESULT_VARIABLE stage_result
  OUTPUT_VARIABLE stage_stdout
  ERROR_VARIABLE stage_stderr
)

if(stage_result EQUAL 0)
  message(FATAL_ERROR
    "StageCompanion accepted a nested stage directory."
  )
endif()
if(NOT EXISTS "${sentinel}")
  message(FATAL_ERROR
    "StageCompanion deleted a nested stage directory before rejecting it."
  )
endif()

string(CONCAT stage_output "${stage_stdout}" "${stage_stderr}")
if(NOT stage_output MATCHES "exactly match")
  message(FATAL_ERROR
    "StageCompanion did not report the exact-directory guard."
  )
endif()

file(REMOVE_RECURSE "${nested_stage_dir}")
