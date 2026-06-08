// Unit tests for the EtherCAT frame parser. Dependency-free: a tiny check()
// harness, exit code 0 on success. Run via ctest.

#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <vector>

#include "ecat/frame.hpp"

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

}  // namespace

int main() {
    using ecat::Command;
    using ecat::DatagramView;
    using ecat::for_each_datagram;

    // A real BRD-of-AL-Status(0x0130) frame, as TwinCAT sends during the scan.
    const std::vector<std::byte> brd = bytes({
        0x01, 0x01, 0x05, 0x01, 0x00, 0x00,              // dst MAC (Beckhoff mcast)
        0x38, 0x14, 0x28, 0x01, 0x96, 0x76,              // src MAC (PLC)
        0x88, 0xa4,                                      // ethertype 0x88a4
        0x0e, 0x10,                                      // ecat hdr: len=14, type=1
        0x07, 0xbd, 0x00, 0x00, 0x30, 0x01, 0x02, 0x00,  // BRD idx adp=0 ado=0x0130 len=2
        0x00, 0x00,                                      // irq
        0x00, 0x00,                                      // data (2 bytes)
        0x00, 0x00,                                      // wkc=0
    });
    std::vector<DatagramView> seen;
    auto n = for_each_datagram(brd, [&](const DatagramView& d) { seen.push_back(d); });
    check(n.has_value(), "BRD frame parses");
    check(n.value_or(0) == 1, "BRD frame has one datagram");
    if (seen.size() == 1) {
        check(seen[0].cmd == Command::kBrd, "cmd == BRD");
        check(seen[0].adp == 0x0000, "adp == 0");
        check(seen[0].ado == 0x0130, "ado == 0x0130 (AL Status)");
        check(seen[0].data.size() == 2, "data length == 2");
        check(seen[0].wkc == 0, "wkc == 0");
        check(!seen[0].more, "more == false (last datagram)");
    }

    // Non-EtherCAT ethertype is rejected.
    const std::vector<std::byte> ip = bytes({
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x08, 0x00, 0, 0, 0, 0,
    });
    check(!for_each_datagram(ip, [](const DatagramView&) {}).has_value(),
          "non-0x88a4 frame rejected");

    // A declared data length that overruns the buffer is malformed.
    const std::vector<std::byte> overrun = bytes({
        0x01, 0x01, 0x05, 0x01, 0x00, 0x00,
        0x38, 0x14, 0x28, 0x01, 0x96, 0x76,
        0x88, 0xa4,
        0x0e, 0x10,
        0x07, 0xbd, 0x00, 0x00, 0x30, 0x01, 0x40, 0x00,  // len flags claim 64 data bytes
        0x00, 0x00,
        0x00, 0x00,
        0x00, 0x00,
    });
    check(!for_each_datagram(overrun, [](const DatagramView&) {}).has_value(),
          "datagram overrunning the buffer rejected");

    // Two datagrams in one frame; the first has the M (more) bit set.
    const std::vector<std::byte> two = bytes({
        0x01, 0x01, 0x05, 0x01, 0x00, 0x00,
        0x38, 0x14, 0x28, 0x01, 0x96, 0x76,
        0x88, 0xa4,
        0x1c, 0x10,                                      // ecat hdr: len=28, type=1
        0x07, 0x01, 0x00, 0x00, 0x00, 0x01, 0x02, 0x80,  // BRD ado=0x0100 len=2 more=1
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,              // irq, data, wkc
        0x07, 0x02, 0x00, 0x00, 0x30, 0x01, 0x02, 0x00,  // BRD ado=0x0130 len=2 more=0
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,              // irq, data, wkc
    });
    seen.clear();
    n = for_each_datagram(two, [&](const DatagramView& d) { seen.push_back(d); });
    check(n.value_or(0) == 2, "two-datagram frame yields two datagrams");
    if (seen.size() == 2) {
        check(seen[0].ado == 0x0100, "first datagram ado == 0x0100");
        check(seen[0].more, "first datagram more == true");
        check(seen[1].ado == 0x0130, "second datagram ado == 0x0130");
        check(!seen[1].more, "second datagram more == false");
    }

    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
