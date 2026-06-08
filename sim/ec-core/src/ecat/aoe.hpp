// AoE (ADS over EtherCAT) mailbox service — ETG.1020.
//
// The IO-Link master terminals (EL6224) carry their parameter/ISDU channel over
// AoE: ADS (Automation Device Specification) telegrams tunnelled in the EtherCAT
// mailbox (mailbox header type 1). During bring-up the master assigns the slave
// its AoE/AMS NetId via an ADS Write (TwinCAT's "AoE Init Cmd (download NetId)");
// without a reply the master times out and the slave never leaves PreOp. This
// module turns an AoE request mailbox buffer into an AoE response buffer; the
// mailbox SyncManager handshake (SM0/SM1, status, counter echo) lives in
// chain.cpp, shared with CoE.
//
// Scope (phase 1): acknowledge ADS Read / Write / ReadWrite with success — enough
// for the NetId download to complete so the EL6224 can reach OP. Real ISDU
// parameter access (reading/writing IO-Link device parameters via ADS) is a later
// step that fills in actual response data.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ecat::aoe {

// Mailbox header type value for AoE (ETG.1000.5 / ETG.1020): byte 5 low nibble.
inline constexpr std::uint8_t kMailboxType = 1;

// Service one AoE mailbox request. `request` is the full mailbox frame (6-byte
// mailbox header + 32-byte AMS header + ADS data) exactly as the master wrote it.
// Writes the full response mailbox frame into `out` and returns its length, or 0
// if the request is not an AoE request we service (the caller then stages no
// reply). The mailbox sequence counter in the response header is left 0 for the
// caller to patch (it echoes the request counter, same as the CoE path).
[[nodiscard]] std::size_t process_mailbox(std::span<const std::byte> request,
                                          std::span<std::byte> out) noexcept;

}  // namespace ecat::aoe
