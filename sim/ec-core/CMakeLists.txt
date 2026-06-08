# ec-core — C++ EtherCAT slave stack (L1 wire / L2 ESM-SM-FMMU / L3 mailbox-CoE).
# Owns the NIC via Npcap.

# Pure, NIC-independent EtherCAT logic (parsing today; ESM/SM/FMMU later).
# Kept as a library so the unit tests can link it without pulling in Npcap.
add_library(ec-core-lib STATIC
  src/ecat/frame.cpp
  src/ecat/station.cpp
  src/ecat/chain.cpp
  src/ecat/coe.cpp
  src/ecat/aoe.cpp
  src/ecat/telltale.cpp  # TELLTALE: removable diagnostic instrumentation (grep TELLTALE)
)
target_include_directories(ec-core-lib PUBLIC src)

# The slave executable: NIC access (Npcap) + the capture/dissect CLI.
add_executable(ec-core
  src/main.cpp
  src/net/pcap_source.cpp
  src/net/priority.cpp
)
target_link_libraries(ec-core PRIVATE ec-core-lib npcap::npcap)
