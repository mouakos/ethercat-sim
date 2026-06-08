// Unit tests for the slave identity helpers (source-MAC stamping / own-frame
// detection) used to break the capture feedback loop.

#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "ecat/station.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s: %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) {
        ++g_failures;
    }
}

std::vector<std::byte> bytes(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (const int v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

int as_int(std::byte b) { return static_cast<int>(std::to_integer<unsigned>(b)); }

}  // namespace

int main() {
    // A frame with the PLC source MAC is not one of ours.
    std::vector<std::byte> frame = bytes({
        0x01, 0x01, 0x05, 0x01, 0x00, 0x00,
        0x38, 0x14, 0x28, 0x01, 0x96, 0x76,
        0x88, 0xa4,
    });
    check(!ecat::is_own_transmission(frame), "master frame is not seen as our own");

    ecat::stamp_source_mac(frame);
    check(ecat::is_own_transmission(frame), "stamped frame is recognised as our own");
    check(as_int(frame[6]) == ecat::kSlaveMac[0] && as_int(frame[11]) == ecat::kSlaveMac[5],
          "source MAC overwritten with kSlaveMac");

    // Too-short buffers are handled safely.
    std::vector<std::byte> tiny = bytes({0x00, 0x01, 0x02});
    check(!ecat::is_own_transmission(tiny), "short frame is not our own");

    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
