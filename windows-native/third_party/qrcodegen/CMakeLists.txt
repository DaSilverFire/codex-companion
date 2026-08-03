# Project Nayuki QR Code generator, pinned at
# 2c9044de6b049ca25cb3cd1649ed7e27aa055138.
add_library(companion_qrcodegen STATIC
  qrcodegen.cpp
  qrcodegen.hpp
)

target_include_directories(companion_qrcodegen SYSTEM PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_features(companion_qrcodegen PUBLIC cxx_std_17)

install(
  FILES LICENSE
  DESTINATION "licenses/qrcodegen"
  COMPONENT CompanionRuntime
)
