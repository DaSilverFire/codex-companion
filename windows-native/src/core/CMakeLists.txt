add_library(companion_core STATIC
  AppSettings.h
  ChatCredentialService.cpp
  ChatCredentialService.h
  CompanionCommandBus.cpp
  CompanionCommandBus.h
  CompanionError.h
  CompanionInstallationIdentityStore.cpp
  CompanionInstallationIdentityStore.h
  CredentialStore.h
  Result.h
  SettingsRepository.cpp
  SettingsRepository.h
)
target_include_directories(companion_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(companion_core PUBLIC
  companion_windows_api_floor
  Qt6::Core
)
companion_enable_warnings(companion_core)
