# ethercat-sim — top-level build.
# Software EtherCAT slave-stack simulator. See docs/ for intent (start with HANDOVER.md).
cmake_minimum_required(VERSION 4.1)

project(ethercat_sim
  VERSION 0.0.1
  DESCRIPTION "Software EtherCAT slave-stack simulator for TwinCAT 3 over a real cable"
  LANGUAGES CXX)

# --- Hard requirements -------------------------------------------------------

# The slave core links the Npcap x64 import libraries (Lib/x64) and binds a NIC
# raw. A 32-bit build would silently mis-link, so fail loudly instead.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR
    "ethercat-sim must be built as x64 (64-bit). Your current toolchain targets "
    "${CMAKE_SIZEOF_VOID_P}-byte pointers.\n"
    "Open an *x64* environment first, e.g.:\n"
    "  Launch-VsDevShell.ps1 -Arch amd64 -HostArch amd64\n"
    "or use the \"x64 Native Tools Command Prompt for VS 2026\".")
endif()

# C++20 everywhere, no compiler extensions (CLAUDE.md: /std:c++20 /permissive-).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json for editor tooling / code intelligence (CLAUDE.md).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to a Debug build if the user didn't pick one (single-config generators).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Warnings-as-rigour per CLAUDE.md code style.
if(MSVC)
  add_compile_options(/W4 /permissive-)
endif()

# --- Npcap SDK ---------------------------------------------------------------

# Override at configure time with -DNPCAP_SDK_DIR=<path> if installed elsewhere.
set(NPCAP_SDK_DIR "C:/dev/sdk/npcap-sdk" CACHE PATH "Path to the Npcap SDK root")

if(NOT EXISTS "${NPCAP_SDK_DIR}/Include/pcap.h")
  message(FATAL_ERROR
    "Npcap SDK not found under NPCAP_SDK_DIR='${NPCAP_SDK_DIR}' "
    "(expected '${NPCAP_SDK_DIR}/Include/pcap.h').\n"
    "Download the SDK from https://npcap.com/#download and either unzip it to "
    "C:/dev/sdk/npcap-sdk or pass -DNPCAP_SDK_DIR=<path>.")
endif()

# Imported target so consumers just `target_link_libraries(... npcap::npcap)`.
add_library(npcap::npcap INTERFACE IMPORTED)
target_include_directories(npcap::npcap INTERFACE "${NPCAP_SDK_DIR}/Include")
target_link_directories(npcap::npcap INTERFACE "${NPCAP_SDK_DIR}/Lib/x64")
target_link_libraries(npcap::npcap INTERFACE wpcap Packet)

# --- Tree --------------------------------------------------------------------

enable_testing()
add_subdirectory(sim/ec-core)
add_subdirectory(test/unit)
