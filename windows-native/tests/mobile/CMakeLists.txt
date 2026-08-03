qt_add_executable(companion_mobile_bridge_profile
  BridgeWireProfileTests.cpp
)
target_link_libraries(companion_mobile_bridge_profile PRIVATE
  companion_mobile
  Qt6::Core
  Qt6::Test
)
target_compile_definitions(companion_mobile_bridge_profile PRIVATE
  COMPANION_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tests/fixtures"
)
companion_enable_warnings(companion_mobile_bridge_profile)
add_test(
  NAME mobile.bridge-profile
  COMMAND companion_mobile_bridge_profile
)

qt_add_executable(companion_mobile_presence_pet_catalog
  MobilePresencePetCatalogServiceTests.cpp
)
target_link_libraries(companion_mobile_presence_pet_catalog PRIVATE
  companion_mobile
  Qt6::Core
  Qt6::Gui
  Qt6::Test
)
target_compile_definitions(companion_mobile_presence_pet_catalog PRIVATE
  COMPANION_MOBILE_PRESENCE_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tests/fixtures/mobile-presence"
)
companion_enable_warnings(companion_mobile_presence_pet_catalog)
add_test(
  NAME mobile.presence-pet-catalog
  COMMAND companion_mobile_presence_pet_catalog
)

qt_add_executable(companion_mobile_security
  BridgeSecurityTests.cpp
)
target_link_libraries(companion_mobile_security PRIVATE
  companion_mobile
  Qt6::Core
  Qt6::Test
)
target_compile_definitions(companion_mobile_security PRIVATE
  COMPANION_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tests/fixtures"
)
companion_enable_warnings(companion_mobile_security)
add_test(
  NAME mobile.security
  COMMAND companion_mobile_security
)

qt_add_executable(companion_mobile_pairing
  PairingCoordinatorTests.cpp
)
target_link_libraries(companion_mobile_pairing PRIVATE
  companion_mobile
  companion_windows_security
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_mobile_pairing)
add_test(
  NAME mobile.pairing
  COMMAND companion_mobile_pairing
)

qt_add_executable(companion_mobile_relay_state
  RelayStateStoreTests.cpp
)
target_link_libraries(companion_mobile_relay_state PRIVATE
  companion_mobile
  companion_windows_security
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_mobile_relay_state)
add_test(
  NAME mobile.relay-state
  COMMAND companion_mobile_relay_state
)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/RelaySettingsTest.rc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/RelaySettingsTest.rc"
  @ONLY
)

qt_add_executable(companion_mobile_relay_models
  RelayModelsTests.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/RelaySettingsTest.rc"
)
target_link_libraries(companion_mobile_relay_models PRIVATE
  companion_mobile
  Qt6::Core
  Qt6::Test
  Version
)
target_compile_definitions(companion_mobile_relay_models PRIVATE
  COMPANION_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tests/fixtures"
)
companion_enable_warnings(companion_mobile_relay_models)
add_test(
  NAME mobile.relay-models
  COMMAND companion_mobile_relay_models
)

qt_add_executable(companion_mobile_relay_connection
  RelayConnectionTests.cpp
)
target_link_libraries(companion_mobile_relay_connection PRIVATE
  companion_mobile
  Qt6::Core
  Qt6::Network
  Qt6::Test
  Qt6::WebSockets
)
companion_enable_warnings(companion_mobile_relay_connection)
set(companion_mobile_relay_connection_cases
  lifecycleRegistersFirstAndUsesOneChannelQuery
  sendResolvesAcceptedUndeliverableAndTimeoutExactlyOnce
  receivesPacketsAnswersPingAndRejectsInvalidWire
  bootstrapUnknownSenderDoesNotWeakenPairedEndpoints
  transportPingStartsAfterRegistrationAcknowledgement
  oversizedPacketFailsBeforeTransport
  bareRelayErrorUsesFallbackAndReconnects
  reconnectBackoffMatchesV034AndResetsAfterRegistration
  stopStartCancelsPriorReconnectGeneration
  finalWireSizeBoundaryIsExact
  endpointManagerReusesAndReplacesOnlyChangedRecords
  endpointManagerSendsToTheSelectedPairedDevice
)
foreach(test_case IN LISTS companion_mobile_relay_connection_cases)
  add_test(
    NAME "mobile.relay-connection.${test_case}"
    COMMAND companion_mobile_relay_connection "${test_case}"
  )
  set_tests_properties(
    "mobile.relay-connection.${test_case}"
    PROPERTIES TIMEOUT 10
  )
endforeach()

qt_add_executable(companion_mobile_relay_pairing_bootstrap
  RelayPairingBootstrapTests.cpp
)
target_link_libraries(companion_mobile_relay_pairing_bootstrap PRIVATE
  companion_mobile
  companion_windows_security
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_mobile_relay_pairing_bootstrap)
add_test(
  NAME mobile.relay-pairing-bootstrap
  COMMAND companion_mobile_relay_pairing_bootstrap
)

qt_add_executable(companion_mobile_attachments
  MobileAttachmentTests.cpp
)
target_link_libraries(companion_mobile_attachments PRIVATE
  companion_mobile
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_mobile_attachments)
add_test(
  NAME mobile.attachment
  COMMAND companion_mobile_attachments
)

qt_add_executable(companion_mobile_history
  MobileHistoryCoordinatorTests.cpp
)
target_link_libraries(companion_mobile_history PRIVATE
  companion_mobile
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_mobile_history)
add_test(
  NAME mobile.history
  COMMAND companion_mobile_history
)

qt_add_executable(companion_mobile_nearby_frames
  NearbyFrameCodecTests.cpp
)
target_link_libraries(companion_mobile_nearby_frames PRIVATE
  companion_mobile
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_mobile_nearby_frames)
add_test(
  NAME mobile.nearby-frames
  COMMAND companion_mobile_nearby_frames
)

qt_add_executable(companion_mobile_nearby_server
  NearbyWebSocketServerTests.cpp
)
target_link_libraries(companion_mobile_nearby_server PRIVATE
  companion_mobile
  Qt6::Core
  Qt6::Network
  Qt6::Test
  Qt6::WebSockets
)
target_compile_definitions(companion_mobile_nearby_server PRIVATE
  COMPANION_QTWEBSOCKET_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/.deps/source/qtwebsockets/tests/auto/websockets/shared"
)
companion_enable_warnings(companion_mobile_nearby_server)
add_test(
  NAME mobile.nearby-server
  COMMAND companion_mobile_nearby_server
)

qt_add_executable(companion_mobile_request_dispatcher
  MobileRequestDispatcherTests.cpp
)
target_link_libraries(companion_mobile_request_dispatcher PRIVATE
  companion_mobile
  Qt6::Core
  Qt6::Test
)
target_compile_definitions(companion_mobile_request_dispatcher PRIVATE
  COMPANION_MOBILE_PRESENCE_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tests/fixtures/mobile-presence"
)
companion_enable_warnings(companion_mobile_request_dispatcher)
add_test(
  NAME mobile.request-dispatcher
  COMMAND companion_mobile_request_dispatcher
)
