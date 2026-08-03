add_library(companion_updater_contract STATIC
  UpdateInstallResult.cpp
  UpdateInstallResult.h
  UpdateInstallRequest.cpp
  UpdateInstallRequest.h
  UpdateInstallTransaction.cpp
  UpdateInstallTransaction.h
  UpdateInstallerHandoff.cpp
  UpdateInstallerHandoff.h
)
target_include_directories(companion_updater_contract PUBLIC
  "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(companion_updater_contract PUBLIC
  companion_core
  companion_update
  Qt6::Core
  Shell32
)
companion_enable_warnings(companion_updater_contract)

qt_add_executable(
  CodexCompanionUpdater
  WIN32
  main.cpp
  "${PROJECT_SOURCE_DIR}/resources/app.manifest"
)
target_link_libraries(
  CodexCompanionUpdater
  PRIVATE
    companion_updater_contract
    Qt6::Core
    Advapi32
    Shell32
    Version
)
companion_enable_warnings(
  CodexCompanionUpdater
)

install(
  TARGETS CodexCompanionUpdater
  RUNTIME DESTINATION
    "${CMAKE_INSTALL_BINDIR}"
  COMPONENT CompanionRuntime
)
