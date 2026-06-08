// A software EtherCAT segment: a fixed chain of emulated Slave Controllers.
//
// A captured frame is made to "traverse" each ESC in order, exactly as it would
// on a real daisy-chained bus — applying EtherCAT addressing (broadcast,
// auto-increment, configured-station) and working-counter rules — so the master
// sees a genuine ring of slaves it can read, write, and address individually.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ecat/coe.hpp"

namespace ecat {

// One emulated EtherCAT Slave Controller: its full 64 KB register space plus a
// cached copy of the configured station address (register 0x0010) and, for
// CoE-capable slaves, a CANopen Object Dictionary serviced over the mailbox.
struct Esc {
    static constexpr std::size_t kRegisterSpace = 0x10000;
    static constexpr std::size_t kEepromWords = 128;  // SII config + identity area

    std::array<std::byte, kRegisterSpace> registers{};
    std::array<std::uint16_t, kEepromWords> eeprom{};  // word-addressed SII image
    std::uint16_t configured_address = 0;
    coe::ObjectDictionary od;  // empty unless this slave supports CoE (e.g. EL3001)
    bool mbx_request_pending = false;  // a mailbox-out request arrived while the previous reply was still unread (single-buffer backpressure); processed when the reply is collected
    std::uint8_t mbx_status_poll = 0xFF;  // diag: last SM1-status byte the master read via LRD
    std::uint8_t mbx_last_req_counter = 0xFF;  // TELLTALE: previous mailbox request counter (retry detection)
    std::uint32_t mbx_read_probe = 0xFFFFFFFF;  // TELLTALE: last logged SM1-buffer read range (ado<<16|len)

    [[nodiscard]] std::uint16_t get_u16(std::uint16_t addr) const noexcept;
    [[nodiscard]] std::uint32_t get_u32(std::uint16_t addr) const noexcept;
    void set_u16(std::uint16_t addr, std::uint16_t value) noexcept;
    void set_u32(std::uint16_t addr, std::uint32_t value) noexcept;
    void set_u8(std::uint16_t addr, std::uint8_t value) noexcept;
};

class Chain {
public:
    explicit Chain(std::size_t slave_count);

    // Service a captured frame in place: walk every datagram, apply addressing
    // and read/write semantics against each ESC, and update the working
    // counters. Returns true if any datagram was serviced (so the frame should
    // be sent back to the master).
    [[nodiscard]] bool process_frame(std::span<std::byte> frame) noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return m_slaves.size(); }
    [[nodiscard]] Esc& slave(std::size_t index) noexcept { return m_slaves[index]; }

    // Set a slave's input process data (e.g. the EL1008's 8 DI). Writes `value`
    // to the physical start of the slave's first active read FMMU, where the
    // master's LRD reads it. No-op until the master has configured an input
    // FMMU (i.e. before the slave reaches SafeOp/Op). Returns true if written.
    bool set_process_input(std::size_t slave_index, std::uint8_t value) noexcept;

    // Set a CoE analog-input slave's process data (e.g. the EL3001): write the
    // 2-byte status word (offset 0) and INT16 value (offset 2) into its first
    // active multi-byte input FMMU's physical region, matching the TxPDO 0x1A00
    // layout. Skips the 1-byte mailbox-status FMMU. No-op until the master has
    // configured the input FMMU (i.e. before SafeOp/Op). Returns true if written.
    bool set_analog_input(std::size_t slave_index, std::int16_t value,
                          std::uint16_t status) noexcept;

    // Read a slave's output process data (e.g. the EL2008's 8 DO). Returns the
    // byte at the physical start of the slave's first active write FMMU, where
    // the master's LWR/LRW deposits it — so the caller can observe the output
    // round-trip. Returns std::nullopt until the master has configured an output
    // FMMU (i.e. before SafeOp/Op) or if the index is out of range.
    [[nodiscard]] std::optional<std::uint8_t> output_byte(
        std::size_t slave_index) const noexcept;

private:
    std::vector<Esc> m_slaves;
};

}  // namespace ecat
