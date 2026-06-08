// RAII wrapper over an Npcap/libpcap capture handle (L1 wire access).
//
// Supports both a live NIC and an offline capture file behind one interface so
// the parser can be exercised against saved traces without binding a NIC.

#pragma once

#include <cstddef>
#include <span>
#include <string_view>

struct pcap;  // libpcap's pcap_t (typedef struct pcap pcap_t)

namespace net {

enum class ReadStatus {
    kPacket,       // a packet is available in ReadResult::bytes
    kTimeout,      // live read timed out; no packet this call (keep polling)
    kEndOfFile,    // offline capture exhausted
    kError,        // unrecoverable read error
};

struct ReadResult {
    ReadStatus status = ReadStatus::kError;
    std::span<const std::byte> bytes;  // valid until the next next() call
};

// Driver-side capture counters (libpcap/Npcap pcap_stats). The decisive metric
// for responder health is `drop`: frames the kernel/Npcap buffer discarded
// because userspace did not drain them in time. A non-zero, climbing `drop`
// during the cyclic PreOp->SafeOp burst is the signature of the Tier-1
// userspace-timing limit (HANDOVER §7) — i.e. why the EL3001 flaps OP<->PreOp.
struct CaptureCounters {
    unsigned recv = 0;     // ps_recv:   packets the capture filter accepted
    unsigned drop = 0;     // ps_drop:   dropped — capture buffer full (we were too slow)
    unsigned if_drop = 0;  // ps_ifdrop: dropped by the NIC/driver (often 0/unsupported)
    bool valid = false;    // false if pcap_stats failed (e.g. an offline source)
};

class PcapSource {
public:
    PcapSource(const PcapSource&) = delete;
    PcapSource& operator=(const PcapSource&) = delete;
    PcapSource(PcapSource&& other) noexcept;
    PcapSource& operator=(PcapSource&& other) noexcept;
    ~PcapSource();

    // Open a capture file (pcap/pcapng). Throws std::runtime_error on failure.
    [[nodiscard]] static PcapSource open_offline(std::string_view file_path);

    // Open a live NIC by its libpcap device name (e.g. \Device\NPF_{GUID}).
    // Requires Administrator privileges to capture. Throws on failure.
    [[nodiscard]] static PcapSource open_live(std::string_view device_name);

    // Fetch the next packet. The returned span is valid only until the next
    // call to next().
    [[nodiscard]] ReadResult next() noexcept;

    // Transmit a raw frame on the interface. Returns false on failure.
    // (Live handles only; sending on an offline source fails.)
    [[nodiscard]] bool send(std::span<const std::byte> frame) noexcept;

    // Snapshot the driver's capture counters (recv/drop/ifdrop). Returns a
    // CaptureCounters with valid==false if the handle has no stats (offline).
    [[nodiscard]] CaptureCounters stats() const noexcept;

private:
    explicit PcapSource(pcap* handle) noexcept : m_handle(handle) {}

    pcap* m_handle = nullptr;
};

// Print the available capture devices (name + description) to stdout.
// Throws std::runtime_error on failure.
void list_devices();

}  // namespace net
