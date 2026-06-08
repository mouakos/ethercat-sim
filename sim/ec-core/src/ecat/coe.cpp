#include "ecat/coe.hpp"

#include <cstdio>

#include "ecat/frame.hpp"  // detail::read_u16_le / write_u16_le
#include "ecat/telltale.hpp"  // TELLTALE: diagnostic counters (grep TELLTALE to remove)

namespace ecat::coe {

namespace {
bool g_logging = false;

void log_sdo(const char* op, std::uint16_t index, std::uint8_t sub, bool ok,
             std::uint32_t value_or_code, std::uint8_t bytes) noexcept {
    if (!g_logging) {
        return;
    }
    if (ok) {
        std::fprintf(stderr, "[coe] %s 0x%04x:%02x = 0x%x (%uB)\n", op,
                     static_cast<unsigned>(index), static_cast<unsigned>(sub),
                     static_cast<unsigned>(value_or_code), static_cast<unsigned>(bytes));
    } else {
        std::fprintf(stderr, "[coe] %s 0x%04x:%02x -> ABORT 0x%08x\n", op,
                     static_cast<unsigned>(index), static_cast<unsigned>(sub),
                     static_cast<unsigned>(value_or_code));
    }
}
}  // namespace

void set_logging(bool enabled) noexcept { g_logging = enabled; }

bool logging_enabled() noexcept { return g_logging; }

void ObjectDictionary::add(std::uint16_t index, std::uint8_t sub,
                           std::uint32_t value, std::uint8_t byte_size,
                           bool writable) {
    m_entries[key_of(index, sub)] = Entry{value, byte_size, writable};
}

const Entry* ObjectDictionary::find(std::uint16_t index,
                                    std::uint8_t sub) const noexcept {
    const auto it = m_entries.find(key_of(index, sub));
    return it == m_entries.end() ? nullptr : &it->second;
}

Entry* ObjectDictionary::find(std::uint16_t index, std::uint8_t sub) noexcept {
    const auto it = m_entries.find(key_of(index, sub));
    return it == m_entries.end() ? nullptr : &it->second;
}

bool ObjectDictionary::has_index(std::uint16_t index) const noexcept {
    const auto lo = m_entries.lower_bound(key_of(index, 0));
    return lo != m_entries.end() && (lo->first >> 8) == index;
}

std::vector<std::uint16_t> ObjectDictionary::indices() const {
    std::vector<std::uint16_t> out;
    for (const auto& [key, entry] : m_entries) {
        const auto idx = static_cast<std::uint16_t>(key >> 8);
        if (out.empty() || out.back() != idx) {
            out.push_back(idx);  // map is ordered, so equal indices are adjacent
        }
    }
    return out;
}

std::uint8_t ObjectDictionary::max_subindex(std::uint16_t index) const noexcept {
    std::uint8_t max_sub = 0;
    for (auto it = m_entries.lower_bound(key_of(index, 0));
         it != m_entries.end() && (it->first >> 8) == index; ++it) {
        max_sub = static_cast<std::uint8_t>(it->first & 0xFF);  // ordered -> last is highest
    }
    return max_sub;
}

namespace {

constexpr std::size_t kMbxHeaderLen = 6;        // ETG.1000.5 mailbox header
constexpr std::uint8_t kMbxTypeCoE = 0x03;      // mailbox type nibble = CoE
constexpr std::uint16_t kCoeSdoRequest = 0x02;  // CoE header service field
constexpr std::uint16_t kCoeSdoResponse = 0x03;
constexpr std::size_t kSdoOffset = kMbxHeaderLen + 2;  // after mailbox + CoE hdr

void put_u32_le(std::span<std::byte> b, std::size_t off, std::uint32_t v) noexcept {
    detail::write_u16_le(b, off, static_cast<std::uint16_t>(v & 0xFFFF));
    detail::write_u16_le(b, off + 2, static_cast<std::uint16_t>(v >> 16));
}

// Assemble a 16-byte response: mailbox header + CoE header (SDO Response) + the
// 8-byte SDO (control, index, sub, 4 data bytes). Returns the length, or 0 if
// `out` is too small.
std::size_t build(std::span<std::byte> out, std::uint8_t control,
                  std::uint16_t index, std::uint8_t sub, std::uint32_t data) noexcept {
    if (out.size() < kMaxResponseBytes) {
        return 0;
    }
    for (std::size_t i = 0; i < kMaxResponseBytes; ++i) {
        out[i] = std::byte{0};
    }
    detail::write_u16_le(out, 0, 10);  // mailbox data length: CoE(2) + SDO(8)
    out[5] = std::byte{kMbxTypeCoE};
    detail::write_u16_le(out, kMbxHeaderLen,
                         static_cast<std::uint16_t>(kCoeSdoResponse << 12));
    out[kSdoOffset] = std::byte{control};
    detail::write_u16_le(out, kSdoOffset + 1, index);
    out[kSdoOffset + 3] = std::byte{sub};
    put_u32_le(out, kSdoOffset + 4, data);
    return kMaxResponseBytes;
}

std::size_t make_abort(std::span<std::byte> out, std::uint16_t index,
                       std::uint8_t sub, std::uint32_t code) noexcept {
    telltale::bump(telltale::Signal::kSdoAbort);  // TELLTALE: an SDO was aborted
    return build(out, 0x80, index, sub, code);  // SDO command specifier = abort
}

// --- SDO Information service (CoE service 0x08): online OD browsing ---
// Lets a tool (TwinCAT's CoE-Online "Show Offline Data" unticked) discover the
// dictionary live: list the object indices, then each object's description and
// each entry's data type / access. Single-frame responses only (our OD is small
// enough to fit the 128-byte mailbox); segmentation is a later step.
constexpr std::uint16_t kCoeSdoInformation = 0x08;
constexpr std::uint8_t kInfoGetListReq = 0x01;
constexpr std::uint8_t kInfoGetListResp = 0x02;
constexpr std::uint8_t kInfoGetObjReq = 0x03;
constexpr std::uint8_t kInfoGetObjResp = 0x04;
constexpr std::uint8_t kInfoGetEntryReq = 0x05;
constexpr std::uint8_t kInfoGetEntryResp = 0x06;
// Payload begins after the mailbox header (6) + CoE header (2) + SDO-Info header
// (opcode 1, reserved 1, fragments-left 2 = 4).
constexpr std::size_t kInfoPayload = kMbxHeaderLen + 2 + 4;  // = 12

// CoE basic data-type codes (ETG.1000.6) for a 1/2/4-byte value.
std::uint16_t coe_datatype(std::uint8_t byte_size) noexcept {
    switch (byte_size) {
        case 1: return 0x0005;  // UNSIGNED8
        case 2: return 0x0006;  // UNSIGNED16
        case 4: return 0x0007;  // UNSIGNED32
        default: return 0x0000;
    }
}

// Frame an SDO-Information response header (mailbox + CoE service 8 + opcode).
void info_headers(std::span<std::byte> out, std::uint8_t resp_opcode) noexcept {
    for (std::size_t i = 0; i < kInfoPayload; ++i) {
        out[i] = std::byte{0};
    }
    out[5] = std::byte{kMbxTypeCoE};
    detail::write_u16_le(out, kMbxHeaderLen,
                         static_cast<std::uint16_t>(kCoeSdoInformation << 12));
    out[kMbxHeaderLen + 2] = std::byte{resp_opcode};  // opcode, incomplete bit clear
    // byte +3 reserved, bytes +4..+5 fragments-left = 0 (already zeroed)
}

// Set the mailbox length field (CoE hdr 2 + SDO-Info hdr 4 + payload) and return
// the total response length.
std::size_t info_finalize(std::span<std::byte> out, std::size_t payload_len) noexcept {
    detail::write_u16_le(out, 0, static_cast<std::uint16_t>(2 + 4 + payload_len));
    return kInfoPayload + payload_len;
}

std::size_t process_sdo_info(ObjectDictionary& od, std::span<const std::byte> req,
                             std::span<std::byte> out) noexcept {
    if (req.size() < kInfoPayload || out.size() < kMaxMailboxResponse) {
        return 0;
    }
    const std::uint8_t opcode =
        static_cast<std::uint8_t>(std::to_integer<std::uint8_t>(req[kMbxHeaderLen + 2]) & 0x7F);

    if (opcode == kInfoGetListReq) {  // GetODList
        const std::uint16_t list_type = detail::read_u16_le(req, kInfoPayload);
        const auto idx = od.indices();
        info_headers(out, kInfoGetListResp);
        detail::write_u16_le(out, kInfoPayload, list_type);
        std::size_t p = 2;
        if (list_type == 0) {  // "lengths": object count per category (all, then 0s)
            for (int i = 0; i < 5 && kInfoPayload + p + 2 <= out.size(); ++i) {
                detail::write_u16_le(out, kInfoPayload + p,
                                     i == 0 ? static_cast<std::uint16_t>(idx.size()) : 0);
                p += 2;
            }
        } else if (list_type == 1) {  // all objects
            for (const std::uint16_t i : idx) {
                if (kInfoPayload + p + 2 > out.size()) {
                    break;  // would overflow the mailbox (no segmentation yet)
                }
                detail::write_u16_le(out, kInfoPayload + p, i);
                p += 2;
            }
        }  // other list types (PDO-mappable/backup/settings): empty list
        if (g_logging) {
            std::fprintf(stderr, "[coe] SDO-Info GetODList type %u -> %zu indices\n",
                         static_cast<unsigned>(list_type),
                         list_type == 1 ? idx.size() : std::size_t{0});
        }
        return info_finalize(out, p);
    }

    if (opcode == kInfoGetObjReq) {  // GetObjectDescription
        const std::uint16_t index = detail::read_u16_le(req, kInfoPayload);
        const std::uint8_t max_sub = od.max_subindex(index);
        const std::uint8_t object_code = (max_sub == 0) ? 0x07 : 0x08;  // VAR : ARRAY
        const Entry* type_entry = od.find(index, max_sub == 0 ? 0 : 1);
        const std::uint16_t dtype = type_entry ? coe_datatype(type_entry->byte_size) : 0x0000;
        info_headers(out, kInfoGetObjResp);
        detail::write_u16_le(out, kInfoPayload + 0, index);
        detail::write_u16_le(out, kInfoPayload + 2, dtype);
        out[kInfoPayload + 4] = std::byte{max_sub};
        out[kInfoPayload + 5] = std::byte{object_code};
        // name omitted (TwinCAT shows the index)
        if (g_logging) {
            std::fprintf(stderr,
                         "[coe] SDO-Info GetObjDesc 0x%04x -> maxsub %u code %u type 0x%04x\n",
                         static_cast<unsigned>(index), static_cast<unsigned>(max_sub),
                         static_cast<unsigned>(object_code), static_cast<unsigned>(dtype));
        }
        return info_finalize(out, 6);
    }

    if (opcode == kInfoGetEntryReq) {  // GetEntryDescription
        const std::uint16_t index = detail::read_u16_le(req, kInfoPayload);
        const std::uint8_t sub = std::to_integer<std::uint8_t>(req[kInfoPayload + 2]);
        const std::uint8_t value_info = std::to_integer<std::uint8_t>(req[kInfoPayload + 3]);
        const Entry* e = od.find(index, sub);
        const std::uint16_t dtype = e ? coe_datatype(e->byte_size) : 0x0000;
        const std::uint16_t bitlen = e ? static_cast<std::uint16_t>(e->byte_size * 8) : 0;
        // ObjectAccess: read in PreOp/SafeOp/Op (0x07); + write (0x38) if writable.
        std::uint16_t access = 0x0007;
        if (e && e->writable) {
            access = static_cast<std::uint16_t>(access | 0x0038);
        }
        info_headers(out, kInfoGetEntryResp);
        detail::write_u16_le(out, kInfoPayload + 0, index);
        out[kInfoPayload + 2] = std::byte{sub};
        out[kInfoPayload + 3] = std::byte{0};  // ValueInfo: only the description follows
        detail::write_u16_le(out, kInfoPayload + 4, dtype);
        detail::write_u16_le(out, kInfoPayload + 6, bitlen);
        detail::write_u16_le(out, kInfoPayload + 8, access);
        // name omitted
        if (g_logging) {
            std::fprintf(stderr,
                         "[coe] SDO-Info GetEntryDesc 0x%04x:%02x (vi %u) -> bits %u access 0x%04x\n",
                         static_cast<unsigned>(index), static_cast<unsigned>(sub),
                         static_cast<unsigned>(value_info), static_cast<unsigned>(bitlen),
                         static_cast<unsigned>(access));
        }
        return info_finalize(out, 10);
    }

    telltale::bump(telltale::Signal::kCoeUnhandled);  // TELLTALE: SDO-Info opcode we don't implement
    if (g_logging) {
        std::fprintf(stderr, "[coe] SDO-Info opcode 0x%02x not handled\n",
                     static_cast<unsigned>(opcode));
    }
    return 0;  // unsupported opcode -> no response
}

}  // namespace

std::size_t process_mailbox(ObjectDictionary& od, std::span<const std::byte> req,
                            std::span<std::byte> out) noexcept {
    // Need at least the mailbox header to read the type.
    if (req.size() < kMbxHeaderLen) {
        telltale::bump(telltale::Signal::kMailboxMalformed);  // TELLTALE: request shorter than the mailbox header
        return 0;
    }
    if ((std::to_integer<std::uint8_t>(req[5]) & 0x0F) != kMbxTypeCoE) {
        telltale::bump(telltale::Signal::kCoeUnhandled);  // TELLTALE: non-CoE mailbox protocol
        return 0;  // not a CoE mailbox
    }
    // Need the CoE header + SDO control/index/sub (data read defensively below).
    if (req.size() < kSdoOffset + 4) {
        telltale::bump(telltale::Signal::kMailboxMalformed);  // TELLTALE: CoE request truncated
        return 0;
    }
    const std::uint16_t coe = detail::read_u16_le(req, kMbxHeaderLen);
    const std::uint16_t service = (coe >> 12) & 0x0F;
    if (service == kCoeSdoInformation) {  // online OD browsing (CoE-Online)
        return process_sdo_info(od, req, out);
    }
    if (service != kCoeSdoRequest) {
        telltale::bump(telltale::Signal::kCoeUnhandled);  // TELLTALE: CoE service we don't implement (e.g. EMCY/PDO)
        return 0;  // only SDO requests and the SDO Information service are serviced
    }

    const std::uint8_t control = std::to_integer<std::uint8_t>(req[kSdoOffset]);
    const std::uint16_t index = detail::read_u16_le(req, kSdoOffset + 1);
    const std::uint8_t sub = std::to_integer<std::uint8_t>(req[kSdoOffset + 3]);
    const std::uint8_t ccs = (control >> 5) & 0x07;

    if (ccs == 2) {  // SDO upload (master reads an object)
        const Entry* e = od.find(index, sub);
        if (e == nullptr) {
            const std::uint32_t code =
                od.has_index(index) ? kAbortNoSuchSubIndex : kAbortNoSuchObject;
            log_sdo("upload", index, sub, false, code, 0);
            return make_abort(out, index, sub, code);
        }
        const std::uint8_t n = static_cast<std::uint8_t>(4 - e->byte_size);
        // scs=2, expedited (e=1), size-indicated (s=1), n unused bytes in bits 2-3.
        const std::uint8_t resp = static_cast<std::uint8_t>(0x43 | (n << 2));
        log_sdo("upload", index, sub, true, e->value, e->byte_size);
        return build(out, resp, index, sub, e->value);
    }
    if (ccs == 1) {  // SDO download (master writes an object)
        Entry* e = od.find(index, sub);
        if (e == nullptr) {
            const std::uint32_t code =
                od.has_index(index) ? kAbortNoSuchSubIndex : kAbortNoSuchObject;
            log_sdo("download", index, sub, false, code, 0);
            return make_abort(out, index, sub, code);
        }
        if (!e->writable) {
            log_sdo("download", index, sub, false, kAbortReadOnly, 0);
            return make_abort(out, index, sub, kAbortReadOnly);
        }
        std::uint32_t data = 0;
        for (std::uint8_t i = 0; i < e->byte_size; ++i) {
            const std::size_t off = kSdoOffset + 4 + i;
            if (off < req.size()) {
                data |= static_cast<std::uint32_t>(
                            std::to_integer<std::uint8_t>(req[off]))
                        << (8 * i);
            }
        }
        if (e->byte_size < 4) {
            data &= (1u << (8 * e->byte_size)) - 1u;
        }
        e->value = data;
        log_sdo("download", index, sub, true, data, e->byte_size);
        return build(out, 0x60, index, sub, 0);  // scs=3, download response
    }
    return make_abort(out, index, sub, kAbortBadCommand);
}

}  // namespace ecat::coe
