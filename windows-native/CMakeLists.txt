cmake_minimum_required(VERSION 4.0)
project(CodexCompanionWindows VERSION 0.3.5 LANGUAGES C CXX RC)

include(GNUInstallDirs)

set(
  CompanionRelayURL
  "wss://codex-companion-relay.silverfire-codex-companion.workers.dev/relay"
  CACHE STRING
  "Bundled secure relay endpoint exposed as CompanionRelayURL"
)
set(
  CODEX_COMPANION_WINDOWS_SIGNER_SHA256
  ""
  CACHE STRING
  "Comma-separated SHA-256 thumbprints allowed to sign Windows updates"
)
set(
  CODEX_COMPANION_WINDOWS_UPDATE_MANIFEST_URL
  ""
  CACHE STRING
  "HTTPS manifest URL for the signed Windows update channel"
)
set(
  CODEX_COMPANION_WINDOWS_UPDATE_PUBLIC_KEY
  ""
  CACHE STRING
  "Base64 Ed25519 public key for the signed Windows update channel"
)
set(
  CODEX_COMPANION_WINDOWS_BUILD
  "1"
  CACHE STRING
  "Monotonic Windows release build number"
)
option(
  CODEX_COMPANION_PUBLIC_RELEASE
  "Require complete public Windows update-channel configuration"
  OFF
)
if(
  NOT CODEX_COMPANION_WINDOWS_SIGNER_SHA256 STREQUAL ""
  AND NOT CODEX_COMPANION_WINDOWS_SIGNER_SHA256 MATCHES
    "^[0-9A-Fa-f]{64}(,[0-9A-Fa-f]{64})*$"
)
  message(FATAL_ERROR
    "CODEX_COMPANION_WINDOWS_SIGNER_SHA256 must be empty or a "
    "comma-separated list of 64-digit hexadecimal thumbprints."
  )
endif()
if(
  NOT CODEX_COMPANION_WINDOWS_BUILD MATCHES
    "^[1-9][0-9]*$"
)
  message(FATAL_ERROR
    "CODEX_COMPANION_WINDOWS_BUILD must be a positive integer."
  )
endif()
if(CODEX_COMPANION_WINDOWS_BUILD GREATER 65535)
  message(FATAL_ERROR
    "CODEX_COMPANION_WINDOWS_BUILD must not exceed 65535."
  )
endif()
if(
  NOT CODEX_COMPANION_WINDOWS_UPDATE_MANIFEST_URL STREQUAL ""
  AND NOT CODEX_COMPANION_WINDOWS_UPDATE_MANIFEST_URL MATCHES
    "^https://"
)
  message(FATAL_ERROR
    "CODEX_COMPANION_WINDOWS_UPDATE_MANIFEST_URL must use HTTPS."
  )
endif()
foreach(
  COMPANION_UPDATE_TEXT_VALUE
  IN ITEMS
    CODEX_COMPANION_WINDOWS_UPDATE_MANIFEST_URL
    CODEX_COMPANION_WINDOWS_UPDATE_PUBLIC_KEY
    CODEX_COMPANION_WINDOWS_SIGNER_SHA256
)
  if(
    "${${COMPANION_UPDATE_TEXT_VALUE}}" MATCHES
      "[\"\\\\\r\n]"
  )
    message(FATAL_ERROR
      "${COMPANION_UPDATE_TEXT_VALUE} contains characters that cannot "
      "be embedded in update-channel.json."
    )
  endif()
endforeach()
if(CODEX_COMPANION_PUBLIC_RELEASE)
  foreach(
    COMPANION_REQUIRED_RELEASE_VALUE
    IN ITEMS
      CODEX_COMPANION_WINDOWS_UPDATE_MANIFEST_URL
      CODEX_COMPANION_WINDOWS_UPDATE_PUBLIC_KEY
      CODEX_COMPANION_WINDOWS_SIGNER_SHA256
  )
    if("${${COMPANION_REQUIRED_RELEASE_VALUE}}" STREQUAL "")
      message(FATAL_ERROR
        "${COMPANION_REQUIRED_RELEASE_VALUE} is required for a public release."
      )
    endif()
  endforeach()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  set(
    CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION
    "${CMAKE_INSTALL_BINDIR}"
  )
  set(
    CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT
    CompanionRuntime
  )
  include(InstallRequiredSystemLibraries)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

find_package(Qt6 6.11.1 QUIET REQUIRED COMPONENTS
  Core Gui Qml Quick QuickControls2 Network WebSockets Sql Test QuickTest
)

qt_standard_project_setup(REQUIRES 6.11.1)
enable_testing()

add_library(companion_windows_api_floor INTERFACE)
if(MSVC)
  target_compile_definitions(companion_windows_api_floor INTERFACE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A00000B
  )
endif()

add_subdirectory(third_party/foundry-local)
add_subdirectory(third_party/qrcodegen)
add_subdirectory(tools)
add_subdirectory(src)
add_subdirectory(tests)
