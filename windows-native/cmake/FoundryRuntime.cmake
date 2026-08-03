set(COMPANION_FOUNDRY_RUNTIME_FILES
  "${COMPANION_FOUNDRY_ROOT}/packages/Microsoft.AI.Foundry.Local.Core.1.2.1/runtimes/win-x64/native/Microsoft.AI.Foundry.Local.Core.dll"
  "${COMPANION_FOUNDRY_ROOT}/packages/Microsoft.ML.OnnxRuntime.Foundry.1.26.0/runtimes/win-x64/native/onnxruntime.dll"
  "${COMPANION_FOUNDRY_ROOT}/packages/Microsoft.ML.OnnxRuntime.Foundry.1.26.0/runtimes/win-x64/native/onnxruntime_providers_shared.dll"
  "${COMPANION_FOUNDRY_ROOT}/packages/Microsoft.ML.OnnxRuntimeGenAI.Foundry.0.14.1/runtimes/win-x64/native/onnxruntime-genai.dll"
)

set(COMPANION_FOUNDRY_NOTICE_MAPPINGS
  "${COMPANION_FOUNDRY_ROOT}/source/LICENSE|Foundry-Local-LICENSE"
  "${COMPANION_FOUNDRY_ROOT}/packages/Microsoft.AI.Foundry.Local.Core.1.2.1/LICENSE.txt|Foundry-Core-LICENSE.txt"
  "${COMPANION_FOUNDRY_ROOT}/packages/Microsoft.ML.OnnxRuntime.Foundry.1.26.0/LICENSE|ONNX-Runtime-LICENSE"
  "${COMPANION_FOUNDRY_ROOT}/packages/Microsoft.ML.OnnxRuntime.Foundry.1.26.0/ThirdPartyNotices.txt|ONNX-Runtime-ThirdPartyNotices.txt"
  "${COMPANION_FOUNDRY_ROOT}/packages/Microsoft.ML.OnnxRuntimeGenAI.Foundry.0.14.1/LICENSE|ONNX-Runtime-GenAI-LICENSE"
  "${COMPANION_FOUNDRY_ROOT}/packages/Microsoft.ML.OnnxRuntimeGenAI.Foundry.0.14.1/ThirdPartyNotices.txt|ONNX-Runtime-GenAI-ThirdPartyNotices.txt"
)

function(companion_add_foundry_runtime target_name)
  set(foundry_runtime_root "${PROJECT_SOURCE_DIR}/.deps/foundry-local/1.2.1")
  add_custom_command(
    TARGET ${target_name}
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E make_directory
      "$<TARGET_FILE_DIR:${target_name}>/licenses/foundry-local"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      "${foundry_runtime_root}/packages/Microsoft.AI.Foundry.Local.Core.1.2.1/runtimes/win-x64/native/Microsoft.AI.Foundry.Local.Core.dll"
      "$<TARGET_FILE_DIR:${target_name}>"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      "${foundry_runtime_root}/packages/Microsoft.ML.OnnxRuntime.Foundry.1.26.0/runtimes/win-x64/native/onnxruntime.dll"
      "$<TARGET_FILE_DIR:${target_name}>"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      "${foundry_runtime_root}/packages/Microsoft.ML.OnnxRuntime.Foundry.1.26.0/runtimes/win-x64/native/onnxruntime_providers_shared.dll"
      "$<TARGET_FILE_DIR:${target_name}>"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      "${foundry_runtime_root}/packages/Microsoft.ML.OnnxRuntimeGenAI.Foundry.0.14.1/runtimes/win-x64/native/onnxruntime-genai.dll"
      "$<TARGET_FILE_DIR:${target_name}>"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      "${foundry_runtime_root}/source/LICENSE"
      "$<TARGET_FILE_DIR:${target_name}>/licenses/foundry-local/Foundry-Local-LICENSE"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      "${foundry_runtime_root}/packages/Microsoft.AI.Foundry.Local.Core.1.2.1/LICENSE.txt"
      "$<TARGET_FILE_DIR:${target_name}>/licenses/foundry-local/Foundry-Core-LICENSE.txt"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      "${foundry_runtime_root}/packages/Microsoft.ML.OnnxRuntime.Foundry.1.26.0/LICENSE"
      "$<TARGET_FILE_DIR:${target_name}>/licenses/foundry-local/ONNX-Runtime-LICENSE"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      "${foundry_runtime_root}/packages/Microsoft.ML.OnnxRuntime.Foundry.1.26.0/ThirdPartyNotices.txt"
      "$<TARGET_FILE_DIR:${target_name}>/licenses/foundry-local/ONNX-Runtime-ThirdPartyNotices.txt"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      "${foundry_runtime_root}/packages/Microsoft.ML.OnnxRuntimeGenAI.Foundry.0.14.1/LICENSE"
      "$<TARGET_FILE_DIR:${target_name}>/licenses/foundry-local/ONNX-Runtime-GenAI-LICENSE"
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      "${foundry_runtime_root}/packages/Microsoft.ML.OnnxRuntimeGenAI.Foundry.0.14.1/ThirdPartyNotices.txt"
      "$<TARGET_FILE_DIR:${target_name}>/licenses/foundry-local/ONNX-Runtime-GenAI-ThirdPartyNotices.txt"
    VERBATIM
  )
endfunction()

install(
  FILES ${COMPANION_FOUNDRY_RUNTIME_FILES}
  DESTINATION "${CMAKE_INSTALL_BINDIR}"
  COMPONENT FoundryRuntime
)
foreach(mapping IN LISTS COMPANION_FOUNDRY_NOTICE_MAPPINGS)
  string(REPLACE "|" ";" mapping_parts "${mapping}")
  list(GET mapping_parts 0 source_file)
  list(GET mapping_parts 1 output_name)
  install(
    FILES "${source_file}"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/CodexCompanion/licenses/foundry-local"
    RENAME "${output_name}"
    COMPONENT FoundryRuntime
  )
endforeach()
