#include "ecat/frame.hpp"

namespace ecat {

std::string_view to_string(Command c) noexcept {
    switch (c) {
        case Command::kNop:  return "NOP";
        case Command::kAprd: return "APRD";
        case Command::kApwr: return "APWR";
        case Command::kAprw: return "APRW";
        case Command::kFprd: return "FPRD";
        case Command::kFpwr: return "FPWR";
        case Command::kFprw: return "FPRW";
        case Command::kBrd:  return "BRD";
        case Command::kBwr:  return "BWR";
        case Command::kBrw:  return "BRW";
        case Command::kLrd:  return "LRD";
        case Command::kLwr:  return "LWR";
        case Command::kLrw:  return "LRW";
        case Command::kArmw: return "ARMW";
        case Command::kFrmw: return "FRMW";
    }
    return "?";
}

}  // namespace ecat
