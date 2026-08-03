if(NOT DEFINED BUILD_DIR OR BUILD_DIR STREQUAL "")
  message(FATAL_ERROR "BUILD_DIR is required.")
endif()
if(NOT DEFINED STAGE_DIR OR STAGE_DIR STREQUAL "")
  message(FATAL_ERROR "STAGE_DIR is required.")
endif()
if(NOT DEFINED CONFIG OR CONFIG STREQUAL "")
  message(FATAL_ERROR "CONFIG is required.")
endif()
if(CONFIG MATCHES "[/\\\\]" OR CONFIG STREQUAL "." OR CONFIG STREQUAL "..")
  message(FATAL_ERROR "CONFIG must be one stage-directory name.")
endif()

cmake_path(ABSOLUTE_PATH BUILD_DIR NORMALIZE OUTPUT_VARIABLE normalized_build_dir)
cmake_path(ABSOLUTE_PATH STAGE_DIR NORMALIZE OUTPUT_VARIABLE normalized_stage_dir)
set(expected_stage_dir "${normalized_build_dir}/stage/${CONFIG}")
cmake_path(
  NORMAL_PATH
  expected_stage_dir
  OUTPUT_VARIABLE normalized_expected_stage_dir
)
cmake_path(
  COMPARE
  "${normalized_stage_dir}"
  EQUAL
  "${normalized_expected_stage_dir}"
  stage_is_expected_config
)
if(NOT stage_is_expected_config)
  message(FATAL_ERROR
    "STAGE_DIR must exactly match the configured stage directory: "
    "${normalized_expected_stage_dir}"
  )
endif()

file(REMOVE_RECURSE "${normalized_stage_dir}")
file(MAKE_DIRECTORY "${normalized_stage_dir}")

execute_process(
  COMMAND
    "${CMAKE_COMMAND}"
    --install "${normalized_build_dir}"
    --config "${CONFIG}"
    --prefix "${normalized_stage_dir}"
  RESULT_VARIABLE install_result
)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR
    "Companion staging failed with exit code ${install_result}."
  )
endif()
