#include "net/pcap_source.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

#include <pcap.h>

namespace net {

PcapSource::PcapSource(PcapSource&& other) noexcept : m_handle(other.m_handle) {
    other.m_handle = nullptr;
}

PcapSource& PcapSource::operator=(PcapSource&& other) noexcept {
    if (this != &other) {
        if (m_handle != nullptr) {
            pcap_close(m_handle);
        }
        m_handle = other.m_handle;
        other.m_handle = nullptr;
    }
    return *this;
}

PcapSource::~PcapSource() {
    if (m_handle != nullptr) {
        pcap_close(m_handle);
    }
}

PcapSource PcapSource::open_offline(std::string_view file_path) {
    char errbuf[PCAP_ERRBUF_SIZE] = {};
    const std::string path(file_path);
    pcap* handle = pcap_open_offline(path.c_str(), errbuf);
    if (handle == nullptr) {
        throw std::runtime_error("pcap_open_offline(" + path + ") failed: " + errbuf);
    }
    return PcapSource(handle);
}

PcapSource PcapSource::open_live(std::string_view device_name) {
    char errbuf[PCAP_ERRBUF_SIZE] = {};
    const std::string device(device_name);

    // Use the create/activate API so we can enable immediate mode: deliver each
    // packet to userspace as soon as it arrives instead of buffering, which is
    // essential for low response latency to the master.
    pcap* handle = pcap_create(device.c_str(), errbuf);
    if (handle == nullptr) {
        throw std::runtime_error("pcap_create(" + device + ") failed: " + errbuf);
    }
    pcap_set_snaplen(handle, 65536);
    pcap_set_promisc(handle, 1);
    pcap_set_timeout(handle, 1);          // 1 ms read timeout
    pcap_set_immediate_mode(handle, 1);   // no buffering — hand up packets ASAP
    const int rc = pcap_activate(handle);
    if (rc < 0) {
        const std::string err = pcap_geterr(handle);
        pcap_close(handle);
        throw std::runtime_error("pcap_activate(" + device + ") failed: " + err);
    }
    // Capture inbound frames only, so we never re-capture the frames we send
    // back to the master (which would otherwise loop). Non-fatal if unsupported.
    if (pcap_setdirection(handle, PCAP_D_IN) != 0) {
        std::fprintf(stderr, "warning: pcap_setdirection(PCAP_D_IN) failed: %s\n",
                     pcap_geterr(handle));
    }
    return PcapSource(handle);
}

ReadResult PcapSource::next() noexcept {
    pcap_pkthdr* header = nullptr;
    const u_char* data = nullptr;
    const int rc = pcap_next_ex(m_handle, &header, &data);
    switch (rc) {
        case 1:
            return {ReadStatus::kPacket,
                    std::span<const std::byte>(
                        reinterpret_cast<const std::byte*>(data), header->caplen)};
        case 0:
            return {ReadStatus::kTimeout, {}};
        case PCAP_ERROR_BREAK:  // -2: offline capture exhausted
            return {ReadStatus::kEndOfFile, {}};
        default:
            return {ReadStatus::kError, {}};
    }
}

bool PcapSource::send(std::span<const std::byte> frame) noexcept {
    return pcap_sendpacket(m_handle,
                           reinterpret_cast<const u_char*>(frame.data()),
                           static_cast<int>(frame.size())) == 0;
}

CaptureCounters PcapSource::stats() const noexcept {
    pcap_stat ps{};
    if (m_handle == nullptr || pcap_stats(m_handle, &ps) != 0) {
        return {};  // valid == false
    }
    return {ps.ps_recv, ps.ps_drop, ps.ps_ifdrop, true};
}

void list_devices() {
    char errbuf[PCAP_ERRBUF_SIZE] = {};
    pcap_if_t* devices = nullptr;
    if (pcap_findalldevs(&devices, errbuf) != 0) {
        throw std::runtime_error(std::string("pcap_findalldevs failed: ") + errbuf);
    }
    for (const pcap_if_t* d = devices; d != nullptr; d = d->next) {
        std::printf("%s\n", d->name);
        if (d->description != nullptr) {
            std::printf("    %s\n", d->description);
        }
    }
    pcap_freealldevs(devices);
}

}  // namespace net
