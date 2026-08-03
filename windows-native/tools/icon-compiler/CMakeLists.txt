add_executable(companion_icon_compiler
  main.cpp
)
target_link_libraries(companion_icon_compiler PRIVATE
  Qt6::Core
  Qt6::Gui
)
companion_enable_warnings(companion_icon_compiler)

set(COMPANION_GENERATED_ROOT
    "${PROJECT_SOURCE_DIR}/../work/generated"
    CACHE INTERNAL "Generated Companion resources")
set(COMPANION_GENERATED_ICON
    "${COMPANION_GENERATED_ROOT}/codex-companion.ico"
    CACHE INTERNAL "Generated Companion icon")

add_custom_command(
  OUTPUT "${COMPANION_GENERATED_ICON}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${COMPANION_GENERATED_ROOT}"
  COMMAND companion_icon_compiler
          "${PROJECT_SOURCE_DIR}/../Assets/AppIcon/codex-companion-icon.png"
          "${COMPANION_GENERATED_ICON}"
  DEPENDS companion_icon_compiler
          "${PROJECT_SOURCE_DIR}/../Assets/AppIcon/codex-companion-icon.png"
  VERBATIM
)

add_custom_target(companion_generated_icon
  DEPENDS "${COMPANION_GENERATED_ICON}")
