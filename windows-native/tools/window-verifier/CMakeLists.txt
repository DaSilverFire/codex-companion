qt_add_executable(companion-window-verifier
  main.cpp
)
target_link_libraries(companion-window-verifier PRIVATE
  companion_windows_api_floor
  companion_windows
  Qt6::Core
  Qt6::Gui
  Dwmapi
)
companion_enable_warnings(companion-window-verifier)
