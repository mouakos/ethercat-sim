// EtherCAT frame / datagram parsing (L1, read side).
//
// An EtherCAT frame rides directly on Ethernet (EtherType 0x88a4). After the
// 14-byte Ethernet header comes a 2-byte EtherCAT header (little-endian:
// 11-bit length + 4-bit type), followed by one or more datagrams. Each datagram
// is a 10-byte header (cmd, index, 32-bit address, 11-bit data length + flags,
// 16-bit IRQ), then <length> data bytes, then a 16-bit working counter.
//
// This parser is non-allocating and read-only: it views into the caller's
// buffer and invokes a visitor per datagram. See ETG.1000.4 for the wire spec.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace ecat {

inline constexpr std::uint16_t kEtherType = 0x88a4;

// EtherCAT command codes (datagram header byte 0).
enum class Command : std::uint8_t {
    kNop = 0,
    kAprd = 1,   // auto-increment physical read
    kApwr = 2,
    kAprw = 3,
    kFprd = 4,   // configured-address physical read
    kFpwr = 5,
    kFprw = 6,
    kBrd = 7,    // broadcast read
    kBwr = 8,
    kBrw = 9,
    kLrd = 10,   // logical read
    kLwr = 11,
    kLrw = 12,
    kArmw = 13,
    kFrmw = 14,
};

[[nodiscard]] std::string_view to_string(Command c) noexcept;

// A read-only view of a single datagram inside a frame buffer.
struct DatagramView {
    Command cmd = Command::kNop;
    std::uint8_t index = 0;
    std::uint16_t adp = 0;               // address position/station (non-logical cmds)
    std::uint16_t ado = 0;               // register offset (non-logical cmds)
    std::uint32_t logical_address = 0;   // full 32-bit address (LRD/LWR/LRW)
    std::span<const std::byte> data;     // <length> data bytes
    std::uint16_t wkc = 0;               // working counter
    bool more = false;                   // M bit: another datagram follows
    bool circulating = false;            // C bit: frame is circulating
    std::size_t address_offset = 0;      // byte offset of the 4-byte address field
    std::size_t data_offset = 0;         // byte offset of `data` within the frame
    std::size_t wkc_offset = 0;          // byte offset of the working counter
};

namespace detail {

[[nodiscard]] constexpr std::uint16_t read_u16_le(std::span<const std::byte> b,
                                                   std::size_t off) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(b[off]) |
                                      (std::to_integer<std::uint16_t>(b[off + 1]) << 8));
}

[[nodiscard]] constexpr std::uint32_t read_u32_le(std::span<const std::byte> b,
                                                   std::size_t off) noexcept {
    return std::to_integer<std::uint32_t>(b[off]) |
           (std::to_integer<std::uint32_t>(b[off + 1]) << 8) |
           (std::to_integer<std::uint32_t>(b[off + 2]) << 16) |
           (std::to_integer<std::uint32_t>(b[off + 3]) << 24);
}

constexpr void write_u16_le(std::span<std::byte> b, std::size_t off,
                            std::uint16_t value) noexcept {
    b[off] = static_cast<std::byte>(value & 0xFFu);
    b[off + 1] = static_cast<std::byte>((value >> 8) & 0xFFu);
}

}  // namespace detail

// Parse the datagrams out of a full Ethernet frame, calling visit(const
// DatagramView&) for each in order. Returns the number of datagrams, or
// std::nullopt if the frame is not EtherCAT or is malformed (a declared data
// length that overruns the buffer).
template <typename Visitor>
[[nodiscard]] std::optional<std::size_t> for_each_datagram(
    std::span<const std::byte> frame, Visitor&& visit) {
    constexpr std::size_t kEthHeaderLen = 14;
    constexpr std::size_t kEcatHeaderLen = 2;
    constexpr std::size_t kDatagramHeaderLen = 10;
    constexpr std::size_t kWkcLen = 2;

    if (frame.size() < kEthHeaderLen + kEcatHeaderLen) {
        return std::nullopt;
    }
    // EtherType is big-endian on the wire.
    const std::uint16_t ethertype = static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(frame[12]) << 8) |
        std::to_integer<std::uint16_t>(frame[13]));
    if (ethertype != kEtherType) {
        return std::nullopt;
    }

    const std::uint16_t ecat_header = detail::read_u16_le(frame, kEthHeaderLen);
    const std::size_t declared_len = ecat_header & 0x07FFu;
    std::size_t area_end = kEthHeaderLen + kEcatHeaderLen + declared_len;
    if (area_end > frame.size()) {
        area_end = frame.size();  // tolerate padding / short declared length
    }

    std::size_t pos = kEthHeaderLen + kEcatHeaderLen;
    std::size_t count = 0;
    for (;;) {
        if (pos + kDatagramHeaderLen + kWkcLen > area_end) {
            break;  // no room for a further datagram
        }
        DatagramView dg{};
        dg.cmd = static_cast<Command>(std::to_integer<std::uint8_t>(frame[pos]));
        dg.index = std::to_integer<std::uint8_t>(frame[pos + 1]);
        dg.address_offset = pos + 2;
        const std::uint32_t addr = detail::read_u32_le(frame, pos + 2);
        dg.adp = static_cast<std::uint16_t>(addr & 0xFFFFu);
        dg.ado = static_cast<std::uint16_t>((addr >> 16) & 0xFFFFu);
        dg.logical_address = addr;

        const std::uint16_t len_flags = detail::read_u16_le(frame, pos + 6);
        const std::size_t data_len = len_flags & 0x07FFu;
        dg.circulating = (len_flags & 0x4000u) != 0;
        dg.more = (len_flags & 0x8000u) != 0;

        const std::size_t data_off = pos + kDatagramHeaderLen;
        if (data_off + data_len + kWkcLen > frame.size()) {
            return std::nullopt;  // declared data length overruns the buffer
        }
        dg.data = frame.subspan(data_off, data_len);
        dg.data_offset = data_off;
        dg.wkc = detail::read_u16_le(frame, data_off + data_len);
        dg.wkc_offset = data_off + data_len;

        visit(static_cast<const DatagramView&>(dg));
        ++count;

        pos = data_off + data_len + kWkcLen;
        if (!dg.more) {
            break;
        }
    }
    return count;
}

}  // namespace ecat
