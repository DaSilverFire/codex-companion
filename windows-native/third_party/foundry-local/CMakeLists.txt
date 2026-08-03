set(
  COMPANION_FOUNDRY_ROOT
  "${PROJECT_SOURCE_DIR}/.deps/foundry-local/1.2.1"
)
set(
  COMPANION_FOUNDRY_SOURCE_ROOT
  "${COMPANION_FOUNDRY_ROOT}/source/sdk/cpp"
)
set(
  COMPANION_FOUNDRY_BOOTSTRAP_ERROR
  "Foundry Local dependencies are missing or invalid. Run windows-native/tools/bootstrap-foundry-local.ps1."
)

set(required_foundry_files
  "${COMPANION_FOUNDRY_ROOT}/bootstrap-stamp.json"
  "${COMPANION_FOUNDRY_SOURCE_ROOT}/src/model.cpp"
  "${COMPANION_FOUNDRY_SOURCE_ROOT}/src/catalog.cpp"
  "${COMPANION_FOUNDRY_SOURCE_ROOT}/src/chat_client.cpp"
  "${COMPANION_FOUNDRY_SOURCE_ROOT}/src/audio_client.cpp"
  "${COMPANION_FOUNDRY_SOURCE_ROOT}/src/live_audio_types.cpp"
  "${COMPANION_FOUNDRY_SOURCE_ROOT}/src/live_audio_session.cpp"
  "${COMPANION_FOUNDRY_SOURCE_ROOT}/src/foundry_local_manager.cpp"
)
foreach(required_file IN LISTS required_foundry_files)
  if(NOT EXISTS "${required_file}")
    message(FATAL_ERROR "${COMPANION_FOUNDRY_BOOTSTRAP_ERROR}")
  endif()
endforeach()

set(foundry_hashed_files
  "${COMPANION_FOUNDRY_ROOT}/source/LICENSE|dbb4186093080916f718e921b8e3b50be0396faf297b3d16bec5d2f47b8f81cb"
  "${COMPANION_FOUNDRY_ROOT}/packages/Microsoft.AI.Foundry.Local.Core.1.2.1/runtimes/win-x64/native/Microsoft.AI.Foundry.Local.Core.dll|171e368e7e4579e60946bae9a1836d6634beb01b7422d69948094b6e49c355c7"
  "${COMPANION_FOUNDRY_ROOT}/packages/Microsoft.AI.Foundry.Local.Core.1.2.1/LICENSE.txt|833bf9711b2e75bf0b1ab667618ee4f4ea2a6f0fa4fb5f828f7221a0cbfb7702"
  "${COMPANION_FOUNDRY_ROOT}/packages/Microsoft.ML.OnnxRuntime.Foundry.1.26.0/runtimes/win-x64/native/onnxruntime.dll|6a4129504501cbd615efddc897345ec9557390b408887165ab5faf9812a54b31"
  "${COMPANION_FOUNDRY_ROOT}/packages/Microsoft.ML.OnnxRuntime.Foundry.1.26.0/runtimes/win-x64/native/onnxruntime_providers_shared.dll|97fc0ccc43386f8769a0afc43fb1dba3a066f718cd1fe0e8f540e24e0ecb61a7"
  "${COMPANION_FOUNDRY_ROOT}/packages/Microsoft.ML.OnnxRuntime.Foundry.1.26.0/LICENSE|c250d6278f0b47a6439fb7592b08b58a55eb9f535aa49a1db63211c3f982b674"
  "${COMPANION_FOUNDRY_ROOT}/packages/Microsoft.ML.OnnxRuntime.Foundry.1.26.0/ThirdPartyNotices.txt|fb0af774b4d7cffc5b9d046f2aaeade2f37df2f80abf8033c95dfffcc77a8866"
  "${COMPANION_FOUNDRY_ROOT}/packages/Microsoft.ML.OnnxRuntimeGenAI.Foundry.0.14.1/runtimes/win-x64/native/onnxruntime-genai.dll|762a76aa622eb2e7b1ee977752e1a30f763669d1037e9be18d036668e0b1ef27"
  "${COMPANION_FOUNDRY_ROOT}/packages/Microsoft.ML.OnnxRuntimeGenAI.Foundry.0.14.1/LICENSE|9906940f61b1f0b533fa7d99baf55178b2808fbe113ea51dfbfad8572ccd5f2b"
  "${COMPANION_FOUNDRY_ROOT}/packages/Microsoft.ML.OnnxRuntimeGenAI.Foundry.0.14.1/ThirdPartyNotices.txt|c3adb0951a5964aa6092b88faa07efdb0037074bb0092e92d1fc7ace31e9c542"
)
foreach(mapping IN LISTS foundry_hashed_files)
  string(REPLACE "|" ";" mapping_parts "${mapping}")
  list(GET mapping_parts 0 source_file)
  list(GET mapping_parts 1 expected_hash)
  if(NOT EXISTS "${source_file}")
    message(FATAL_ERROR "${COMPANION_FOUNDRY_BOOTSTRAP_ERROR}")
  endif()
  file(SHA256 "${source_file}" actual_hash)
  if(NOT actual_hash STREQUAL expected_hash)
    message(FATAL_ERROR "${COMPANION_FOUNDRY_BOOTSTRAP_ERROR}")
  endif()
endforeach()

find_package(nlohmann_json CONFIG REQUIRED)
find_package(Microsoft.GSL CONFIG REQUIRED)

add_library(companion_foundry_local STATIC
  "${COMPANION_FOUNDRY_SOURCE_ROOT}/src/model.cpp"
  "${COMPANION_FOUNDRY_SOURCE_ROOT}/src/catalog.cpp"
  "${COMPANION_FOUNDRY_SOURCE_ROOT}/src/chat_client.cpp"
  "${COMPANION_FOUNDRY_SOURCE_ROOT}/src/audio_client.cpp"
  "${COMPANION_FOUNDRY_SOURCE_ROOT}/src/live_audio_types.cpp"
  "${COMPANION_FOUNDRY_SOURCE_ROOT}/src/live_audio_session.cpp"
  "${COMPANION_FOUNDRY_SOURCE_ROOT}/src/foundry_local_manager.cpp"
)
target_include_directories(companion_foundry_local
  PUBLIC "${COMPANION_FOUNDRY_SOURCE_ROOT}/include"
  PRIVATE "${COMPANION_FOUNDRY_SOURCE_ROOT}/src"
)
target_link_libraries(companion_foundry_local PUBLIC
  nlohmann_json::nlohmann_json
  Microsoft.GSL::GSL
  companion_windows_api_floor
)
target_compile_features(companion_foundry_local PUBLIC cxx_std_17)
companion_enable_warnings(companion_foundry_local)

include(FoundryRuntime)
