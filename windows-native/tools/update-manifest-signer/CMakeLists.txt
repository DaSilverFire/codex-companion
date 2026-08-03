add_library(companion_update_manifest_signer STATIC
  UpdateManifestSigner.cpp
  UpdateManifestSigner.h
)
target_include_directories(companion_update_manifest_signer PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(companion_update_manifest_signer
  PUBLIC
    companion_update
    Qt6::Core
  PRIVATE
    companion_monocypher_ed25519
)
companion_enable_warnings(companion_update_manifest_signer)

qt_add_executable(companion-update-manifest-signer
  main.cpp
)
target_link_libraries(companion-update-manifest-signer PRIVATE
  companion_update_manifest_signer
  Qt6::Core
)
target_compile_definitions(companion-update-manifest-signer PRIVATE
  COMPANION_PROJECT_VERSION="${PROJECT_VERSION}"
)
companion_enable_warnings(companion-update-manifest-signer)
