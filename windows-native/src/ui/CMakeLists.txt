set_source_files_properties(
  qml/theme/CompanionTheme.qml
  PROPERTIES QT_QML_SINGLETON_TYPE TRUE
)

add_library(companion_ui STATIC)
target_sources(companion_ui PRIVATE
  CompanionPresentationPolicy.cpp
  CompanionPresentationPolicy.h
  CompanionShellViewModel.cpp
  CompanionShellViewModel.h
  PetViewModel.cpp
  PetViewModel.h
  PetWindowController.cpp
  PetWindowController.h
  PairingQrCode.cpp
  PairingQrCode.h
  SettingsViewModel.cpp
  SettingsViewModel.h
  WindowBackdropState.cpp
  WindowBackdropState.h
  pet/PetAnimation.cpp
  pet/PetAnimation.h
  pet/PetCatalog.cpp
  pet/PetCatalog.h
  pet/PetDefinition.cpp
  pet/PetDefinition.h
  pet/PetDirectionalLook.cpp
  pet/PetDirectionalLook.h
  pet/PetMenuPlacement.cpp
  pet/PetMenuPlacement.h
  pet/PetRoamingPlanner.cpp
  pet/PetRoamingPlanner.h
)
target_include_directories(companion_ui PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(companion_ui PUBLIC
  companion_codex
  companion_core
  companion_mobile
  companion_qrcodegen
  companion_windows
  Qt6::Core
  Qt6::Gui
  Qt6::Qml
  Qt6::Quick
  Qt6::QuickControls2
)

qt_add_qml_module(companion_ui
  URI CodexCompanion
  VERSION 1.0
  OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/CodexCompanion"
  NO_CACHEGEN
  NO_PLUGIN
  QML_FILES
    qml/Main.qml
    qml/AttentionWindow.qml
    qml/CompanionMenuWindow.qml
    qml/GoalConfettiOverlay.qml
    qml/PetWindow.qml
    qml/SettingsWindow.qml
    qml/UsageWindow.qml
    qml/pet/PetSpriteSheet.qml
    qml/settings/AccountsSettingsPage.qml
    qml/settings/ChatSettingsPage.qml
    qml/settings/GeneralSettingsPage.qml
    qml/settings/MobileSettingsPage.qml
    qml/settings/UpdatesSettingsPage.qml
    qml/theme/CompanionTheme.qml
)

install(
  DIRECTORY
    "${PROJECT_SOURCE_DIR}/../Skills/companion-pet/"
  DESTINATION
    "resources/skills/companion-pet"
  COMPONENT CompanionRuntime
  PATTERN "tests" EXCLUDE
  PATTERN "__pycache__" EXCLUDE
  PATTERN "*.pyc" EXCLUDE
)

companion_enable_warnings(companion_ui)
