qt_add_executable(companion_foundation_build_smoke
  BuildSmokeTests.cpp
)
target_link_libraries(companion_foundation_build_smoke PRIVATE
  companion_windows_api_floor
  companion_core
  Qt6::Test
)
companion_enable_warnings(companion_foundation_build_smoke)
add_test(NAME foundation.build-smoke COMMAND companion_foundation_build_smoke)

qt_add_executable(companion_foundation_settings
  SettingsRepositoryTests.cpp
)
target_link_libraries(companion_foundation_settings PRIVATE
  companion_windows_api_floor
  companion_core
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_foundation_settings)
add_test(NAME foundation.settings COMMAND companion_foundation_settings)

qt_add_executable(
  companion_foundation_installation_identity
  CompanionInstallationIdentityStoreTests.cpp
)
target_link_libraries(
  companion_foundation_installation_identity
  PRIVATE
    companion_windows_api_floor
    companion_core
    Qt6::Core
    Qt6::Test
)
companion_enable_warnings(
  companion_foundation_installation_identity
)
add_test(
  NAME foundation.installation-identity
  COMMAND companion_foundation_installation_identity
)

qt_add_executable(companion_foundation_command_bus
  CompanionCommandBusTests.cpp
)
target_link_libraries(companion_foundation_command_bus PRIVATE
  companion_windows_api_floor
  companion_core
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_foundation_command_bus)
add_test(
  NAME foundation.command-bus
  COMMAND companion_foundation_command_bus
)

qt_add_executable(companion_foundation_single_instance
  SingleInstanceGateTests.cpp
)
target_link_libraries(companion_foundation_single_instance PRIVATE
  companion_windows_api_floor
  companion_windows
  Qt6::Core
  Qt6::Network
  Qt6::Test
)
companion_enable_warnings(companion_foundation_single_instance)
add_test(NAME foundation.single-instance COMMAND companion_foundation_single_instance)

qt_add_executable(
  companion_foundation_power_availability
  WindowsPowerAvailabilityControllerTests.cpp
)
target_link_libraries(
  companion_foundation_power_availability
  PRIVATE
    companion_windows_api_floor
    companion_windows
    Qt6::Core
    Qt6::Test
)
companion_enable_warnings(
  companion_foundation_power_availability
)
add_test(
  NAME foundation.power-availability
  COMMAND companion_foundation_power_availability
)

qt_add_executable(companion_foundation_utility_window
  UtilityWindowPolicyTests.cpp
)
target_link_libraries(companion_foundation_utility_window PRIVATE
  companion_windows_api_floor
  companion_windows
  Qt6::Gui
  Qt6::Quick
  Qt6::Test
)
companion_enable_warnings(companion_foundation_utility_window)
add_dependencies(companion_foundation_utility_window companion-window-verifier)
add_test(NAME foundation.utility-window COMMAND companion_foundation_utility_window)
set_tests_properties(foundation.utility-window PROPERTIES
  ENVIRONMENT "COMPANION_WINDOW_VERIFIER=$<TARGET_FILE:companion-window-verifier>"
  RESOURCE_LOCK companion_desktop_ui
)

cmake_path(NORMAL_PATH COMPANION_GENERATED_ICON OUTPUT_VARIABLE COMPANION_APP_LIFECYCLE_ICON_NORMALIZED)
file(TO_NATIVE_PATH "${COMPANION_APP_LIFECYCLE_ICON_NORMALIZED}" COMPANION_APP_LIFECYCLE_ICON_PATH)
string(REPLACE "\\" "\\\\" COMPANION_APP_LIFECYCLE_ICON_PATH "${COMPANION_APP_LIFECYCLE_ICON_PATH}")
set(COMPANION_ICON_PATH "${COMPANION_APP_LIFECYCLE_ICON_PATH}")
configure_file(
  "${PROJECT_SOURCE_DIR}/resources/CodexCompanion.rc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/companion_foundation_app_lifecycle.rc"
  @ONLY
)

qt_add_executable(companion_foundation_app_lifecycle
  CompanionApplicationTests.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/companion_foundation_app_lifecycle.rc"
)
target_link_libraries(companion_foundation_app_lifecycle PRIVATE
  companion_windows_api_floor
  companion_app
  companion_windows
  Qt6::Gui
  Qt6::Test
)
companion_enable_warnings(companion_foundation_app_lifecycle)
add_dependencies(companion_foundation_app_lifecycle companion_generated_icon)
set_source_files_properties(
  "${CMAKE_CURRENT_BINARY_DIR}/companion_foundation_app_lifecycle.rc"
  PROPERTIES
    OBJECT_DEPENDS "${COMPANION_GENERATED_ICON}"
)
add_test(NAME foundation.app-lifecycle COMMAND companion_foundation_app_lifecycle)
set_tests_properties(foundation.app-lifecycle PROPERTIES
  RESOURCE_LOCK companion_desktop_ui
)

qt_add_executable(
  companion_foundation_pet_process_reactions
  PetProcessReactionControllerTests.cpp
)
target_link_libraries(
  companion_foundation_pet_process_reactions
  PRIVATE
    companion_windows_api_floor
    companion_app
    Qt6::Core
    Qt6::Test
)
companion_enable_warnings(
  companion_foundation_pet_process_reactions
)
add_test(
  NAME foundation.pet-process-reactions
  COMMAND companion_foundation_pet_process_reactions
)

qt_add_executable(companion_foundation_runtime_host_status
  RuntimeHostStatusDispatcherTests.cpp
)
target_link_libraries(companion_foundation_runtime_host_status PRIVATE
  companion_windows_api_floor
  companion_app
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_foundation_runtime_host_status)
add_test(
  NAME foundation.runtime-host-status
  COMMAND companion_foundation_runtime_host_status
)

qt_add_executable(companion_foundation_runtime_host
  CompanionRuntimeHostTests.cpp
)
target_link_libraries(companion_foundation_runtime_host PRIVATE
  companion_windows_api_floor
  companion_app
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_foundation_runtime_host)
add_test(
  NAME foundation.runtime-host
  COMMAND companion_foundation_runtime_host
)

qt_add_executable(
  companion_foundation_mobile_relay_router
  CompanionMobileRelayRouterTests.cpp
)
target_link_libraries(
  companion_foundation_mobile_relay_router
  PRIVATE
    companion_windows_api_floor
    companion_app
    Qt6::Core
    Qt6::Test
)
companion_enable_warnings(
  companion_foundation_mobile_relay_router
)
add_test(
  NAME mobile.relay-router
  COMMAND companion_foundation_mobile_relay_router
)

qt_add_executable(
  companion_foundation_mobile_host
  CompanionMobileHostTests.cpp
)
target_link_libraries(
  companion_foundation_mobile_host
  PRIVATE
    companion_windows_api_floor
    companion_app
    Qt6::Core
    Qt6::Network
    Qt6::Test
)
target_compile_definitions(
  companion_foundation_mobile_host
  PRIVATE
    COMPANION_QTWEBSOCKET_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/.deps/source/qtwebsockets/tests/auto/websockets/shared"
)
companion_enable_warnings(
  companion_foundation_mobile_host
)
add_test(
  NAME mobile.host
  COMMAND companion_foundation_mobile_host
)

qt_add_executable(
  companion_foundation_mobile_runtime_adapter
  CompanionMobileRuntimeAdapterTests.cpp
)
target_link_libraries(
  companion_foundation_mobile_runtime_adapter
  PRIVATE
    companion_windows_api_floor
    companion_app
    Qt6::Core
    Qt6::Test
)
companion_enable_warnings(
  companion_foundation_mobile_runtime_adapter
)
add_test(
  NAME mobile.runtime-adapter
  COMMAND companion_foundation_mobile_runtime_adapter
)

qt_add_executable(
  companion_foundation_chat_credentials
  ChatCredentialAvailabilityTests.cpp
)
target_link_libraries(
  companion_foundation_chat_credentials
  PRIVATE
    companion_windows_api_floor
    companion_app
    Qt6::Core
    Qt6::Test
)
companion_enable_warnings(
  companion_foundation_chat_credentials
)
add_test(
  NAME foundation.chat-credentials
  COMMAND companion_foundation_chat_credentials
)

qt_add_executable(
  companion_foundation_chatgpt_accent
  ChatGPTAccentThemeReaderTests.cpp
)
target_link_libraries(
  companion_foundation_chatgpt_accent
  PRIVATE
    companion_windows_api_floor
    companion_windows
    Qt6::Core
    Qt6::Test
)
companion_enable_warnings(
  companion_foundation_chatgpt_accent
)
add_test(
  NAME foundation.chatgpt-accent
  COMMAND companion_foundation_chatgpt_accent
)

cmake_path(NORMAL_PATH COMPANION_GENERATED_ICON OUTPUT_VARIABLE COMPANION_GENERATED_ICON_NORMALIZED)
file(TO_NATIVE_PATH "${COMPANION_GENERATED_ICON_NORMALIZED}" COMPANION_ICON_PATH)
string(REPLACE "\\" "\\\\" COMPANION_ICON_PATH "${COMPANION_ICON_PATH}")
configure_file(
  "${PROJECT_SOURCE_DIR}/resources/CodexCompanion.rc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/companion_foundation_icon.rc"
  @ONLY
)

qt_add_executable(companion_foundation_icon
  IconResourceTests.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/companion_foundation_icon.rc"
)
target_link_libraries(companion_foundation_icon PRIVATE
  companion_windows_api_floor
  Qt6::Core
  Qt6::Test
  Version
)
add_dependencies(
  companion_foundation_icon
  companion_generated_icon
  CodexCompanion
)
set_source_files_properties(
  "${CMAKE_CURRENT_BINARY_DIR}/companion_foundation_icon.rc"
  PROPERTIES
    OBJECT_DEPENDS "${COMPANION_GENERATED_ICON}"
)
companion_enable_warnings(companion_foundation_icon)
add_test(NAME foundation.icon COMMAND companion_foundation_icon)
set_tests_properties(foundation.icon PROPERTIES
  ENVIRONMENT
    "COMPANION_TEST_ICON=${COMPANION_GENERATED_ICON};COMPANION_TEST_RELAY_URL=${CompanionRelayURL};COMPANION_TEST_APP_EXECUTABLE=$<TARGET_FILE:CodexCompanion>"
)

qt_add_executable(companion_foundation_tray
  TrayIconContractTests.cpp
)
target_link_libraries(companion_foundation_tray PRIVATE
  companion_windows_api_floor
  companion_windows
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_foundation_tray)
add_test(NAME foundation.tray COMMAND companion_foundation_tray)

qt_add_executable(companion_foundation_backdrop
  BackdropControllerTests.cpp
)
target_link_libraries(companion_foundation_backdrop PRIVATE
  companion_windows_api_floor
  companion_windows
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_foundation_backdrop)
add_test(NAME foundation.backdrop COMMAND companion_foundation_backdrop)

qt_add_executable(companion_foundation_window_region
  WindowRegionPolicyTests.cpp
)
target_link_libraries(companion_foundation_window_region PRIVATE
  companion_windows_api_floor
  companion_windows
  Qt6::Gui
  Qt6::Quick
  Qt6::Test
)
companion_enable_warnings(companion_foundation_window_region)
add_test(
  NAME foundation.window-region
  COMMAND companion_foundation_window_region
)
set_tests_properties(
  foundation.window-region
  PROPERTIES
    RESOURCE_LOCK companion_desktop_ui
)

qt_add_executable(companion_windows_nearby_platform
  WindowsNearbyPlatformTests.cpp
)
target_link_libraries(companion_windows_nearby_platform PRIVATE
  companion_windows_api_floor
  companion_windows
  Qt6::Core
  Qt6::Network
  Qt6::Test
)
companion_enable_warnings(companion_windows_nearby_platform)
add_test(
  NAME mobile.nearby-windows-platform
  COMMAND companion_windows_nearby_platform
)

add_test(
  NAME foundation.stage-guard
  COMMAND
    "${CMAKE_COMMAND}"
    "-DPROJECT_BUILD_DIR=${PROJECT_BINARY_DIR}"
    "-DSTAGE_SCRIPT=${PROJECT_SOURCE_DIR}/cmake/StageCompanion.cmake"
    "-DTEST_CONFIG=$<CONFIG>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/StageCompanionGuardTest.cmake"
)

set(COMPANION_TEST_MSVC_RUNTIME_DIRECTORY "")
foreach(runtime_library IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
  get_filename_component(runtime_name "${runtime_library}" NAME)
  if(runtime_name STREQUAL "msvcp140.dll")
    get_filename_component(
      COMPANION_TEST_MSVC_RUNTIME_DIRECTORY
      "${runtime_library}"
      DIRECTORY
    )
    break()
  endif()
endforeach()
if(COMPANION_TEST_MSVC_RUNTIME_DIRECTORY STREQUAL "")
  message(FATAL_ERROR
    "The deployment smoke failure test could not locate the MSVC runtime."
  )
endif()
get_filename_component(
  COMPANION_TEST_MSVC_RUNTIME_ARCH_DIRECTORY
  "${COMPANION_TEST_MSVC_RUNTIME_DIRECTORY}"
  DIRECTORY
)
get_filename_component(
  COMPANION_TEST_MSVC_RUNTIME_VERSION_DIRECTORY
  "${COMPANION_TEST_MSVC_RUNTIME_ARCH_DIRECTORY}"
  DIRECTORY
)
get_filename_component(
  COMPANION_TEST_MSVC_RUNTIME_ARCH
  "${COMPANION_TEST_MSVC_RUNTIME_ARCH_DIRECTORY}"
  NAME
)
get_filename_component(
  COMPANION_TEST_MSVC_RUNTIME_NAME
  "${COMPANION_TEST_MSVC_RUNTIME_DIRECTORY}"
  NAME
)
string(
  REGEX REPLACE
  "\\.CRT$"
  ".DebugCRT"
  COMPANION_TEST_MSVC_DEBUG_RUNTIME_NAME
  "${COMPANION_TEST_MSVC_RUNTIME_NAME}"
)
set(
  COMPANION_TEST_MSVC_DEBUG_RUNTIME_DIRECTORY
  "${COMPANION_TEST_MSVC_RUNTIME_VERSION_DIRECTORY}/debug_nonredist/${COMPANION_TEST_MSVC_RUNTIME_ARCH}/${COMPANION_TEST_MSVC_DEBUG_RUNTIME_NAME}"
)
if(NOT IS_DIRECTORY
   "${COMPANION_TEST_MSVC_DEBUG_RUNTIME_DIRECTORY}")
  message(FATAL_ERROR
    "The deployment smoke failure test could not locate the MSVC debug runtime."
  )
endif()

add_test(
  NAME foundation.deployment-smoke-failure
  COMMAND
    "${CMAKE_COMMAND}"
    "-DPROJECT_BUILD_DIR=${PROJECT_BINARY_DIR}"
    "-DVERIFY_SCRIPT=${PROJECT_SOURCE_DIR}/tools/verify-deployed-app.ps1"
    "-DPOWERSHELL_EXECUTABLE=$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe"
    "-DUPDATER_EXECUTABLE=$<TARGET_FILE:CodexCompanionUpdater>"
    "-DQT_CORE_RUNTIME=$<TARGET_FILE:Qt6::Core>"
    "-DQWEBP_RUNTIME=${PROJECT_SOURCE_DIR}/.deps/qt-6.11.1/plugins/imageformats/qwebp.dll"
    "-DTEST_CONFIG=$<CONFIG>"
    "-DMSVC_RUNTIME_DIRECTORY=${COMPANION_TEST_MSVC_RUNTIME_DIRECTORY}"
    "-DMSVC_DEBUG_RUNTIME_DIRECTORY=${COMPANION_TEST_MSVC_DEBUG_RUNTIME_DIRECTORY}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyDeployedAppFailureTest.cmake"
)
