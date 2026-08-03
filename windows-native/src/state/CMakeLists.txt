add_library(companion_state STATIC
  ../core/CompanionState.h
  ../core/CompanionState.cpp
  ../codex/runtime/CodexRuntime.h
  ../codex/runtime/CodexRuntime.cpp
  ../codex/runtime/CodexRuntimeCommands.cpp
  ../codex/runtime/CodexRuntimeCommandState.h
  ../codex/runtime/CodexRuntimeDependencyFactory.h
  ../codex/runtime/CodexRuntimeDependencyFactory.cpp
  ../codex/runtime/CodexRuntimeOperations.h
  ../codex/runtime/CodexRuntimeHistory.cpp
  ../codex/runtime/CodexRuntimeReads.cpp
  ../codex/runtime/CodexRuntimeLifetime.h
  ../codex/runtime/CodexRuntimeLifetime.cpp
  ../codex/runtime/CodexRuntimeOperationRegistry.h
  ../codex/runtime/CodexRuntimeOperationRegistry.cpp
  ../codex/runtime/CodexRuntimeOperationState.h
  ../codex/runtime/CodexRuntimeOperationState.cpp
  ../codex/runtime/RuntimeContinuationHost.h
  ../codex/runtime/RuntimeContinuationHost.cpp
)
target_include_directories(companion_state PUBLIC
  "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(companion_state PUBLIC
  companion_core
  companion_codex
  Qt6::Core
)
companion_enable_warnings(companion_state)
