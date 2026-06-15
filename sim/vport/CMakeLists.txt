# sim/vport/CMakeLists.txt
#
# The vport kernel driver is built with the Windows Driver Kit (WDK), which
# uses its own MSBuild-based toolchain and cannot share the main CMake/Ninja
# build tree.  This file documents the source set and build steps; the actual
# compilation is done via the PowerShell helper in scripts/Build-VPort.ps1.
#
# Quick-start (from a Developer PowerShell for VS 2026, as Administrator):
#
#   # 1. Enable test-signing (once per machine, requires reboot)
#   bcdedit /set testsigning on
#   Restart-Computer
#
#   # 2. Build the driver
#   .\sim\vport\scripts\Build-VPort.ps1
#
#   # 3. Install the two virtual NICs
#   .\sim\vport\scripts\Install-VPort.ps1 -Action Install
#
# After install:
#   - "EtherCAT vPort Master" appears in Device Manager → TwinCAT → Install RT driver
#   - "EtherCAT vPort Slave"  is visible to Npcap → ec-core --list → --serve <NPF_GUID>

cmake_minimum_required(VERSION 4.1)
project(vport LANGUAGES NONE)   # no C compiler selected here; WDK owns the build

set(VPORT_SOURCES
    vport.h
    vport.c
    vport.inf
)

# Inform IDEs / tooling about the source files without attempting to compile them.
add_custom_target(vport_sources SOURCES ${VPORT_SOURCES})

message(STATUS
    "vport: kernel driver — build via sim/vport/scripts/Build-VPort.ps1 (WDK required)")
