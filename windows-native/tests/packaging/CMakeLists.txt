qt_add_executable(companion_packaging_installer
  InstallerContractTests.cpp
)
target_link_libraries(companion_packaging_installer PRIVATE
  companion_windows_api_floor
  Qt6::Core
  Qt6::Test
  Version
)
target_compile_definitions(companion_packaging_installer PRIVATE
  COMPANION_INSTALLER_SCRIPT_PATH="${PROJECT_SOURCE_DIR}/packaging/CodexCompanion.iss"
  COMPANION_RELEASE_BUILD_SCRIPT_PATH="${PROJECT_SOURCE_DIR}/packaging/build-release.ps1"
)
companion_enable_warnings(companion_packaging_installer)
add_test(
  NAME packaging.installer
  COMMAND companion_packaging_installer
)

qt_add_executable(companion_packaging_release_verifier
  ReleaseVerifierTests.cpp
)
target_link_libraries(companion_packaging_release_verifier PRIVATE
  companion_release_verifier
  Qt6::Core
)
companion_enable_warnings(companion_packaging_release_verifier)
add_test(
  NAME packaging.release-verifier
  COMMAND companion_packaging_release_verifier
)

qt_add_executable(companion_packaging_manifest_signer
  UpdateManifestSignerTests.cpp
)
target_link_libraries(companion_packaging_manifest_signer PRIVATE
  companion_update_manifest_signer
  companion_update
  Qt6::Core
  Qt6::Test
)
target_compile_definitions(companion_packaging_manifest_signer PRIVATE
  COMPANION_MANIFEST_SIGNER_PATH="$<TARGET_FILE:companion-update-manifest-signer>"
)
add_dependencies(
  companion_packaging_manifest_signer
  companion-update-manifest-signer
)
companion_enable_warnings(companion_packaging_manifest_signer)
add_test(
  NAME packaging.manifest-signer
  COMMAND companion_packaging_manifest_signer
)

qt_add_executable(companion_packaging_cutover_automation
  CutoverAutomationTests.cpp
)
target_link_libraries(companion_packaging_cutover_automation PRIVATE
  companion_windows_api_floor
  Qt6::Core
  Qt6::Test
)
target_compile_definitions(companion_packaging_cutover_automation PRIVATE
  COMPANION_CLEAN_VM_CREATE_SCRIPT_PATH="${PROJECT_SOURCE_DIR}/packaging/clean-vm/New-CompanionTestVm.ps1"
  COMPANION_CLEAN_VM_INVOKE_SCRIPT_PATH="${PROJECT_SOURCE_DIR}/packaging/clean-vm/Invoke-CompanionCleanVmTest.ps1"
  COMPANION_CLEAN_VM_VERIFY_SCRIPT_PATH="${PROJECT_SOURCE_DIR}/packaging/clean-vm/verify-installed.ps1"
  COMPANION_CLEAN_VM_CONTRACT_MODULE_PATH="${PROJECT_SOURCE_DIR}/packaging/clean-vm/CleanVmContract.psm1"
  COMPANION_PORTABLE_DPI_MATRIX_SCRIPT_PATH="${PROJECT_SOURCE_DIR}/packaging/verify-portable-dpi-matrix.ps1"
  COMPANION_PET_DRAG_REVERSAL_SCRIPT_PATH="${PROJECT_SOURCE_DIR}/packaging/verify-pet-drag-reversal.ps1"
  COMPANION_DOTNET_FALLBACK_SCRIPT_PATH="${PROJECT_SOURCE_DIR}/packaging/build-dotnet-fallback.ps1"
  COMPANION_NATIVE_COMMAND_SCRIPT_PATH="${PROJECT_SOURCE_DIR}/tools/native-command.cmd"
  COMPANION_CURRENT_CANDIDATE_REBUILD_SCRIPT_PATH="${PROJECT_SOURCE_DIR}/tools/rebuild-and-launch-current-candidate.cmd"
  COMPANION_CODEX_TEST_CMAKE_PATH="${PROJECT_SOURCE_DIR}/tests/codex/CMakeLists.txt"
)
companion_enable_warnings(companion_packaging_cutover_automation)
add_test(
  NAME packaging.cutover-automation
  COMMAND companion_packaging_cutover_automation
)
