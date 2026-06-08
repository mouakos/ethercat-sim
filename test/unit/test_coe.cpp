// Unit tests for the CoE SDO engine (ecat::coe): Object Dictionary access and
// expedited SDO upload/download/abort over a mailbox buffer.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "ecat/coe.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s: %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) {
        ++g_failures;
    }
}

// Build a 16-byte CoE SDO-request mailbox frame.
std::vector<std::byte> sdo_request(std::uint8_t control, std::uint16_t index,
                                   std::uint8_t sub, std::uint32_t data) {
    std::vector<std::byte> f(16, std::byte{0});
    auto put8 = [&](std::size_t o, unsigned v) { f[o] = static_cast<std::byte>(v & 0xFF); };
    auto put16 = [&](std::size_t o, unsigned v) { put8(o, v); put8(o + 1, v >> 8); };
    put16(0, 10);          // mailbox data length: CoE(2) + SDO(8)
    put8(5, 0x03);         // mailbox type = CoE
    put16(6, 0x02u << 12); // CoE header: SDO request
    put8(8, control);
    put16(9, index);
    put8(11, sub);
    put8(12, data); put8(13, data >> 8); put8(14, data >> 16); put8(15, data >> 24);
    return f;
}

std::uint8_t r_control(std::span<const std::byte> o) { return std::to_integer<std::uint8_t>(o[8]); }
std::uint16_t r_index(std::span<const std::byte> o) {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(o[9]) |
                                      (std::to_integer<unsigned>(o[10]) << 8));
}
std::uint8_t r_sub(std::span<const std::byte> o) { return std::to_integer<std::uint8_t>(o[11]); }
std::uint32_t r_data(std::span<const std::byte> o) {
    return std::to_integer<std::uint32_t>(o[12]) |
           (std::to_integer<std::uint32_t>(o[13]) << 8) |
           (std::to_integer<std::uint32_t>(o[14]) << 16) |
           (std::to_integer<std::uint32_t>(o[15]) << 24);
}

// Request command bytes (master side).
constexpr std::uint8_t kUpload = 0x40;       // ccs = 2
constexpr std::uint8_t kDownload4 = 0x23;    // ccs = 1, 4 bytes
constexpr std::uint8_t kDownload2 = 0x2B;    // ccs = 1, 2 bytes
constexpr std::uint8_t kDownload1 = 0x2F;    // ccs = 1, 1 byte

// Build an SDO-Information request mailbox frame (CoE service 0x08). `a` is the
// payload's first word (list type, or object index); `b`/`c` are the sub-index
// and value-info used by GetEntryDescription.
std::vector<std::byte> sdo_info_request(std::uint8_t opcode, std::uint16_t a,
                                        std::uint8_t b = 0, std::uint8_t c = 0) {
    std::vector<std::byte> f(20, std::byte{0});
    auto put8 = [&](std::size_t o, unsigned v) { f[o] = static_cast<std::byte>(v & 0xFF); };
    auto put16 = [&](std::size_t o, unsigned v) { put8(o, v); put8(o + 1, v >> 8); };
    put16(0, 8);            // mailbox data length: CoE(2) + SDO-Info hdr(4) + payload(2)
    put8(5, 0x03);          // mailbox type = CoE
    put16(6, 0x08u << 12);  // CoE header: SDO Information service
    put8(8, opcode);        // SDO-Info opcode (1=list, 3=obj desc, 5=entry desc)
    put16(12, a);           // payload word 0: list type / object index
    put8(14, b);            // sub-index (entry desc)
    put8(15, c);            // value info (entry desc)
    return f;
}

std::uint8_t r_info_opcode(std::span<const std::byte> o) { return std::to_integer<std::uint8_t>(o[8]); }
// Read payload word `w` (payload starts at byte 12).
std::uint16_t r_info_w(std::span<const std::byte> o, std::size_t w) {
    const std::size_t off = 12 + w * 2;
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(o[off]) |
                                      (std::to_integer<unsigned>(o[off + 1]) << 8));
}

}  // namespace

int main() {
    using namespace ecat::coe;

    // --- Object Dictionary basics ---
    {
        ObjectDictionary od;
        od.add(0x1018, 0, 4, 1, false);            // identity: #entries = 4
        od.add(0x1018, 1, 0x00000002, 4, false);   // vendor id (Beckhoff)
        od.add(0x8000, 0x12, 0, 2, true);          // a writable 16-bit setting
        check(od.find(0x1018, 1) != nullptr && od.find(0x1018, 1)->value == 2,
              "OD find returns the stored value");
        check(od.find(0x1018, 9) == nullptr, "OD find misses an absent sub-index");
        check(od.has_index(0x1018) && od.has_index(0x8000), "has_index true for present");
        check(!od.has_index(0x9999), "has_index false for absent");
    }

    // --- SDO upload (read): widths produce the right response control byte ---
    {
        ObjectDictionary od;
        od.add(0x1018, 0, 4, 1, false);
        od.add(0x1018, 1, 0x00000002, 4, false);
        od.add(0x6000, 1, 0xBEEF, 2, false);
        std::array<std::byte, 16> out{};

        auto up4 = sdo_request(kUpload, 0x1018, 1, 0);
        check(process_mailbox(od, up4, out) == 16, "4-byte upload serviced");
        check(r_control(out) == 0x43, "4-byte upload response control = 0x43");
        check(r_index(out) == 0x1018 && r_sub(out) == 1, "upload echoes index/sub");
        check(r_data(out) == 0x00000002, "upload returns the vendor id");

        auto up2 = sdo_request(kUpload, 0x6000, 1, 0);
        check(process_mailbox(od, up2, out) == 16, "2-byte upload serviced");
        check(r_control(out) == 0x4B, "2-byte upload response control = 0x4B");
        check(r_data(out) == 0xBEEF, "2-byte upload returns the value");

        auto up1 = sdo_request(kUpload, 0x1018, 0, 0);
        check(process_mailbox(od, up1, out) == 16, "1-byte upload serviced");
        check(r_control(out) == 0x4F, "1-byte upload response control = 0x4F");
        check(r_data(out) == 4, "1-byte upload returns #entries = 4");
    }

    // --- SDO upload aborts: missing object vs missing sub-index ---
    {
        ObjectDictionary od;
        od.add(0x1018, 1, 0x00000002, 4, false);
        std::array<std::byte, 16> out{};

        auto miss_obj = sdo_request(kUpload, 0x3333, 0, 0);
        check(process_mailbox(od, miss_obj, out) == 16, "missing-object upload returns an abort");
        check(r_control(out) == 0x80, "abort uses SDO command 0x80");
        check(r_data(out) == kAbortNoSuchObject, "abort code = no such object");

        auto miss_sub = sdo_request(kUpload, 0x1018, 7, 0);
        check(process_mailbox(od, miss_sub, out) == 16, "missing-sub upload returns an abort");
        check(r_data(out) == kAbortNoSuchSubIndex, "abort code = no such sub-index");
    }

    // --- SDO download (write): updates the OD; read-back confirms ---
    {
        ObjectDictionary od;
        od.add(0x8000, 0x12, 0, 2, true);   // writable 16-bit
        od.add(0x1018, 1, 0x02, 4, false);  // read-only
        std::array<std::byte, 16> out{};

        auto dn = sdo_request(kDownload2, 0x8000, 0x12, 0x1234);
        check(process_mailbox(od, dn, out) == 16, "download serviced");
        check(r_control(out) == 0x60, "download response control = 0x60");
        check(od.find(0x8000, 0x12)->value == 0x1234, "OD updated by download");

        auto rb = sdo_request(kUpload, 0x8000, 0x12, 0);
        (void)process_mailbox(od, rb, out);
        check(r_control(out) == 0x4B && r_data(out) == 0x1234, "read-back returns written value");

        // 1-byte write only stores the low byte.
        od.add(0x8001, 0, 0, 1, true);
        auto dn1 = sdo_request(kDownload1, 0x8001, 0, 0xAB);
        (void)process_mailbox(od, dn1, out);
        check(od.find(0x8001, 0)->value == 0xAB, "1-byte download stores the low byte");

        // Writing a read-only object aborts.
        auto ro = sdo_request(kDownload4, 0x1018, 1, 0x99);
        check(process_mailbox(od, ro, out) == 16, "read-only download returns an abort");
        check(r_control(out) == 0x80 && r_data(out) == kAbortReadOnly,
              "abort code = write to read-only object");
    }

    // --- Non-CoE mailbox traffic is ignored (no response) ---
    {
        ObjectDictionary od;
        od.add(0x1018, 1, 0x02, 4, false);
        std::array<std::byte, 16> out{};

        auto eoe = sdo_request(kUpload, 0x1018, 1, 0);
        eoe[5] = std::byte{0x02};  // mailbox type = EoE, not CoE
        check(process_mailbox(od, eoe, out) == 0, "non-CoE mailbox produces no response");
    }

    // --- SDO Information service: online OD browsing (CoE-Online) ---
    {
        ObjectDictionary od;
        od.add(0x1000, 0, 0x1389, 4, false);        // VAR (single value)
        od.add(0x1018, 0, 4, 1, false);             // identity record: #entries
        od.add(0x1018, 1, 0x02, 4, false);          //   vendor id
        od.add(0x1018, 2, 0x0BB93052, 4, false);    //   product code
        od.add(0x8000, 0x11, 0, 2, true);           // writable 16-bit setting
        std::array<std::byte, kMaxMailboxResponse> out{};

        // GetODList(type 1) -> the three distinct indices, ascending.
        auto list = sdo_info_request(0x01, 0x0001);
        check(process_mailbox(od, list, out) > 0, "SDO-Info GetODList serviced");
        check(r_info_opcode(out) == 0x02, "GetODList response opcode = 2");
        check(r_info_w(out, 0) == 0x0001, "GetODList echoes list type 1");
        check(r_info_w(out, 1) == 0x1000 && r_info_w(out, 2) == 0x1018 &&
                  r_info_w(out, 3) == 0x8000,
              "GetODList returns the object indices, ascending");

        // GetObjectDescription(0x1018) -> ARRAY, max sub-index 2.
        auto obj = sdo_info_request(0x03, 0x1018);
        check(process_mailbox(od, obj, out) > 0, "SDO-Info GetObjDesc serviced");
        check(r_info_opcode(out) == 0x04, "GetObjDesc response opcode = 4");
        check(r_info_w(out, 0) == 0x1018, "GetObjDesc echoes the index");
        check(std::to_integer<unsigned>(out[16]) == 2, "GetObjDesc max sub-index = 2");
        check(std::to_integer<unsigned>(out[17]) == 0x08, "GetObjDesc object code = ARRAY");

        // GetObjectDescription(0x1000) -> VAR, max sub-index 0.
        auto obj0 = sdo_info_request(0x03, 0x1000);
        (void)process_mailbox(od, obj0, out);
        check(std::to_integer<unsigned>(out[16]) == 0, "single-value object max sub = 0");
        check(std::to_integer<unsigned>(out[17]) == 0x07, "single-value object code = VAR");

        // GetEntryDescription(0x8000:11) -> writable 16-bit (RW access).
        auto ent = sdo_info_request(0x05, 0x8000, 0x11, 0);
        check(process_mailbox(od, ent, out) > 0, "SDO-Info GetEntryDesc serviced");
        check(r_info_opcode(out) == 0x06, "GetEntryDesc response opcode = 6");
        check(r_info_w(out, 2) == 0x0006, "entry data type = UNSIGNED16");
        check(r_info_w(out, 3) == 16, "entry bit length = 16");
        check(r_info_w(out, 4) == 0x003F, "writable entry access = RW (0x3F)");

        // GetEntryDescription of a read-only entry -> access 0x07.
        auto ro = sdo_info_request(0x05, 0x1018, 1, 0);
        (void)process_mailbox(od, ro, out);
        check(r_info_w(out, 4) == 0x0007, "read-only entry access = RO (0x07)");

        // Unsupported SDO-Info opcode -> no response.
        auto bad = sdo_info_request(0x10, 0x0000);
        check(process_mailbox(od, bad, out) == 0, "unsupported SDO-Info opcode ignored");
    }

    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
