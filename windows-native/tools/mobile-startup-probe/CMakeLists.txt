qt_add_executable(companion-mobile-startup-probe
  main.cpp
  "${PROJECT_BINARY_DIR}/src/app/CodexCompanion.rc"
)
add_dependencies(
  companion-mobile-startup-probe
  companion_generated_icon
)
target_link_libraries(companion-mobile-startup-probe PRIVATE
  companion_windows_api_floor
  companion_app
  Qt6::Core
  Qt6::Network
)
companion_enable_warnings(companion-mobile-startup-probe)
