cmake_path(
  NORMAL_PATH COMPANION_GENERATED_ICON
  OUTPUT_VARIABLE COMPANION_GENERATED_ICON_NORMALIZED
)
file(
  TO_NATIVE_PATH
  "${COMPANION_GENERATED_ICON_NORMALIZED}"
  COMPANION_ICON_PATH
)
string(REPLACE "\\" "\\\\" COMPANION_ICON_PATH "${COMPANION_ICON_PATH}")
configure_file(
  "${PROJECT_SOURCE_DIR}/resources/CodexCompanion.rc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/CodexCompanion.rc"
  @ONLY
)

add_library(companion_app STATIC
  ChatCredentialAvailability.h
  ChatCredentialAvailability.cpp
  CompanionApplication.h
  CompanionApplication.cpp
  CompanionMobileHost.h
  CompanionMobileHost.cpp
  CompanionMobileRelayRouter.h
  CompanionMobileRelayRouter.cpp
  CompanionMobileRuntimeAdapter.h
  CompanionMobileRuntimeAdapter.cpp
  PetProcessReactionController.h
  PetProcessReactionController.cpp
  CompanionRuntimeHost.h
  CompanionRuntimeHost.cpp
  RuntimeHostStatusDispatcher.h
  RuntimeHostStatusDispatcher.cpp
  UpdateViewModel.h
  UpdateViewModel.cpp
)
target_include_directories(companion_app PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(companion_app PUBLIC
  companion_core
  companion_mobile
  companion_state
  companion_ui
  companion_update
  companion_updater_contract
  companion_windows
  Qt6::Core
  Qt6::Gui
  Qt6::Qml
  Qt6::Quick
)
companion_enable_warnings(companion_app)

qt_add_executable(CodexCompanion WIN32
  main.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/CodexCompanion.rc"
  "${PROJECT_SOURCE_DIR}/resources/app.manifest"
)
add_dependencies(CodexCompanion companion_generated_icon)
set_source_files_properties(
  "${CMAKE_CURRENT_BINARY_DIR}/CodexCompanion.rc"
  PROPERTIES
    OBJECT_DEPENDS "${COMPANION_GENERATED_ICON}"
)
target_link_libraries(CodexCompanion PRIVATE
  companion_app
  Qt6::Gui
)
companion_enable_warnings(CodexCompanion)
companion_add_foundry_runtime(CodexCompanion)

install(
  TARGETS CodexCompanion
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  COMPONENT CompanionRuntime
)
install(
  FILES "${PROJECT_SOURCE_DIR}/resources/qt.conf"
  DESTINATION "${CMAKE_INSTALL_BINDIR}"
  COMPONENT CompanionRuntime
)
install(
  FILES "$<TARGET_FILE:Qt6::WebSockets>"
  DESTINATION "${CMAKE_INSTALL_BINDIR}"
  COMPONENT CompanionRuntime
)
set(
  COMPANION_QWEBP_PLUGIN_ROOT
  "${PROJECT_SOURCE_DIR}/.deps/qt-6.11.1/plugins/imageformats"
)
foreach(plugin_name IN ITEMS qwebp.dll qwebpd.dll)
  if(NOT EXISTS "${COMPANION_QWEBP_PLUGIN_ROOT}/${plugin_name}")
    message(FATAL_ERROR
      "The Qt WebP plugin is unavailable: "
      "${COMPANION_QWEBP_PLUGIN_ROOT}/${plugin_name}. "
      "Run tools/bootstrap-dependencies.ps1."
    )
  endif()
endforeach()
install(
  FILES
    "$<IF:$<CONFIG:Debug>,${COMPANION_QWEBP_PLUGIN_ROOT}/qwebpd.dll,${COMPANION_QWEBP_PLUGIN_ROOT}/qwebp.dll>"
  DESTINATION "plugins/imageformats"
  COMPONENT CompanionRuntime
)

get_target_property(
  COMPANION_WINDEPLOYQT_EXECUTABLE
  Qt6::windeployqt
  IMPORTED_LOCATION
)
get_target_property(
  COMPANION_QTPATHS_EXECUTABLE
  Qt6::qtpaths
  IMPORTED_LOCATION
)
if(NOT EXISTS "${COMPANION_WINDEPLOYQT_EXECUTABLE}")
  message(FATAL_ERROR "The Qt deployment tool is unavailable.")
endif()
if(NOT EXISTS "${COMPANION_QTPATHS_EXECUTABLE}")
  message(FATAL_ERROR "The Qt path query tool is unavailable.")
endif()
set(
  COMPANION_QT_DEPLOY_PROBE_EXECUTABLE
  "${PROJECT_BINARY_DIR}/companion_icon_compiler.exe"
)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(COMPANION_QT_DEPLOY_MODE "--debug")
else()
  set(COMPANION_QT_DEPLOY_MODE "--release")
endif()
configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/DeployCompanionQml.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/DeployCompanionQml.cmake"
  @ONLY
)
install(
  SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/DeployCompanionQml.cmake"
  COMPONENT CompanionRuntime
)

set(
  COMPANION_STAGE_DIR
  "${PROJECT_BINARY_DIR}/stage/$<CONFIG>"
)
add_custom_target(companion_stage
  COMMAND
    "${CMAKE_COMMAND}"
    "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
    "-DSTAGE_DIR=${COMPANION_STAGE_DIR}"
    "-DCONFIG=$<CONFIG>"
    -P "${PROJECT_SOURCE_DIR}/cmake/StageCompanion.cmake"
  DEPENDS
    CodexCompanion
    CodexCompanionUpdater
    companion_icon_compiler
  VERBATIM
)

set(
  COMPANION_DEPLOYED_EXECUTABLE
  "${COMPANION_STAGE_DIR}/${CMAKE_INSTALL_BINDIR}/CodexCompanion.exe"
)
add_custom_target(companion_stage_smoke
  COMMAND
    "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe"
    -NoProfile
    -ExecutionPolicy Bypass
    -File "${PROJECT_SOURCE_DIR}/tools/verify-deployed-app.ps1"
    -ExecutablePath "${COMPANION_DEPLOYED_EXECUTABLE}"
  DEPENDS companion_stage
  VERBATIM
)
