// CoE (CANopen over EtherCAT) mailbox services — ETG.1000.5 (mailbox) and
// ETG.1000.6 (CoE / SDO).
//
// A slave's *acyclic* parameter channel: the master writes an SDO request into
// the slave's mailbox, the slave services it against its Object Dictionary and
// writes an SDO response back. This module is the protocol core only — it turns
// a request mailbox buffer into a response mailbox buffer. How those bytes move
// in and out of the slave (the mailbox SyncManager handshake) lives in chain.cpp.
//
// Scope (phase 1): expedited SDO upload/download (values up to 4 bytes) and the
// SDO abort response. Segmented/normal transfer (e.g. long strings) and the SDO
// Information service are deliberately not handled yet.
//
// This is off the cyclic hot path (mailbox traffic happens in PreOp/parameter
// setup, not per process-data cycle), so the std::map allocation here does not
// violate the "no allocation on the L2 dispatch hot path" rule.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace ecat::coe {

// One Object Dictionary entry: a little-endian value of 1, 2, or 4 bytes plus
// its access right. (Strings / arrays arrive with segmented transfer, a later
// step.)
struct Entry {
    std::uint32_t value = 0;
    std::uint8_t byte_size = 0;  // 1, 2, or 4
    bool writable = false;
};

// (index, subindex) -> Entry. Ordered map keyed by (index << 8 | subindex).
class ObjectDictionary {
public:
    void add(std::uint16_t index, std::uint8_t sub, std::uint32_t value,
             std::uint8_t byte_size, bool writable);

    [[nodiscard]] const Entry* find(std::uint16_t index, std::uint8_t sub) const noexcept;
    [[nodiscard]] Entry* find(std::uint16_t index, std::uint8_t sub) noexcept;
    [[nodiscard]] bool has_index(std::uint16_t index) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }

    // Distinct object indices, ascending (for the SDO-Information OD list).
    [[nodiscard]] std::vector<std::uint16_t> indices() const;
    // Highest sub-index present for an index (0 if only sub 0 / not found).
    [[nodiscard]] std::uint8_t max_subindex(std::uint16_t index) const noexcept;

private:
    static constexpr std::uint32_t key_of(std::uint16_t index, std::uint8_t sub) noexcept {
        return (static_cast<std::uint32_t>(index) << 8) | sub;
    }
    std::map<std::uint32_t, Entry> m_entries;
};

// SDO abort codes (ETG.1000.6 Table). Exposed for tests.
inline constexpr std::uint32_t kAbortNoSuchObject = 0x06020000;    // object missing
inline constexpr std::uint32_t kAbortNoSuchSubIndex = 0x06090011;  // sub-index missing
inline constexpr std::uint32_t kAbortReadOnly = 0x06010002;        // write to RO object
inline constexpr std::uint32_t kAbortBadCommand = 0x05040001;      // cs not valid

// Largest expedited SDO response this module produces (mailbox header 6 + CoE
// header 2 + SDO 8). Callers can size a fixed buffer with this.
inline constexpr std::size_t kMaxResponseBytes = 16;

// Largest mailbox response overall, including SDO-Information replies (object
// lists / descriptions). Bounded by the standard 128-byte mailbox buffer; the
// caller's response buffer must be at least this large.
inline constexpr std::size_t kMaxMailboxResponse = 128;

// Service one CoE mailbox request. `request` is the full mailbox frame as the
// master wrote it (6-byte mailbox header, CoE header, SDO). Writes the full
// response mailbox frame into `out` and returns its length, or 0 if the request
// is not a CoE SDO we service (caller then produces no mailbox response).
[[nodiscard]] std::size_t process_mailbox(ObjectDictionary& od,
                                          std::span<const std::byte> request,
                                          std::span<std::byte> out) noexcept;

// Enable/disable one-line stderr logging of every serviced SDO (FR-OBS-01).
// Off by default so unit tests stay quiet; `ec-core --serve` turns it on.
void set_logging(bool enabled) noexcept;

// Whether SDO/mailbox logging is currently on (so the chain can log the
// mailbox-in handshake under the same switch).
[[nodiscard]] bool logging_enabled() noexcept;

}  // namespace ecat::coe
