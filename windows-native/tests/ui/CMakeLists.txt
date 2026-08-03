qt_add_executable(companion_ui_settings_qml
  SettingsQmlTestRunner.cpp
  "${PROJECT_BINARY_DIR}/tests/mobile/RelaySettingsTest.rc"
)
target_link_libraries(companion_ui_settings_qml PRIVATE
  companion_ui
  Qt6::QuickTest
)
companion_enable_warnings(companion_ui_settings_qml)
add_test(
  NAME ui.settings
  COMMAND companion_ui_settings_qml
    -input "${CMAKE_CURRENT_SOURCE_DIR}/tst_settings.qml"
    -import "${PROJECT_BINARY_DIR}/src/ui"
    -o -,txt
)

qt_add_executable(companion_ui_settings_view_model
  SettingsViewModelTests.cpp
  "${PROJECT_BINARY_DIR}/tests/mobile/RelaySettingsTest.rc"
)
target_link_libraries(companion_ui_settings_view_model PRIVATE
  companion_ui
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_ui_settings_view_model)
add_test(NAME ui.settings-view-model COMMAND companion_ui_settings_view_model)

qt_add_executable(companion_ui_shell_view_model
  CompanionShellViewModelTests.cpp
)
target_link_libraries(companion_ui_shell_view_model PRIVATE
  companion_ui
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_ui_shell_view_model)
add_test(NAME ui.shell-view-model COMMAND companion_ui_shell_view_model)

qt_add_executable(companion_ui_window_backdrop_state
  WindowBackdropStateTests.cpp
)
target_link_libraries(companion_ui_window_backdrop_state PRIVATE
  companion_ui
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_ui_window_backdrop_state)
add_test(
  NAME ui.window-backdrop-state
  COMMAND companion_ui_window_backdrop_state
)

qt_add_executable(companion_ui_shell_qml
  ShellQmlTestRunner.cpp
)
target_link_libraries(companion_ui_shell_qml PRIVATE
  companion_ui
  Qt6::QuickTest
)
companion_enable_warnings(companion_ui_shell_qml)
add_test(
  NAME ui.shell
  COMMAND companion_ui_shell_qml
    -input "${CMAKE_CURRENT_SOURCE_DIR}/tst_companion_shell.qml"
    -import "${PROJECT_BINARY_DIR}/src/ui"
    -o -,txt
)

qt_add_executable(companion_ui_pet_view_model
  PetViewModelTests.cpp
)
target_link_libraries(companion_ui_pet_view_model PRIVATE
  companion_ui
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_ui_pet_view_model)
add_test(NAME ui.pet-view-model COMMAND companion_ui_pet_view_model)

qt_add_executable(companion_ui_pet_catalog
  PetCatalogTests.cpp
)
target_link_libraries(companion_ui_pet_catalog PRIVATE
  companion_ui
  Qt6::Core
  Qt6::Gui
  Qt6::Test
)
companion_enable_warnings(companion_ui_pet_catalog)
add_test(NAME ui.pet-catalog COMMAND companion_ui_pet_catalog)

qt_add_executable(companion_ui_pet_roaming_planner
  PetRoamingPlannerTests.cpp
)
target_link_libraries(companion_ui_pet_roaming_planner PRIVATE
  companion_ui
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_ui_pet_roaming_planner)
add_test(
  NAME ui.pet-roaming-planner
  COMMAND companion_ui_pet_roaming_planner
)

qt_add_executable(companion_ui_pet_menu_placement
  PetMenuPlacementTests.cpp
)
target_link_libraries(companion_ui_pet_menu_placement PRIVATE
  companion_ui
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_ui_pet_menu_placement)
add_test(
  NAME ui.pet-menu-placement
  COMMAND companion_ui_pet_menu_placement
)

qt_add_executable(companion_ui_pet_window_controller
  PetWindowControllerTests.cpp
)
target_link_libraries(companion_ui_pet_window_controller PRIVATE
  companion_ui
  Qt6::Core
  Qt6::Gui
  Qt6::Quick
  Qt6::Test
)
companion_enable_warnings(companion_ui_pet_window_controller)
add_test(
  NAME ui.pet-window-controller
  COMMAND companion_ui_pet_window_controller
)

qt_add_executable(companion_ui_pet_window_qml
  PetWindowQmlTestRunner.cpp
)
target_link_libraries(companion_ui_pet_window_qml PRIVATE
  companion_ui
  Qt6::QuickTest
)
companion_enable_warnings(companion_ui_pet_window_qml)
add_test(
  NAME ui.pet-window
  COMMAND companion_ui_pet_window_qml
    -input "${CMAKE_CURRENT_SOURCE_DIR}/tst_pet_window.qml"
    -import "${PROJECT_BINARY_DIR}/src/ui"
    -o -,txt
)

qt_add_executable(companion_ui_attention_window_qml
  AttentionWindowQmlTestRunner.cpp
)
target_link_libraries(companion_ui_attention_window_qml PRIVATE
  companion_ui
  Qt6::QuickTest
)
companion_enable_warnings(companion_ui_attention_window_qml)
add_test(
  NAME ui.attention-window
  COMMAND companion_ui_attention_window_qml
    -input "${CMAKE_CURRENT_SOURCE_DIR}/tst_attention_window.qml"
    -import "${PROJECT_BINARY_DIR}/src/ui"
    -o -,txt
)

add_test(
  NAME ui.interaction-audit
  COMMAND
    "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe"
    -NoProfile
    -NonInteractive
    -ExecutionPolicy Bypass
    -File
      "${CMAKE_CURRENT_SOURCE_DIR}/VerifyQmlInteractionIds.ps1"
    -QmlRoot
      "${PROJECT_SOURCE_DIR}/src/ui/qml"
)

set_tests_properties(
  ui.settings
  ui.shell
  ui.pet-window-controller
  ui.pet-window
  ui.attention-window
  PROPERTIES
    RESOURCE_LOCK companion_desktop_ui
)
