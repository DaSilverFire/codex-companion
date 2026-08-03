qt_add_executable(companion-mobile-probe
  main.cpp
)
target_link_libraries(companion-mobile-probe PRIVATE
  companion_windows_api_floor
  companion_app
  Qt6::Core
  Qt6::Network
  Qt6::WebSockets
)
target_compile_definitions(companion-mobile-probe PRIVATE
  COMPANION_QTWEBSOCKET_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/.deps/source/qtwebsockets/tests/auto/websockets/shared"
)
companion_enable_warnings(companion-mobile-probe)

add_test(
  NAME mobile.probe.loopback
  COMMAND
    companion-mobile-probe
    --scenario loopback
    --fixture-root "${PROJECT_SOURCE_DIR}/tests/fixtures/mobile-v034"
    --output "${PROJECT_BINARY_DIR}/evidence/mobile-loopback"
    --device-id "windows-loopback-probe"
    --timeout-seconds 60
)
set_tests_properties(
  mobile.probe.loopback
  PROPERTIES
    TIMEOUT 90
)
