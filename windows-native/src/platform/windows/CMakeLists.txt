add_library(companion_windows_security STATIC
  security/WindowsCrypto.h
  security/WindowsCrypto.cpp
  security/WindowsDpapiProtector.h
  security/WindowsDpapiProtector.cpp
)
target_include_directories(companion_windows_security PUBLIC
  "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(companion_windows_security PUBLIC
  companion_core
  Qt6::Core
  Bcrypt
  Crypt32
)
companion_enable_warnings(companion_windows_security)

add_library(companion_windows STATIC
  BackdropController.h
  BackdropController.cpp
  ChatGPTAccentThemeReader.h
  ChatGPTAccentThemeReader.cpp
  DpapiCredentialStore.h
  DpapiCredentialStore.cpp
  mobile/WindowsDnsSdAdvertiser.h
  mobile/WindowsDnsSdAdvertiser.cpp
  mobile/WindowsNetworkProfileMonitor.h
  mobile/WindowsNetworkProfileMonitor.cpp
  mobile/WindowsPowerAvailabilityController.h
  mobile/WindowsPowerAvailabilityController.cpp
  mobile/WindowsTlsIdentityStore.h
  mobile/WindowsTlsIdentityStore.cpp
  NativeWindowApi.h
  NativeWindowApi.cpp
  SingleInstanceGate.h
  SingleInstanceGate.cpp
  TrayIconHost.h
  TrayIconHost.cpp
  TrayWindowPlacement.h
  TrayWindowPlacement.cpp
  UtilityWindowPolicy.h
  UtilityWindowPolicy.cpp
  WindowRegionPolicy.h
  WindowRegionPolicy.cpp
  WindowCoordinator.h
  WindowCoordinator.cpp
)
target_include_directories(companion_windows PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(companion_windows PUBLIC
  companion_core
  Qt6::Core
  Qt6::Gui
  Qt6::Network
  Qt6::Quick
  Advapi32
  Bcrypt
  Crypt32
  Dnsapi
  Dwmapi
  Gdi32
  Ncrypt
  Ole32
  Shell32
  User32
)
companion_enable_warnings(companion_windows)
