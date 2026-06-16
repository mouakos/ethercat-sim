# sim/vport/bridge — user-mode TAP bridge (no kernel signing required)
# Builds as part of the normal CMake/Ninja build on Windows.
# Requires no external dependencies beyond the Win32 API.

if(NOT WIN32)
  message(STATUS "vport-bridge: skipped (Windows only)")
  return()
endif()

add_executable(vport-bridge vport-bridge.cpp)

target_compile_features(vport-bridge PRIVATE cxx_std_20)

target_link_libraries(vport-bridge PRIVATE
  advapi32   # RegOpenKeyExA / RegQueryValueExA
  kernel32   # CreateFile / DeviceIoControl / overlapped I/O
)

# Place the binary next to ec-core for easy access.
set_target_properties(vport-bridge PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
