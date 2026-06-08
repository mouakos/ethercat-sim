// EtherCAT station behaviour (L2, write side) — what our emulated slaves do
// with an incoming frame. Stage 2 implements only the first step of the state
// machine: make the master detect us.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ecat {

// Application-layer state, register 0x0130 low nibble (ETG.1000.6).
enum class AlState : std::uint8_t {
    kInit = 1,
    kPreOp = 2,
    kBootstrap = 3,
    kSafeOp = 4,
    kOp = 8,
};

inline constexpr std::uint16_t kAlStatusRegister = 0x0130;

// We present four EtherCAT Slave Controllers: the EK1100 coupler, the EL1008
// digital-input terminal, the EL2008 digital-output terminal, and the EL3001
// analog-input terminal (the first with a CoE mailbox). A broadcast read's
// working counter therefore increments by four.
//
// The EL6224 IO-Link master (slave 4) is implemented (kEl6224Profile /
// populate_el6224_od / its unit test) but **parked** at count 4: it needs AoE
// (ADS-over-EtherCAT) + master-configurable PDOs before it can reach OP, and its
// failed init was disrupting the bus (collaterally breaking the EL3001). Bump
// this back to 5 to re-enable once AoE support lands. See HANDOVER §3 (2026-06-07).
inline constexpr std::uint16_t kEmulatedSlaveCount = 4;

// Locally-administered source MAC stamped onto our replies. TwinCAT matches
// responses by EtherType + datagram index, not source MAC, so this is safe —
// and it lets us recognise (and skip) our own transmissions when the capture
// handle also delivers outbound frames (Npcap can't always restrict to inbound).
inline constexpr std::array<std::uint8_t, 6> kSlaveMac = {0x02, 0xEC, 0xCA,
                                                          0x00, 0x00, 0x01};

// Overwrite the Ethernet source MAC (bytes 6..11) with kSlaveMac.
void stamp_source_mac(std::span<std::byte> frame) noexcept;

// True if the frame's source MAC is kSlaveMac, i.e. it is one of our replies.
[[nodiscard]] bool is_own_transmission(std::span<const std::byte> frame) noexcept;

}  // namespace ecat
