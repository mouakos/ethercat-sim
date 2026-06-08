// ec-core — software EtherCAT slave stack entry point.
//
// Stage 1: capture and parse. Reads frames from a live NIC or an offline
// capture file, dissects the EtherCAT datagrams, and prints a summary of the
// commands and registers seen — our own version of what tshark gave us during
// Milestone 1. No responses are sent yet (that is Stage 2).
//
// Usage:
//   ec-core --list                 list capture devices (find the NPF name)
//   ec-core --offline <file>       dissect a .pcap/.pcapng capture
//   ec-core --iface <device>       capture live (needs Administrator)

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <span>
#include <string_view>

#include <pcap.h>

#include "ecat/chain.hpp"
#include "ecat/frame.hpp"
#include "ecat/station.hpp"
#include "ecat/telltale.hpp"  // TELLTALE: diagnostic counters (grep TELLTALE to remove)
#include "net/pcap_source.hpp"
#include "net/priority.hpp"

namespace {

struct CaptureStats {
    std::size_t frames = 0;       // EtherCAT frames parsed
    std::size_t datagrams = 0;    // datagrams across all frames
    std::size_t non_ecat = 0;     // captured packets that were not EtherCAT
    std::array<std::size_t, 16> by_command{};
    std::map<std::uint16_t, std::size_t> by_register;  // ADO -> count
};

void account(CaptureStats& stats, const ecat::DatagramView& dg) {
    ++stats.datagrams;
    const auto code = static_cast<std::size_t>(dg.cmd);
    if (code < stats.by_command.size()) {
        ++stats.by_command[code];
    }
    ++stats.by_register[dg.ado];
}

void print_summary(const CaptureStats& stats) {
    std::printf("\n== capture summary ==\n");
    std::printf("EtherCAT frames: %zu | datagrams: %zu | non-EtherCAT skipped: %zu\n",
                stats.frames, stats.datagrams, stats.non_ecat);

    std::printf("by command:\n");
    for (std::size_t i = 0; i < stats.by_command.size(); ++i) {
        if (stats.by_command[i] == 0) {
            continue;
        }
        const std::string_view name = ecat::to_string(static_cast<ecat::Command>(i));
        std::printf("  %-4.*s (%2zu): %zu\n",
                    static_cast<int>(name.size()), name.data(), i, stats.by_command[i]);
    }

    std::printf("by register (ADO):\n");
    for (const auto& [ado, n] : stats.by_register) {
        std::printf("  0x%04x: %zu\n", static_cast<unsigned>(ado), n);
    }
}

void dissect(CaptureStats& stats, std::span<const std::byte> frame) {
    const auto datagrams =
        ecat::for_each_datagram(frame, [&](const ecat::DatagramView& dg) {
            account(stats, dg);
        });
    if (datagrams.has_value()) {
        ++stats.frames;
    } else {
        ++stats.non_ecat;
    }
}

int run_offline(std::string_view path) {
    net::PcapSource source = net::PcapSource::open_offline(path);
    CaptureStats stats;
    for (;;) {
        const net::ReadResult r = source.next();
        if (r.status == net::ReadStatus::kEndOfFile) {
            break;
        }
        if (r.status == net::ReadStatus::kError) {
            std::fprintf(stderr, "read error\n");
            return 1;
        }
        if (r.status == net::ReadStatus::kPacket) {
            dissect(stats, r.bytes);
        }
    }
    print_summary(stats);
    return 0;
}

int run_live(std::string_view device) {
    net::PcapSource source = net::PcapSource::open_live(device);
    CaptureStats stats;
    std::printf("capturing on %.*s (Ctrl-C to stop)...\n",
                static_cast<int>(device.size()), device.data());
    for (;;) {
        const net::ReadResult r = source.next();
        if (r.status == net::ReadStatus::kError) {
            std::fprintf(stderr, "read error\n");
            return 1;
        }
        if (r.status != net::ReadStatus::kPacket) {
            continue;  // timeout — keep polling
        }
        dissect(stats, r.bytes);
        if (stats.frames > 0 && stats.frames % 1000 == 0) {
            print_summary(stats);
        }
    }
}

int run_serve(std::string_view device, double duration_seconds,
              std::size_t slave_count, bool enable_log) {
    net::raise_responder_priority();  // cut scheduling jitter that drops OP
    // Per-event CoE/mailbox logging does a blocking fprintf on the responder
    // thread; during the cyclic PreOp->SafeOp mailbox burst that stalls frame
    // servicing -> dropped frames -> "clear sm pdos" timeout -> the EL3001 flaps
    // OP<->PreOp. Keep it OFF on the hot path for stable cyclic bring-up; pass
    // --log to flip it on for diagnosing mailbox content (e.g. which SDO aborts).
    // (FR-OBS-01 wants observability without perturbing timing — a later
    // async/ring-buffer log is the real fix.)
    ecat::coe::set_logging(enable_log);
    net::PcapSource source = net::PcapSource::open_live(device);
    std::printf("serving as EtherCAT station (%u ESCs) on %.*s%s\n",
                static_cast<unsigned>(slave_count),
                static_cast<int>(device.size()), device.data(),
                duration_seconds > 0.0 ? "" : " (Ctrl-C to stop)");

    ecat::Chain chain(slave_count);
    CaptureStats stats;
    std::size_t responded = 0;
    std::size_t send_failures = 0;
    std::array<std::byte, 2048> tx{};
    const auto start = std::chrono::steady_clock::now();

    // TELLTALE: responder-health instrumentation (step 3 — measure before
    // optimizing the Tier-1 jitter). The decisive number is Npcap's own drop
    // delta per window: frames the driver discarded because we did not drain
    // them fast enough. svc_max/slow capture our per-frame service latency so we
    // can tell a *driver* drop (we were busy) from a *send* stall. A [resp] line
    // prints every ~2 s off the per-frame path. Grep TELLTALE to remove.
    std::size_t rx_packets = 0;       // frames Npcap delivered to us
    std::size_t echo_skips = 0;       // our own replies re-captured (loopback)
    long long svc_max_us = 0;         // worst per-frame service latency this window
    std::size_t svc_slow = 0;         // frames serviced in > 1 ms this window
    net::CaptureCounters npcap_prev{};      // Npcap counters at the last window
    auto last_resp_report = start;

    // TELLTALE: announce the diagnostic instrumentation is live. A ~2 s health
    // line follows only when an anomaly trips (silent when healthy). Remove this
    // block + everything tagged TELLTALE to retire the instrumentation.
    std::fprintf(stderr,
                 "[telltale] instrumentation active (grep TELLTALE in sim/ to find or remove)\n");
    auto last_telltale = start;

    // Milestone 4: drive the EL1008's 8 DI as a walking bit (one channel high
    // at a time, advancing every second) so the input round-trip is visible in
    // TwinCAT. Slave index 1 is the EL1008 (slave 0 is the EK1100 coupler).
    auto last_di_update = start;
    std::uint8_t di_value = 0x01;

    // "the rest": drive the EL3001's analog input (slave 3) as a slow sine-wave
    // virtual sensor so the PLC reads a live, changing INT16 value (and the
    // terminal's WcState goes valid). Updated at ~20 Hz; the status word's TxPDO
    // Toggle bit (bit 15) flips each update so the master sees fresh data, with
    // all error/range bits clear. ~6 s period, +/-20000 counts (~+/-6 V).
    auto last_ai_update = start;
    bool ai_toggle = false;
    std::int16_t ai_value = 0;
    bool ai_served = false;  // true once the EL3001 input FMMU is live (SafeOp/Op)

    // Milestone 6: observe the EL2008's 8 DO. The master writes them via LWR
    // into our output FMMU; reading them back demonstrates the DO round-trip
    // (AC-04). Slave index 2 is the EL2008 (0 = EK1100, 1 = EL1008). Report only
    // on change to keep the log quiet; -1 means "nothing seen yet".
    int last_do = -1;

    // Loopback detection. pcap_setdirection(PCAP_D_IN) is unsupported on this
    // NIC, so the capture handle also delivers the frames we transmit. We must
    // NOT rewrite the source MAC of our replies — a kernel-mode EtherCAT master
    // only accepts a returned frame that still carries its own (the master's)
    // source MAC, exactly as a real slave ring preserves it. So instead of
    // recognising our echoes by a synthetic MAC, we fingerprint every frame we
    // send (FNV-1a over its bytes) and skip any captured frame whose
    // fingerprint we just transmitted. Our reply always differs from the
    // master's request (modified working counter / data), so the master's
    // requests are never mistaken for echoes.
    std::array<std::uint64_t, 512> recent_tx{};
    std::size_t recent_idx = 0;
    const auto fingerprint = [](std::span<const std::byte> bytes) noexcept {
        std::uint64_t h = 1469598103934665603ull;  // FNV-1a 64-bit offset basis
        for (const std::byte b : bytes) {
            h ^= std::to_integer<std::uint8_t>(b);
            h *= 1099511628211ull;  // FNV prime
        }
        return h;
    };
    const auto is_recent_tx = [&](std::uint64_t h) noexcept {
        for (const std::uint64_t v : recent_tx) {
            if (v == h) {
                return true;
            }
        }
        return false;
    };

    for (;;) {
        if (duration_seconds > 0.0) {
            const std::chrono::duration<double> elapsed =
                std::chrono::steady_clock::now() - start;
            if (elapsed.count() >= duration_seconds) {
                break;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_ai_update >= std::chrono::milliseconds(50)) {
            last_ai_update = now;
            const double t = std::chrono::duration<double>(now - start).count();
            ai_value = static_cast<std::int16_t>(
                20000.0 * std::sin(2.0 * 3.141592653589793 * t / 6.0));
            ai_toggle = !ai_toggle;
            const std::uint16_t status = ai_toggle ? 0x8000u : 0x0000u;  // TxPDO Toggle
            // Returns true only once the master has configured the input FMMU, i.e.
            // the EL3001 has reached SafeOp/Op — so this doubles as a "PD is live"
            // signal and keeps the console quiet while it is still at PreOp.
            ai_served = chain.set_analog_input(3, ai_value, status);  // EL3001 = slave 3
        }
        if (now - last_di_update >= std::chrono::seconds(1)) {
            last_di_update = now;
            chain.set_process_input(1, di_value);  // EL1008 = slave 1
            di_value = (di_value == 0x80) ? 0x01 : static_cast<std::uint8_t>(di_value << 1);
            if (ai_served) {  // only once the EL3001 is exchanging process data
                std::printf("EL3001 AI = %d (served)\n", static_cast<int>(ai_value));
            }
        }
        // TELLTALE: off the per-frame path, emit the health line every ~2 s (it
        // prints only when an anomaly counter is non-zero, so healthy = silent).
        // During the SAFEOP investigation this was temporarily forced to print every
        // tick; pass force=true here again if you need the evidence counters
        // (sm0-read / mbx-buf-read / deferred) visible on every outcome.
        if (now - last_telltale >= std::chrono::seconds(2)) {
            last_telltale = now;
            ecat::telltale::report();  // prints only on an anomaly (kept off the timing path)
        }
        // TELLTALE: responder-health line every ~2 s (off the per-frame path).
        // Watch `drop(+N)` during the PreOp->SafeOp burst: a non-zero delta is
        // Npcap discarding frames we never drained -> the cyclic mailbox
        // handshake misses a frame -> "clear sm pdos (0x1C12)" timeout -> flap.
        if (now - last_resp_report >= std::chrono::seconds(2)) {
            last_resp_report = now;
            const net::CaptureCounters nc = source.stats();
            const double t = std::chrono::duration<double>(now - start).count();
            if (nc.valid) {
                std::fprintf(stderr,
                             "[resp t=%5.1fs] rx=%zu serviced=%zu sendfail=%zu echo=%zu | "
                             "npcap recv=%u drop=%u(+%u) ifdrop=%u | "
                             "svc_max=%lldus slow>1ms=%zu (2s win)\n",
                             t, rx_packets, responded, send_failures, echo_skips,
                             nc.recv, nc.drop, nc.drop - npcap_prev.drop, nc.if_drop,
                             svc_max_us, svc_slow);
                npcap_prev = nc;
            } else {
                std::fprintf(stderr,
                             "[resp t=%5.1fs] rx=%zu serviced=%zu sendfail=%zu echo=%zu | "
                             "npcap stats unavailable | svc_max=%lldus slow>1ms=%zu (2s win)\n",
                             t, rx_packets, responded, send_failures, echo_skips,
                             svc_max_us, svc_slow);
            }
            svc_max_us = 0;  // reset the per-window peak / slow tallies
            svc_slow = 0;
        }

        const net::ReadResult r = source.next();
        if (r.status == net::ReadStatus::kError) {
            std::fprintf(stderr, "read error\n");
            return 1;
        }
        if (r.status != net::ReadStatus::kPacket) {
            continue;  // timeout — keep polling
        }
        ++rx_packets;  // TELLTALE: every frame Npcap handed us this window
        if (is_recent_tx(fingerprint(r.bytes))) {
            ++echo_skips;  // TELLTALE: loopback echo of our own reply
            continue;  // our own reply echoed back to the capture handle
        }
        // TELLTALE: time the whole service path (parse + process + send). A spike
        // here means *we* stalled (e.g. a blocking send); a climbing Npcap drop
        // with a low svc_max means the driver buffer overran while we were idle.
        const auto svc_start = std::chrono::steady_clock::now();
        dissect(stats, r.bytes);
        if (r.bytes.size() <= tx.size()) {
            std::memcpy(tx.data(), r.bytes.data(), r.bytes.size());
            const std::span<std::byte> frame(tx.data(), r.bytes.size());
            if (chain.process_frame(frame)) {
                // Reply with the master's frame unchanged except the EtherCAT
                // payload (working counter / data) — source MAC preserved.
                recent_tx[recent_idx] = fingerprint(frame);
                recent_idx = (recent_idx + 1) % recent_tx.size();
                if (source.send(frame)) {
                    ++responded;
                } else {
                    ++send_failures;
                }
            }
        }
        const long long svc_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - svc_start).count();  // TELLTALE
        if (svc_us > svc_max_us) {
            svc_max_us = svc_us;
        }
        if (svc_us > 1000) {  // > 1 ms to service one frame is a stall
            ++svc_slow;
        }

        // Report the EL2008 output image whenever the master changes it.
        if (const auto do_value = chain.output_byte(2)) {
            if (static_cast<int>(*do_value) != last_do) {
                last_do = *do_value;
                char bits[9];
                for (int b = 0; b < 8; ++b) {
                    bits[b] = ((*do_value >> (7 - b)) & 1) ? '1' : '0';
                }
                bits[8] = '\0';
                std::printf("EL2008 DO = 0x%02x  [%s]\n",
                            static_cast<unsigned>(*do_value), bits);
            }
        }
    }

    print_summary(stats);
    std::printf("responded to %zu frame(s), %zu send failure(s)\n", responded,
                send_failures);
    return 0;
}

// Optional "--slaves N" override of the emulated chain length (default
// kEmulatedSlaveCount). Lets us toggle the parked EL6224 (5th ESC) on/off for the
// rig without a recompile: "--slaves 5" presents it, default (4) leaves it parked.
std::size_t parse_slave_count(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--slaves") {
            const int n = std::atoi(argv[i + 1]);
            if (n > 0 && n <= 32) {
                return static_cast<std::size_t>(n);
            }
        }
    }
    return ecat::kEmulatedSlaveCount;
}

// Optional "--log" flag: turn on per-SDO [coe]/[mbx] logging (off by default to
// keep the responder hot path clean). Use it to see which SDO aborts / mailbox
// content during a bring-up; expect some timing perturbation while it's on.
bool parse_log_flag(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--log") {
            return true;
        }
    }
    return false;
}

int usage() {
    std::fprintf(stderr,
                 "ec-core (ethercat-sim) — linked %s\n"
                 "usage:\n"
                 "  ec-core --list                  list capture devices\n"
                 "  ec-core --offline <file>        dissect a .pcap/.pcapng capture\n"
                 "  ec-core --iface <device>        capture live (needs Administrator)\n"
                 "  ec-core --serve <device> [secs] [--slaves N] [--log]\n"
                 "                                  respond as the slave station "
                 "(needs Administrator;\n"
                 "                                  --slaves N overrides the chain "
                 "length, e.g. 5 to add the EL6224;\n"
                 "                                  --log turns on per-SDO [coe] "
                 "logging for diagnosis)\n",
                 pcap_lib_version());
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc >= 2 && std::string_view(argv[1]) == "--list") {
            net::list_devices();
            return 0;
        }
        if (argc >= 3 && std::string_view(argv[1]) == "--offline") {
            return run_offline(argv[2]);
        }
        if (argc >= 3 && std::string_view(argv[1]) == "--iface") {
            return run_live(argv[2]);
        }
        if (argc >= 3 && std::string_view(argv[1]) == "--serve") {
            // argv[3], if present and numeric, is the run duration in seconds;
            // "--slaves N" overrides the chain length; "--log" enables SDO logging.
            const double seconds =
                (argc >= 4 && std::atof(argv[3]) > 0.0) ? std::atof(argv[3]) : 0.0;
            return run_serve(argv[2], seconds, parse_slave_count(argc, argv),
                             parse_log_flag(argc, argv));
        }
        return usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
