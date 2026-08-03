cmake_minimum_required(VERSION 4.0)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(CompanionUpdateFocusedTests LANGUAGES C CXX)

  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)

  find_package(Qt6 6.11.1 REQUIRED COMPONENTS Core Network Test)
  qt_standard_project_setup(REQUIRES 6.11.1)
  enable_testing()

  list(APPEND CMAKE_MODULE_PATH
    "${CMAKE_CURRENT_LIST_DIR}/../../cmake"
  )
  include(CompilerWarnings)
endif()

get_filename_component(
  COMPANION_NATIVE_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../.."
  ABSOLUTE
)

if(NOT TARGET companion_update)
  add_subdirectory(
    "${COMPANION_NATIVE_ROOT}/src/update"
    "${CMAKE_CURRENT_BINARY_DIR}/companion-update"
  )
endif()

function(companion_add_update_test target source test_name)
  qt_add_executable(${target} ${source} ${ARGN})
  target_link_libraries(${target} PRIVATE
    companion_update
    Qt6::Core
    Qt6::Network
    Qt6::Test
  )
  target_compile_definitions(${target} PRIVATE
    COMPANION_UPDATE_FIXTURE_ROOT="${COMPANION_NATIVE_ROOT}/tests/fixtures/update"
  )
  companion_enable_warnings(${target})
  add_test(NAME ${test_name} COMMAND ${target})
endfunction()

companion_add_update_test(
  companion_update_manifest_tests
  UpdateManifestTests.cpp
  update.manifest
)
companion_add_update_test(
  companion_update_version_tests
  ReleaseVersionTests.cpp
  update.version
)
companion_add_update_test(
  companion_update_signature_tests
  Ed25519VerifierTests.cpp
  update.signature
)
companion_add_update_test(
  companion_update_manifest_validator_tests
  UpdateManifestValidatorTests.cpp
  update.manifest-validator
)
companion_add_update_test(
  companion_update_feed_client_tests
  UpdateFeedClientTests.cpp
  update.transport
)
set_tests_properties(
  update.transport
  PROPERTIES TIMEOUT 30
)
companion_add_update_test(
  companion_update_artifact_store_tests
  UpdateArtifactStoreTests.cpp
  update.artifact
)
set_tests_properties(
  update.artifact
  PROPERTIES TIMEOUT 30
)
companion_add_update_test(
  companion_update_artifact_downloader_tests
  UpdateArtifactDownloaderTests.cpp
  update.artifact-downloader
)
set_tests_properties(
  update.artifact-downloader
  PROPERTIES TIMEOUT 30
)
companion_add_update_test(
  companion_update_service_tests
  UpdateServiceTests.cpp
  update.service
  "${COMPANION_NATIVE_ROOT}/src/app/UpdateViewModel.cpp"
)
set_tests_properties(
  update.service
  PROPERTIES TIMEOUT 30
)

qt_add_executable(
  companion_update_install_request_tests
  UpdateInstallRequestTests.cpp
)
target_link_libraries(
  companion_update_install_request_tests
  PRIVATE
    companion_updater_contract
    Qt6::Core
    Qt6::Test
)
companion_enable_warnings(
  companion_update_install_request_tests
)
add_test(
  NAME update.helper.request
  COMMAND
    companion_update_install_request_tests
)
set_tests_properties(
  update.helper.request
  PROPERTIES TIMEOUT 30
)

qt_add_executable(
  companion_update_install_result_tests
  UpdateInstallResultTests.cpp
)
target_link_libraries(
  companion_update_install_result_tests
  PRIVATE
    companion_updater_contract
    Qt6::Core
    Qt6::Test
)
companion_enable_warnings(
  companion_update_install_result_tests
)
add_test(
  NAME update.helper.result
  COMMAND
    companion_update_install_result_tests
)
set_tests_properties(
  update.helper.result
  PROPERTIES TIMEOUT 30
)

qt_add_executable(
  companion_update_install_transaction_tests
  UpdateInstallTransactionTests.cpp
)
target_link_libraries(
  companion_update_install_transaction_tests
  PRIVATE
    companion_updater_contract
    Qt6::Core
    Qt6::Test
)
companion_enable_warnings(
  companion_update_install_transaction_tests
)
add_test(
  NAME update.helper.transaction
  COMMAND
    companion_update_install_transaction_tests
)
set_tests_properties(
  update.helper.transaction
  PROPERTIES TIMEOUT 30
)

qt_add_executable(
  companion_update_installer_handoff_tests
  UpdateInstallerHandoffTests.cpp
)
target_link_libraries(
  companion_update_installer_handoff_tests
  PRIVATE
    companion_updater_contract
    Qt6::Core
    Qt6::Test
)
companion_enable_warnings(
  companion_update_installer_handoff_tests
)
add_test(
  NAME update.helper.handoff
  COMMAND
    companion_update_installer_handoff_tests
)
set_tests_properties(
  update.helper.handoff
  PROPERTIES TIMEOUT 30
)

configure_file(
  UpdateCompatibilityTest.rc.in
  "${CMAKE_CURRENT_BINARY_DIR}/UpdateCompatibilityTest.rc"
  @ONLY
)
companion_add_update_test(
  companion_update_compatibility_tests
  UpdateCompatibilityTests.cpp
  update.compatibility
  "${CMAKE_CURRENT_BINARY_DIR}/UpdateCompatibilityTest.rc"
)
