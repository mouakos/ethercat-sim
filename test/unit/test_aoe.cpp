// Unit test for the AoE (ADS over EtherCAT) mailbox handler. Feeds the exact
// "download NetId" request TwinCAT sends to the EL6224 (captured in
// el6224_aoe.pcapng, frame 11503) and checks the ADS Write response.

#include "ecat/aoe.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

std::uint16_t rd16(std::span<const std::byte> b, std::size_t o) {
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(b[o]) |
        (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(b[o + 1])) << 8));
}

std::uint32_t rd32(std::span<const std::byte> b, std::size_t o) {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[o])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[o + 1])) << 8) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[o + 2])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[o + 3])) << 24);
}

}  // namespace

int main() {
    // The exact AoE "download NetId" request from the rig capture: mailbox header
    // (type 1 = AoE) + AMS header (ADS Write, invoke id 1) + ADS payload writing a
    // 6-byte NetId (192.168.178.105.3.9) to IndexGroup 1 / IndexOffset 3.
    const std::vector<std::uint8_t> req = {
        0x32, 0x00, 0x00, 0x00, 0x00, 0x01,              // mbx: len 50, addr 0, type 1
        0xc0, 0xa8, 0xb2, 0x69, 0x03, 0x01, 0xed, 0x03,  // AMS target NetId + port 1005
        0xc0, 0xa8, 0xb2, 0x69, 0x03, 0x01, 0xed, 0x03,  // AMS sender NetId + port 1005
        0x03, 0x00,                                      // CmdId = 3 (ADS Write)
        0x04, 0x00,                                      // StateFlags = 0x0004 (request)
        0x12, 0x00, 0x00, 0x00,                          // cbData = 18
        0x00, 0x00, 0x00, 0x00,                          // ErrorCode = 0
        0x01, 0x00, 0x00, 0x00,                          // InvokeId = 1
        0x01, 0x00, 0x00, 0x00,                          // IndexGroup = 1
        0x03, 0x00, 0x00, 0x00,                          // IndexOffset = 3
        0x06, 0x00, 0x00, 0x00,                          // CbLength = 6
        0xc0, 0xa8, 0xb2, 0x69, 0x03, 0x09};             // NetId payload

    std::vector<std::byte> request(req.size());
    for (std::size_t i = 0; i < req.size(); ++i) {
        request[i] = std::byte{req[i]};
    }

    std::array<std::byte, 128> out{};
    const std::size_t n = ecat::aoe::process_mailbox(request, out);
    const std::span<const std::byte> r(out.data(), n);

    check(n == 6 + 32 + 4, "AoE Write response = 42 bytes (mbx 6 + AMS 32 + result 4)");
    check((std::to_integer<std::uint8_t>(r[5]) & 0x0F) == ecat::aoe::kMailboxType,
          "response mailbox type is AoE (1)");
    check(rd16(r, 0) == 32 + 4, "mailbox length = AMS 32 + result 4");
    check(rd16(r, 6 + 16) == 3, "response CmdId echoes ADS Write (3)");
    check((rd16(r, 6 + 18) & 0x0001) != 0, "response StateFlags has the response bit set");
    check(rd32(r, 6 + 20) == 4, "response cbData = 4 (result only)");
    check(rd32(r, 6 + 24) == 0, "response AMS ErrorCode = 0");
    check(rd32(r, 6 + 28) == 1, "response echoes InvokeId 1");
    check(rd32(r, 6 + 32) == 0, "ADS result = 0 (success)");
    check(std::to_integer<std::uint8_t>(r[6 + 0]) == 0xc0 &&
              std::to_integer<std::uint8_t>(r[6 + 6]) == 0xed,
          "response AMS target = the request's sender (swapped)");

    // A non-AoE mailbox (type 3 = CoE) must not be serviced by this handler.
    request[5] = std::byte{0x03};
    check(ecat::aoe::process_mailbox(request, out) == 0,
          "non-AoE mailbox (type 3) returns 0");

    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
