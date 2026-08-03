get_filename_component(
  COMPANION_NATIVE_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../.."
  ABSOLUTE
)
set(
  COMPANION_MONOCYPHER_SOURCE_ROOT
  "${COMPANION_NATIVE_ROOT}/.deps/source/monocypher/src"
)
configure_file(
  UpdateBuildConfiguration.h.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/update/UpdateBuildConfiguration.h"
  @ONLY
)
configure_file(
  "${PROJECT_SOURCE_DIR}/resources/update-channel.json.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/update-channel.json"
  @ONLY
)

add_library(companion_monocypher_ed25519 STATIC
  "${COMPANION_MONOCYPHER_SOURCE_ROOT}/monocypher.c"
  "${COMPANION_MONOCYPHER_SOURCE_ROOT}/optional/monocypher-ed25519.c"
)
target_include_directories(companion_monocypher_ed25519 PUBLIC
  "${COMPANION_MONOCYPHER_SOURCE_ROOT}"
  "${COMPANION_MONOCYPHER_SOURCE_ROOT}/optional"
)
set_target_properties(companion_monocypher_ed25519 PROPERTIES
  AUTOMOC OFF
  AUTORCC OFF
  AUTOUIC OFF
  C_STANDARD 99
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
)
if(MSVC)
  target_compile_options(companion_monocypher_ed25519 PRIVATE /W0)
endif()

add_library(companion_update STATIC
  ../platform/windows/AuthenticodeVerifier.cpp
  ../platform/windows/AuthenticodeVerifier.h
  ../platform/windows/InstallerMetadataReader.cpp
  ../platform/windows/InstallerMetadataReader.h
  ../platform/windows/PeImageInspector.cpp
  ../platform/windows/PeImageInspector.h
  ../platform/windows/WindowsVersionProvider.cpp
  ../platform/windows/WindowsVersionProvider.h
  Ed25519Verifier.h
  MonocypherEd25519Verifier.cpp
  MonocypherEd25519Verifier.h
  ReleaseVersion.cpp
  ReleaseVersion.h
  UpdateArtifactDownloader.cpp
  UpdateArtifactDownloader.h
  UpdateArtifactStore.cpp
  UpdateArtifactStore.h
  UpdateCompatibility.cpp
  UpdateCompatibility.h
  UpdateFeedClient.cpp
  UpdateFeedClient.h
  UpdateManifest.cpp
  UpdateManifest.h
  UpdateManifestValidator.cpp
  UpdateManifestValidator.h
  UpdateService.cpp
  UpdateService.h
)
set_target_properties(companion_update PROPERTIES
  AUTOMOC ON
  AUTORCC OFF
  AUTOUIC OFF
)
qt_add_resources(
  companion_update
  companion_update_channel
  PREFIX "/codex-companion"
  BASE "${CMAKE_CURRENT_BINARY_DIR}/generated"
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/generated/update-channel.json"
)
target_include_directories(companion_update PUBLIC
  "${COMPANION_NATIVE_ROOT}/src"
)
target_include_directories(companion_update PUBLIC
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_link_libraries(companion_update
  PUBLIC
    Qt6::Core
    Qt6::Network
  PRIVATE
    companion_monocypher_ed25519
    Crypt32
    Version
    Wintrust
)
if(TARGET companion_windows_api_floor)
  target_link_libraries(companion_update PUBLIC
    companion_windows_api_floor
  )
endif()
if(COMMAND companion_enable_warnings)
  companion_enable_warnings(companion_update)
endif()
