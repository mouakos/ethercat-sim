#include "ecat/aoe.hpp"

namespace ecat::aoe {
namespace {

constexpr std::size_t kMbxHeader = 6;   // EtherCAT mailbox header
constexpr std::size_t kAmsHeader = 32;  // AMS/ADS header

// AMS header field offsets, relative to the start of the AMS header (i.e. after
// the 6-byte mailbox header). Layout per ETG.1020 / Beckhoff ADS, little-endian.
constexpr std::size_t kOffTargetNetId = 0;   // 6 bytes
constexpr std::size_t kOffSenderNetId = 8;   // 6 bytes (port at +6 of each id)
constexpr std::size_t kOffCmdId = 16;        // 2
constexpr std::size_t kOffStateFlags = 18;   // 2
constexpr std::size_t kOffCbData = 20;       // 4
constexpr std::size_t kOffErrorCode = 24;    // 4
constexpr std::size_t kOffInvokeId = 28;     // 4
constexpr std::size_t kOffAdsData = 32;      // ADS payload start

constexpr std::uint16_t kStateResponse = 0x0001;  // AMS state flag bit 0 = response

// ADS command IDs (ETG.1020 / Beckhoff ADS).
constexpr std::uint16_t kAdsRead = 2;
constexpr std::uint16_t kAdsReadWrite = 9;

std::uint16_t rd16(std::span<const std::byte> b, std::size_t o) noexcept {
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(b[o]) |
        (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(b[o + 1])) << 8));
}

std::uint32_t rd32(std::span<const std::byte> b, std::size_t o) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[o])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[o + 1])) << 8) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[o + 2])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[o + 3])) << 24);
}

void wr16(std::span<std::byte> b, std::size_t o, std::uint16_t v) noexcept {
    b[o] = std::byte{static_cast<std::uint8_t>(v & 0xFF)};
    b[o + 1] = std::byte{static_cast<std::uint8_t>((v >> 8) & 0xFF)};
}

void wr32(std::span<std::byte> b, std::size_t o, std::uint32_t v) noexcept {
    b[o] = std::byte{static_cast<std::uint8_t>(v & 0xFF)};
    b[o + 1] = std::byte{static_cast<std::uint8_t>((v >> 8) & 0xFF)};
    b[o + 2] = std::byte{static_cast<std::uint8_t>((v >> 16) & 0xFF)};
    b[o + 3] = std::byte{static_cast<std::uint8_t>((v >> 24) & 0xFF)};
}

}  // namespace

std::size_t process_mailbox(std::span<const std::byte> request,
                            std::span<std::byte> out) noexcept {
    if (request.size() < kMbxHeader + kAmsHeader) {
        return 0;
    }
    if ((std::to_integer<std::uint8_t>(request[5]) & 0x0F) != kMailboxType) {
        return 0;  // not an AoE mailbox message
    }
    const std::span<const std::byte> ams = request.subspan(kMbxHeader);
    const std::uint16_t cmd_id = rd16(ams, kOffCmdId);
    const std::uint16_t state = rd16(ams, kOffStateFlags);
    const std::uint32_t invoke = rd32(ams, kOffInvokeId);
    if ((state & kStateResponse) != 0) {
        return 0;  // already a response (not a master request) — ignore
    }

    // Acknowledge the ADS request with success. The response ADS payload is:
    //   Write / WriteControl: a 4-byte result code.
    //   Read / ReadWrite:     a 4-byte result code + 4-byte length (0 = no data).
    // The NetId download is an ADS Write, so a 4-byte result of 0 is all it needs.
    const bool has_read_len = (cmd_id == kAdsRead || cmd_id == kAdsReadWrite);
    const std::size_t ads_len = has_read_len ? 8u : 4u;
    const std::size_t total = kMbxHeader + kAmsHeader + ads_len;
    if (out.size() < total) {
        return 0;
    }
    for (std::size_t i = 0; i < total; ++i) {
        out[i] = std::byte{0};
    }

    // Mailbox header: length = AMS header + ADS data; type = AoE. The 3-bit
    // sequence counter (header byte 5, bits 4..6) is patched by the caller.
    wr16(out, 0, static_cast<std::uint16_t>(kAmsHeader + ads_len));
    out[5] = std::byte{kMailboxType};

    const std::span<std::byte> ores = out.subspan(kMbxHeader);
    // Swap target<->sender (NetId 6 + port 2 = 8 bytes each): the reply goes from
    // the addressed device back to whoever sent the request.
    for (std::size_t i = 0; i < 8; ++i) {
        ores[kOffTargetNetId + i] = ams[kOffSenderNetId + i];
        ores[kOffSenderNetId + i] = ams[kOffTargetNetId + i];
    }
    wr16(ores, kOffCmdId, cmd_id);
    wr16(ores, kOffStateFlags, static_cast<std::uint16_t>(state | kStateResponse));
    wr32(ores, kOffCbData, static_cast<std::uint32_t>(ads_len));
    wr32(ores, kOffErrorCode, 0);      // AMS error code: no error
    wr32(ores, kOffInvokeId, invoke);  // echo invoke id (pairs reply to request)
    wr32(ores, kOffAdsData, 0);        // ADS result: 0 = success
    if (has_read_len) {
        wr32(ores, kOffAdsData + 4, 0);  // ADS read length: 0 (no data returned yet)
    }
    return total;
}

}  // namespace ecat::aoe
