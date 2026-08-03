qt_add_executable(companion_codex_bridge_json
  BridgeJsonCodecTests.cpp
)
target_link_libraries(companion_codex_bridge_json PRIVATE
  companion_codex
  Qt6::Core
  Qt6::Test
)
target_compile_definitions(companion_codex_bridge_json PRIVATE
  COMPANION_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tests/fixtures"
)
companion_enable_warnings(companion_codex_bridge_json)
add_test(NAME codex.bridge-json COMMAND companion_codex_bridge_json)

qt_add_executable(companion_codex_discovery
  CodexDiscoveryTests.cpp
)
target_link_libraries(companion_codex_discovery PRIVATE
  companion_codex
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_codex_discovery)
add_test(NAME codex.discovery COMMAND companion_codex_discovery)

qt_add_executable(
  companion_codex_account_profiles
  CodexAccountProfileTests.cpp
)
target_link_libraries(
  companion_codex_account_profiles
  PRIVATE
    companion_codex
    Qt6::Core
    Qt6::Test
)
companion_enable_warnings(
  companion_codex_account_profiles
)
add_test(
  NAME codex.account-profiles
  COMMAND companion_codex_account_profiles
)

qt_add_executable(
  companion_codex_continuation
  CodexContinuationTests.cpp
)
target_link_libraries(
  companion_codex_continuation
  PRIVATE
    companion_codex
    Qt6::Core
    Qt6::Test
)
companion_enable_warnings(
  companion_codex_continuation
)
add_test(
  NAME codex.continuation
  COMMAND companion_codex_continuation
)

qt_add_executable(companion_codex_state_reader
  StateReaderTests.cpp
)
target_link_libraries(companion_codex_state_reader PRIVATE
  companion_codex
  Qt6::Core
  Qt6::Sql
  Qt6::Test
)
companion_enable_warnings(companion_codex_state_reader)
add_test(NAME codex.state-reader COMMAND companion_codex_state_reader)

qt_add_executable(companion_codex_desktop_approval_state
  DesktopApprovalStateTests.cpp
)
target_link_libraries(companion_codex_desktop_approval_state PRIVATE
  companion_codex
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_codex_desktop_approval_state)
add_test(
  NAME codex.desktop-approval-state
  COMMAND companion_codex_desktop_approval_state
)

qt_add_executable(companion_codex_task_projection
  TaskProjectionTests.cpp
)
target_link_libraries(companion_codex_task_projection PRIVATE
  companion_codex
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_codex_task_projection)
add_test(NAME codex.task-projection COMMAND companion_codex_task_projection)

qt_add_executable(companion_codex_tool_projection
  ToolProjectionTests.cpp
)
target_link_libraries(companion_codex_tool_projection PRIVATE
  companion_codex
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_codex_tool_projection)
add_test(NAME codex.tool-projection COMMAND companion_codex_tool_projection)

qt_add_executable(companion_codex_timeline_projection
  TimelineProjectionTests.cpp
)
target_link_libraries(companion_codex_timeline_projection PRIVATE
  companion_codex
  Qt6::Core
  Qt6::Test
)
target_compile_definitions(companion_codex_timeline_projection PRIVATE
  COMPANION_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tests/fixtures"
)
companion_enable_warnings(companion_codex_timeline_projection)
add_test(NAME codex.timeline COMMAND companion_codex_timeline_projection)

qt_add_executable(companion_codex_subagent_reader
  SubagentReaderTests.cpp
)
target_link_libraries(companion_codex_subagent_reader PRIVATE
  companion_codex
  Qt6::Core
  Qt6::Sql
  Qt6::Test
)
companion_enable_warnings(companion_codex_subagent_reader)
add_test(NAME codex.subagent-reader COMMAND companion_codex_subagent_reader)

qt_add_executable(companion_codex_history_coordinator
  HistoryCoordinatorTests.cpp
)
target_link_libraries(companion_codex_history_coordinator PRIVATE
  companion_codex
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_codex_history_coordinator)
add_test(
  NAME codex.history-coordinator
  COMMAND companion_codex_history_coordinator
)

qt_add_executable(companion_codex_attachments
  AttachmentStoreTests.cpp
)
target_link_libraries(companion_codex_attachments PRIVATE
  companion_codex
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_codex_attachments)
add_test(NAME codex.attachments COMMAND companion_codex_attachments)

qt_add_executable(companion_codex_follower
  FollowerProtocolTests.cpp
)
target_link_libraries(companion_codex_follower PRIVATE
  companion_codex
  Qt6::Core
  Qt6::Test
)
target_compile_definitions(companion_codex_follower PRIVATE
  COMPANION_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tests/fixtures"
)
companion_enable_warnings(companion_codex_follower)
add_test(NAME codex.follower COMMAND companion_codex_follower)

qt_add_executable(companion_codex_task_creator
  AppServerTaskCreatorTests.cpp
)
target_link_libraries(companion_codex_task_creator PRIVATE
  companion_codex
  Qt6::Core
  Qt6::Test
)
target_compile_definitions(companion_codex_task_creator PRIVATE
  COMPANION_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tests/fixtures"
)
companion_enable_warnings(companion_codex_task_creator)
add_test(NAME codex.task-creator COMMAND companion_codex_task_creator)

qt_add_executable(companion_codex_chat
  ChatCatalogTests.cpp
)
target_link_libraries(companion_codex_chat PRIVATE
  companion_codex
  companion_windows
  Qt6::Core
  Qt6::Test
)
target_compile_definitions(companion_codex_chat PRIVATE
  COMPANION_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tests/fixtures"
)
companion_enable_warnings(companion_codex_chat)
add_test(NAME codex.chat COMMAND companion_codex_chat)

qt_add_executable(
  companion_codex_portable_chat_tools
  PortableChatToolServicesTests.cpp
)
target_link_libraries(
  companion_codex_portable_chat_tools
  PRIVATE
    companion_codex
    Qt6::Core
    Qt6::Test
)
companion_enable_warnings(
  companion_codex_portable_chat_tools
)
add_test(
  NAME codex.portable-chat-tools
  COMMAND companion_codex_portable_chat_tools
)

qt_add_executable(companion_codex_capabilities
  CapabilityServiceTests.cpp
)
target_link_libraries(companion_codex_capabilities PRIVATE
  companion_codex
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_codex_capabilities)
add_test(
  NAME codex.capabilities
  COMMAND companion_codex_capabilities
)

qt_add_executable(companion_codex_task_snapshot_loader
  TaskSnapshotLoaderTests.cpp
)
target_link_libraries(companion_codex_task_snapshot_loader PRIVATE
  companion_codex
  Qt6::Core
  Qt6::Sql
  Qt6::Test
)
if(MSVC)
  target_link_options(
    companion_codex_task_snapshot_loader
    PRIVATE
      /INCREMENTAL:NO
  )
endif()
companion_enable_warnings(companion_codex_task_snapshot_loader)
add_test(
  NAME codex.task-snapshot-loader
  COMMAND companion_codex_task_snapshot_loader
)

qt_add_executable(
  companion_codex_thread_runtime_status
  ThreadRuntimeStatusReaderTests.cpp
)
target_link_libraries(
  companion_codex_thread_runtime_status
  PRIVATE
    companion_codex
    Qt6::Core
    Qt6::Test
)
companion_enable_warnings(
  companion_codex_thread_runtime_status
)
add_test(
  NAME codex.thread-runtime-status
  COMMAND companion_codex_thread_runtime_status
)

qt_add_executable(companion_codex_commands
  CommandServiceTests.cpp
)
target_link_libraries(companion_codex_commands PRIVATE
  companion_codex
  Qt6::Core
  Qt6::Test
)
target_compile_definitions(companion_codex_commands PRIVATE
  COMPANION_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tests/fixtures"
)
companion_enable_warnings(companion_codex_commands)
add_test(NAME codex.commands COMMAND companion_codex_commands)

qt_add_executable(companion_codex_task_list_model
  TaskListModelTests.cpp
)
target_link_libraries(companion_codex_task_list_model PRIVATE
  companion_state
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_codex_task_list_model)
add_test(
  NAME codex.task-list-model
  COMMAND companion_codex_task_list_model
)

qt_add_executable(companion_codex_process_list_model
  ProcessListModelTests.cpp
)
target_link_libraries(companion_codex_process_list_model PRIVATE
  companion_codex
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_codex_process_list_model)
add_test(
  NAME codex.process-list-model
  COMMAND companion_codex_process_list_model
)

qt_add_executable(companion_codex_runtime
  CodexRuntimeTests.cpp
)
target_link_libraries(companion_codex_runtime PRIVATE
  companion_state
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_codex_runtime)
add_test(
  NAME codex.runtime
  COMMAND companion_codex_runtime
)

qt_add_executable(companion_codex_runtime_operation
  CodexRuntimeOperationTests.cpp
)
target_link_libraries(companion_codex_runtime_operation PRIVATE
  companion_state
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_codex_runtime_operation)
add_test(
  NAME codex.runtime-operation
  COMMAND companion_codex_runtime_operation
)

qt_add_executable(companion_codex_runtime_continuation
  RuntimeContinuationHostTests.cpp
)
target_link_libraries(companion_codex_runtime_continuation PRIVATE
  companion_state
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_codex_runtime_continuation)
add_test(
  NAME codex.runtime-continuation
  COMMAND companion_codex_runtime_continuation
)

qt_add_executable(companion_codex_runtime_lifetime
  CodexRuntimeLifetimeTests.cpp
)
target_link_libraries(companion_codex_runtime_lifetime PRIVATE
  companion_state
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_codex_runtime_lifetime)
add_test(
  NAME codex.runtime-lifetime
  COMMAND companion_codex_runtime_lifetime
)

qt_add_executable(companion_codex_runtime_history
  CodexRuntimeHistoryTests.cpp
)
target_link_libraries(companion_codex_runtime_history PRIVATE
  companion_state
  Qt6::Core
  Qt6::Sql
  Qt6::Test
)
companion_enable_warnings(companion_codex_runtime_history)
add_test(
  NAME codex.runtime-history
  COMMAND companion_codex_runtime_history
)

qt_add_executable(companion_codex_runtime_read
  CodexRuntimeReadTests.cpp
)
target_link_libraries(companion_codex_runtime_read PRIVATE
  companion_state
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_codex_runtime_read)
add_test(
  NAME codex.runtime-read
  COMMAND companion_codex_runtime_read
)

qt_add_executable(companion_codex_runtime_command
  CodexRuntimeCommandTests.cpp
)
target_link_libraries(companion_codex_runtime_command PRIVATE
  companion_state
  Qt6::Core
  Qt6::Test
)
companion_enable_warnings(companion_codex_runtime_command)
add_test(
  NAME codex.runtime-command
  COMMAND companion_codex_runtime_command
)

qt_add_executable(
  companion_codex_runtime_dependency_factory
  CodexRuntimeDependencyFactoryTests.cpp
)
target_link_libraries(
  companion_codex_runtime_dependency_factory
  PRIVATE
    companion_state
    Qt6::Core
    Qt6::Test
)
companion_enable_warnings(
  companion_codex_runtime_dependency_factory
)
add_test(
  NAME codex.runtime-dependency-factory
  COMMAND companion_codex_runtime_dependency_factory
)

qt_add_executable(
  companion_codex_on_device_chat_backend
  WindowsOnDeviceChatBackendTests.cpp
)
target_link_libraries(
  companion_codex_on_device_chat_backend
  PRIVATE
    companion_codex
    Qt6::Core
    Qt6::Test
)
companion_enable_warnings(
  companion_codex_on_device_chat_backend
)
add_test(
  NAME codex.on-device-chat-backend
  COMMAND companion_codex_on_device_chat_backend
)

qt_add_executable(
  companion_codex_foundry_local_driver
  FoundryLocalDriverTests.cpp
)
target_link_libraries(
  companion_codex_foundry_local_driver
  PRIVATE
    companion_codex
    Qt6::Core
    Qt6::Test
)
companion_enable_warnings(
  companion_codex_foundry_local_driver
)
add_test(
  NAME codex.foundry-local-driver
  COMMAND companion_codex_foundry_local_driver
)

qt_add_executable(
  companion_codex_on_device_chat_registry
  WindowsOnDeviceChatBackendRegistryTests.cpp
)
target_link_libraries(
  companion_codex_on_device_chat_registry
  PRIVATE
    companion_codex
    Qt6::Core
    Qt6::Test
)
companion_enable_warnings(
  companion_codex_on_device_chat_registry
)
add_test(
  NAME codex.on-device-chat-registry
  COMMAND companion_codex_on_device_chat_registry
)

qt_add_executable(
  companion_codex_foundry_packaging
  FoundryPackagingTests.cpp
)
add_dependencies(
  companion_codex_foundry_packaging
  CodexCompanion
)
target_link_libraries(
  companion_codex_foundry_packaging
  PRIVATE
    Qt6::Core
    Qt6::Test
)
target_compile_definitions(
  companion_codex_foundry_packaging
  PRIVATE
    COMPANION_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
    COMPANION_PACKAGED_APP_DIR="$<TARGET_FILE_DIR:CodexCompanion>"
)
companion_enable_warnings(
  companion_codex_foundry_packaging
)
add_test(
  NAME codex.foundry-packaging
  COMMAND companion_codex_foundry_packaging
)
