#include "ecat/station.hpp"

namespace ecat {

void stamp_source_mac(std::span<std::byte> frame) noexcept {
    if (frame.size() < 12) {
        return;
    }
    for (std::size_t i = 0; i < kSlaveMac.size(); ++i) {
        frame[6 + i] = static_cast<std::byte>(kSlaveMac[i]);
    }
}

bool is_own_transmission(std::span<const std::byte> frame) noexcept {
    if (frame.size() < 12) {
        return false;
    }
    for (std::size_t i = 0; i < kSlaveMac.size(); ++i) {
        if (std::to_integer<std::uint8_t>(frame[6 + i]) != kSlaveMac[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace ecat
