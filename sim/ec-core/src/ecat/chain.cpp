#include "ecat/chain.hpp"

#include <algorithm>
#include <cstdio>

#include "ecat/aoe.hpp"      // AoE (ADS over EtherCAT) mailbox for the EL6224
#include "ecat/coe.hpp"      // process_mailbox, logging_enabled
#include "ecat/frame.hpp"
#include "ecat/station.hpp"  // AlState, kAlStatusRegister
#include "ecat/telltale.hpp"  // TELLTALE: diagnostic counters (grep TELLTALE to remove)

namespace ecat {

std::uint16_t Esc::get_u16(std::uint16_t addr) const noexcept {
    return detail::read_u16_le(std::span<const std::byte>(registers), addr);
}

void Esc::set_u16(std::uint16_t addr, std::uint16_t value) noexcept {
    detail::write_u16_le(std::span<std::byte>(registers), addr, value);
}

std::uint32_t Esc::get_u32(std::uint16_t addr) const noexcept {
    return detail::read_u32_le(std::span<const std::byte>(registers), addr);
}

void Esc::set_u32(std::uint16_t addr, std::uint32_t value) noexcept {
    detail::write_u16_le(std::span<std::byte>(registers), addr,
                         static_cast<std::uint16_t>(value & 0xFFFF));
    detail::write_u16_le(std::span<std::byte>(registers),
                         static_cast<std::uint16_t>(addr + 2),
                         static_cast<std::uint16_t>(value >> 16));
}

void Esc::set_u8(std::uint16_t addr, std::uint8_t value) noexcept {
    registers[addr] = static_cast<std::byte>(value);
}

namespace {

enum class Addressing { kNone, kBroadcast, kAutoIncrement, kConfigured, kLogical };

Addressing addressing_of(Command c) noexcept {
    switch (c) {
        case Command::kAprd:
        case Command::kApwr:
        case Command::kAprw:
            return Addressing::kAutoIncrement;
        case Command::kFprd:
        case Command::kFpwr:
        case Command::kFprw:
            return Addressing::kConfigured;
        case Command::kBrd:
        case Command::kBwr:
        case Command::kBrw:
            return Addressing::kBroadcast;
        case Command::kLrd:
        case Command::kLwr:
        case Command::kLrw:
            return Addressing::kLogical;
        default:
            // NOP and the DC read-multiple-write commands are not serviced yet.
            return Addressing::kNone;
    }
}

bool is_read(Command c) noexcept {
    switch (c) {
        case Command::kAprd:
        case Command::kAprw:
        case Command::kFprd:
        case Command::kFprw:
        case Command::kBrd:
        case Command::kBrw:
        case Command::kLrd:
        case Command::kLrw:
            return true;
        default:
            return false;
    }
}

bool is_write(Command c) noexcept {
    switch (c) {
        case Command::kApwr:
        case Command::kAprw:
        case Command::kFpwr:
        case Command::kFprw:
        case Command::kBwr:
        case Command::kBrw:
        case Command::kLwr:
        case Command::kLrw:
            return true;
        default:
            return false;
    }
}

constexpr std::uint16_t kStationAddressRegister = 0x0010;
constexpr std::uint16_t kAlControlRegister = 0x0120;
constexpr std::uint16_t kEepromControlRegister = 0x0502;  // control/status
constexpr std::uint16_t kEepromAddressRegister = 0x0504;  // word address (lo)
constexpr std::uint16_t kEepromDataRegister = 0x0508;     // read result
constexpr std::uint16_t kSm0Config = 0x0800;  // SyncManager 0 = mailbox out (master->slave)
constexpr std::uint16_t kSm0Status = 0x0805;  // SM0 status byte (mailbox-out full/empty)
constexpr std::uint16_t kSm1Config = 0x0808;  // SyncManager 1 = mailbox in (slave->master)
constexpr std::uint16_t kSm1Status = 0x080D;  // SM1 status byte
constexpr std::uint8_t kMailboxFull = 0x08;   // SM status bit 3: mailbox written / data ready
constexpr std::uint16_t kAlEventRequest = 0x0220;     // AL Event Request (32-bit)
constexpr std::uint32_t kSm1EventBit = 0x00000200u;   // bit 9 = SyncManager 1 (mailbox-in) event

// Execute a pending EEPROM read: the master has written the read command
// (0x0100) to the control register plus a word address. Load the addressed SII
// words into the data register so the master's next read returns them. We leave
// the busy bit clear (instant read) and report no error.
void run_eeprom_command(Esc& esc) noexcept {
    const std::uint16_t control = esc.get_u16(kEepromControlRegister);
    constexpr std::uint16_t kCommandMask = 0x0700;  // bits 8-10
    constexpr std::uint16_t kReadCommand = 0x0100;  // 001 = read
    if ((control & kCommandMask) != kReadCommand) {
        return;
    }
    const std::uint16_t word_addr = esc.get_u16(kEepromAddressRegister);
    // ESC EEPROM data register is up to 8 bytes (4 words) per access.
    for (std::uint16_t i = 0; i < 4; ++i) {
        const std::uint16_t a = static_cast<std::uint16_t>(word_addr + i);
        const std::uint16_t value = (a < esc.eeprom.size()) ? esc.eeprom[a] : 0;
        esc.set_u16(static_cast<std::uint16_t>(kEepromDataRegister + 2 * i), value);
    }
    // Acknowledge: keep the read command echo, busy (bit 15) and error bits
    // clear, so the master sees the access complete with no checksum error.
    esc.set_u16(kEepromControlRegister, kReadCommand);
}

// Service a CoE mailbox request the master just wrote into the mailbox-out
// SyncManager (SM0). The SM0/SM1 buffer addresses are read straight from the SM
// config registers (the master programs them in PreOp). Parse the request from
// SM0, run it against this slave's Object Dictionary, stage the response in the
// mailbox-in SyncManager (SM1), and set SM1's "mailbox full" status so the
// master's next read collects it. We consume the request synchronously, so SM0
// is left "empty" — the master is always free to post the next request.
void run_mailbox(Esc& esc) noexcept {
    const std::uint16_t out_addr = esc.get_u16(kSm0Config);
    const std::uint16_t out_len = esc.get_u16(kSm0Config + 2);
    const std::uint16_t in_addr = esc.get_u16(kSm1Config);
    if (out_addr == 0 || out_len == 0 || in_addr == 0) {
        telltale::bump(telltale::Signal::kSmUnconfigured);  // TELLTALE: mailbox traffic before SMs configured
        return;  // mailbox SMs not configured yet
    }
    if (static_cast<std::size_t>(out_addr) + out_len > Esc::kRegisterSpace) {
        telltale::bump(telltale::Signal::kBoundsGuard);  // TELLTALE: SM0 window out of range
        return;
    }
    telltale::bump(telltale::Signal::kMailboxRequest);  // TELLTALE: a mailbox request is being serviced
    // SM0 (mailbox-out) full/empty handshake. A real ESC sets SM0's "full" status
    // bit (0x0805.3) the instant the master finishes writing the request buffer,
    // and clears it once the PDI/application consumes the request. TwinCAT's cyclic
    // mailbox state machine watches this transition ("did the slave take my
    // request?") before it collects the SM1 reply — so without it the optimized
    // bring-up can stall at PreOp->SafeOp. We consume synchronously, so we set
    // "full" on arrival and clear it immediately after parsing (see docs/coe-safeop-brief.md,
    // SM0-handshake hypothesis). NOTE: this is permanent device modelling, not a
    // tell-tale — do not strip it with the TELLTALE counters.
    esc.set_u8(kSm0Status, static_cast<std::uint8_t>(
                               std::to_integer<std::uint8_t>(esc.registers[kSm0Status]) |
                               kMailboxFull));
    const std::span<const std::byte> request(esc.registers.data() + out_addr, out_len);
    std::array<std::byte, coe::kMaxMailboxResponse> response{};
    // Dispatch by mailbox protocol type (header byte 5, low nibble): AoE (1) for
    // the EL6224 IO-Link master's ADS channel (NetId download, later ISDU), CoE
    // (3) for SDOs. Other types fall through to the CoE engine, which returns 0.
    const std::uint8_t mbx_type = static_cast<std::uint8_t>(
        std::to_integer<std::uint8_t>(esc.registers[out_addr + 5]) & 0x0F);
    const std::size_t n = (mbx_type == aoe::kMailboxType)
                              ? aoe::process_mailbox(request, response)
                              : coe::process_mailbox(esc.od, request, response);
    // Request consumed by the PDI/application: SM0 is empty again (whether or not
    // we produced a reply), so the master is free to post the next request.
    esc.set_u8(kSm0Status, static_cast<std::uint8_t>(
                               std::to_integer<std::uint8_t>(esc.registers[kSm0Status]) &
                               ~kMailboxFull));
    if (n == 0) {
        return;  // not a CoE SDO we service — leave the mailbox untouched
    }
    if (static_cast<std::size_t>(in_addr) + n > Esc::kRegisterSpace) {
        telltale::bump(telltale::Signal::kBoundsGuard);  // TELLTALE: SM1 reply would overrun
        return;
    }
    // Mailbox sequence counter (ETG.1000.4): echo the request's 3-bit counter
    // (header byte 5, bits 4..6) into the reply. This is the AC-05-verified PreOp
    // behaviour: TwinCAT's simple fixed-cycle mailbox poll pairs a reply to its
    // request by this counter. The SafeOp investigation tried other schemes here
    // (constant 0; the slave's own incrementing counter) to get the optimized
    // cyclic path to accept consecutive replies, but none reached SafeOp, so we keep
    // the proven echo and leave the counter analysis to docs/coe-safeop-brief.md.
    const std::uint8_t req_counter = static_cast<std::uint8_t>(
        std::to_integer<std::uint8_t>(esc.registers[out_addr + 5]) & 0x70);
    response[5] = std::byte{static_cast<std::uint8_t>(
        (std::to_integer<std::uint8_t>(response[5]) & 0x0F) | req_counter)};
    // TELLTALE: a request that reuses the previous (non-zero) counter is a master
    // retry — it rejected our last reply. (Counter 0 = "not used" -> skip.)
    if (req_counter != 0 && req_counter == esc.mbx_last_req_counter) {
        telltale::bump(telltale::Signal::kRequestRetry);
    }
    esc.mbx_last_req_counter = req_counter;  // TELLTALE
    for (std::size_t i = 0; i < n; ++i) {
        esc.registers[in_addr + i] = response[i];
    }
    // TELLTALE: if the previous reply's full bit is still set, the master never
    // read it before we overwrote it — it isn't collecting our replies. This is
    // the cyclic-mode SAFEOP symptom (TwinCAT sees full via LRD but never FPRDs).
    if ((std::to_integer<std::uint8_t>(esc.registers[kSm1Status]) & kMailboxFull) != 0) {
        telltale::bump(telltale::Signal::kReplyUncollected);
    }
    // Signal "mailbox-in full" atomically with the reply: set the SM1 status bit
    // and the AL Event Request SM1 bit in the same step that staged the reply (and
    // bumped the counter). A fixed-cycle master reads the mailbox every cycle and
    // keys off the counter; if the new counter were visible before the full bit, it
    // would record that counter while the mailbox still reads "empty", then reject
    // the later full read as a duplicate. So counter + full must rise together.
    esc.set_u8(kSm1Status, static_cast<std::uint8_t>(
                               std::to_integer<std::uint8_t>(esc.registers[kSm1Status]) |
                               kMailboxFull));
    esc.set_u32(kAlEventRequest, esc.get_u32(kAlEventRequest) | kSm1EventBit);
    telltale::bump(telltale::Signal::kReplyStaged);  // TELLTALE: reply staged into SM1
    if (coe::logging_enabled()) {
        std::fprintf(stderr,
                     "[mbx] staged %zu-byte reply seq=%u at SM1 0x%04x "
                     "(SM1-full + AL-event set)\n",
                     n, static_cast<unsigned>(req_counter >> 4),
                     static_cast<unsigned>(in_addr));
    }
}

// Apply one ESC's read/write for a datagram, mutating the frame's data area.
// A broadcast read combines slaves with a logical OR (real ESC behaviour);
// a unicast read replaces.
void service(Esc& esc, const DatagramView& dg, std::span<std::byte> frame,
             bool read, bool write, bool or_read) noexcept {
    const std::size_t ado = dg.ado;
    const std::size_t len = dg.data.size();
    if (ado + len > Esc::kRegisterSpace) {
        return;
    }
    if (read) {
        for (std::size_t i = 0; i < len; ++i) {
            frame[dg.data_offset + i] =
                or_read ? (frame[dg.data_offset + i] | esc.registers[ado + i])
                        : esc.registers[ado + i];
        }
        // Mailbox-in read: clear "mailbox full" when the master reads the SM1 LAST
        // byte (in_addr + in_len - 1 = 0x10FF). A wire capture shows TwinCAT consumes
        // each reply with three reads: the 2-byte length header (0x1080), a dedicated
        // 1-byte read of the SM's last byte (0x10FF), and the 16-byte content. That
        // last-byte read IS the ESC "mailbox read complete" event (it mirrors the
        // master's 0x107F last-byte "commit" on the write side), so the full bit must
        // drop exactly there. (An earlier attempt to clear on the reply-content end
        // made TwinCAT's 0x10FF read a no-op, so its completion read never saw the bit
        // fall and it retried the SDO forever. The reason this last-byte clear looked
        // broken originally was unrelated: the non-zero echoed mailbox counter made
        // TwinCAT reject the reply at the header, before it ever read the last byte.)
        if (!esc.od.empty()) {
            const std::uint16_t in_addr = esc.get_u16(kSm1Config);
            const std::uint16_t in_len = esc.get_u16(kSm1Config + 2);
            // TELLTALE: does the master read the SM0 (mailbox-out) status byte
            // (0x0805) at all in cyclic mode? This is the decisive evidence for the
            // SM0-handshake SAFEOP hypothesis: if it stays 0 while replies go
            // uncollected, watching SM0 cannot be the gate, so look elsewhere.
            if (ado <= kSm0Status && kSm0Status < ado + len) {
                telltale::bump(telltale::Signal::kSm0StatusRead);
            }
            // TELLTALE: does the master ever attempt to read the SM1 reply buffer
            // (starting at in_addr / 0x1080)? If replies are staged but this stays
            // 0, TwinCAT never tries the FPRD at all (blocked upstream -> SM0 gate);
            // if it is non-zero but collected stays low, it reads the buffer without
            // covering the last byte (our last-byte clear is the problem).
            if (in_addr != 0 && ado <= in_addr && in_addr < ado + len) {
                telltale::bump(telltale::Signal::kMailboxBufferRead);
                // TELLTALE/diagnostic: while a reply is staged (SM1 full), log the
                // *range* of each distinct SM1-buffer read. This shows whether
                // TwinCAT reads the whole SM buffer (reaches the last byte -> our
                // clear fires -> "collected") or only peeks the mailbox header
                // (never reaches last byte -> reply sits uncollected -> SAFEOP
                // stalls). hdr_seq is the staged reply's mailbox sequence counter,
                // so we can correlate a rejected read with its counter.
                if (coe::logging_enabled() && in_len != 0) {
                    const auto st = std::to_integer<std::uint8_t>(esc.registers[kSm1Status]);
                    const std::uint32_t probe =
                        (static_cast<std::uint32_t>(ado) << 16) |
                        static_cast<std::uint32_t>(len & 0xFFFF);
                    if ((st & kMailboxFull) != 0 && probe != esc.mbx_read_probe) {
                        esc.mbx_read_probe = probe;
                        const std::uint16_t last =
                            static_cast<std::uint16_t>(in_addr + in_len - 1);
                        const bool reaches_last = ado <= last && last < ado + len;
                        const auto hdr_seq = static_cast<std::uint8_t>(
                            (std::to_integer<std::uint8_t>(esc.registers[in_addr + 5]) >> 4) &
                            0x07);
                        std::fprintf(
                            stderr,
                            "[mbx-read] SM1 buffer read ado=0x%04zx len=%zu reaches_last=%d "
                            "hdr_seq=%u\n",
                            ado, len, reaches_last ? 1 : 0, static_cast<unsigned>(hdr_seq));
                    }
                }
            }
            const std::uint16_t in_last =
                (in_addr != 0 && in_len != 0)
                    ? static_cast<std::uint16_t>(in_addr + in_len - 1)
                    : 0;
            if (in_addr != 0 && in_len != 0 && ado <= in_last && in_last < ado + len) {
                const auto status =
                    std::to_integer<std::uint8_t>(esc.registers[kSm1Status]);
                const bool was_full = (status & kMailboxFull) != 0;
                esc.set_u8(kSm1Status,
                           static_cast<std::uint8_t>(status & ~kMailboxFull));
                esc.set_u32(kAlEventRequest,
                            esc.get_u32(kAlEventRequest) & ~kSm1EventBit);
                // Only log when a reply was actually waiting: the master polls the
                // mailbox buffer every cycle, so logging every read floods the
                // console. "COLLECTED" therefore marks a real reply pickup.
                if (was_full) {
                    telltale::bump(telltale::Signal::kReplyCollected);  // TELLTALE: master read a staged reply
                    if (coe::logging_enabled()) {
                        std::fprintf(stderr,
                                     "[mbx] master COLLECTED reply at SM1 0x%04x (full + AL-event cleared)\n",
                                     static_cast<unsigned>(in_addr));
                    }
                    // Single-buffer backpressure release: the reply is collected, so
                    // SM1 is free. If a request was deferred while it was unread,
                    // service it now from SM0 and stage its reply (full goes 0->1
                    // again, giving the master a fresh edge to read).
                    if (esc.mbx_request_pending) {
                        esc.mbx_request_pending = false;
                        run_mailbox(esc);
                    }
                }
            }
        }
    }
    if (write) {
        // The SyncManager status bytes (SM0 = 0x0805, SM1 = 0x080D) are ESC-managed
        // and read-only to the master. TwinCAT's SM-config write is 8 bytes
        // (0x0808..0x080F for SM1) and therefore *spans* 0x080D; in cyclic bring-up
        // it re-writes the SM config repeatedly, so a blind copy would clobber our
        // mailbox-full bit (set 0x080D back to 0) right after we staged a reply ->
        // its LRD sees "empty", never FPRDs, and the PreOp->SafeOp transition loops.
        // Preserve our managed status across the write (CoE slaves only). [SAFEOP]
        const bool guard_sm_status = !esc.od.empty();
        const std::byte sm0_status = esc.registers[kSm0Status];
        const std::byte sm1_status = esc.registers[kSm1Status];
        for (std::size_t i = 0; i < len; ++i) {
            esc.registers[ado + i] = frame[dg.data_offset + i];
        }
        if (guard_sm_status) {
            if (ado <= kSm0Status && kSm0Status < ado + len) {
                esc.registers[kSm0Status] = sm0_status;
            }
            if (ado <= kSm1Status && kSm1Status < ado + len) {
                esc.registers[kSm1Status] = sm1_status;
            }
        }
        // A write covering register 0x0010 sets the configured station address.
        if (ado <= kStationAddressRegister && ado + len > kStationAddressRegister) {
            esc.configured_address = esc.get_u16(kStationAddressRegister);
        }
        // ESM: a write to AL Control (0x0120) requests a state transition. We
        // optimistically accept it — reflect the requested state (low nibble)
        // into AL Status (0x0130), clearing the error indication — so the
        // master sees the transition succeed and advances Init->PreOp->SafeOp->Op.
        if (ado <= kAlControlRegister && ado + len > kAlControlRegister) {
            const std::uint16_t requested = esc.get_u16(kAlControlRegister);
            esc.set_u16(kAlStatusRegister, requested & 0x000Fu);
        }
        // SII: a write covering the EEPROM control register may carry a read
        // command (with the word address in the same write); execute it so the
        // master can read our identity from the data register.
        if (ado <= kEepromControlRegister && ado + len > kEepromControlRegister) {
            run_eeprom_command(esc);
        }
        // Mailbox-out write: if this CoE slave's mailbox-out SM buffer (SM0) was
        // written, service the SDO request the master just deposited. A wire capture
        // shows TwinCAT deposits a request as content at 0x1000 then a 1-byte
        // "commit" write to the SM's last byte (0x107F); we trigger on the content
        // write (covering 0x1000), which is what the working PreOp path (AC-05) uses.
        // (Triggering on the 0x107F commit instead — symmetric with the read-side
        // last-byte clear — was tried and regressed: the optimized SafeOp path then
        // left the 0x1c12 reply unread. See docs/coe-safeop-brief.md.)
        if (!esc.od.empty()) {
            const std::uint16_t out_addr = esc.get_u16(kSm0Config);
            const std::uint16_t out_len = esc.get_u16(kSm0Config + 2);
            if (out_addr != 0 && out_len != 0 && ado <= out_addr &&
                out_addr < ado + len) {
                // Single-buffer mailbox backpressure: if the previous reply is
                // still unread (SM1 full), do NOT process now — staging a new reply
                // would clobber the one the master has not collected yet. TwinCAT's
                // optimized path writes the next request (e.g. 0x1c13) before
                // reading the prior reply (0x1c12), so without this the second reply
                // overwrites the first and the bring-up loops. Defer it; the read
                // path runs the pending request once the master collects the reply.
                const bool sm1_full =
                    (std::to_integer<std::uint8_t>(esc.registers[kSm1Status]) &
                     kMailboxFull) != 0;
                if (sm1_full) {
                    esc.mbx_request_pending = true;
                    telltale::bump(telltale::Signal::kMailboxDeferred);  // TELLTALE: backpressure engaged
                } else {
                    run_mailbox(esc);
                }
            }
        }
    }
}

}  // namespace

namespace {

// A minimal description of each emulated slave's ESC, enough for the master's
// identification + topology scan. Values are modelled on a Beckhoff ET1100-class
// ESC and refined against TwinCAT's reaction on the wire.
struct SlaveProfile {
    std::uint8_t esc_type;          // 0x0000 Type (0x11 = ET1100)
    std::uint8_t fmmu_count;        // 0x0004 number of FMMUs
    std::uint8_t sm_count;          // 0x0005 number of SyncManagers
    std::uint8_t ram_kb;            // 0x0006 process-data RAM size (KB)
    std::uint32_t vendor_id;        // SII word 0x08-0x09 (Beckhoff = 0x02)
    std::uint32_t product_code;     // SII word 0x0A-0x0B
    std::uint32_t revision;         // SII word 0x0C-0x0D
    bool is_coupler;                // true for the bus coupler (port 0 is MII)
    bool has_coe;                   // true if the slave exposes a CoE mailbox
    bool has_aoe;                   // true if it also exposes AoE (ADS over EtherCAT)
    // Port wiring (0x0007 port descriptor + 0x0110 DL Status) is not a fixed
    // device trait — it depends on where the slave sits in the chain — so
    // apply_topology() derives it from position, not this profile.
};

// DL Status (0x0110) bit layout (ETG.1000.4): bit4..7 = physical link on port
// 0..3; bit8/10/12/14 = loop closed on port 0..3; bit9/11/13/15 = communication
// established on port 0..3.
inline constexpr std::uint16_t kLinkPort0 = 1u << 4;
inline constexpr std::uint16_t kLinkPort1 = 1u << 5;
inline constexpr std::uint16_t kCommPort0 = 1u << 9;
inline constexpr std::uint16_t kCommPort1 = 1u << 11;
inline constexpr std::uint16_t kLoopClosedPort1 = 1u << 10;
inline constexpr std::uint16_t kLoopClosedPort2 = 1u << 12;
inline constexpr std::uint16_t kLoopClosedPort3 = 1u << 14;

// Identity from configs/esi/: Beckhoff vendor ID 0x02; product codes and
// revisions per the ESI XML for each part. The high 16 bits of a Beckhoff EL
// product code spell the terminal number in hex (EL1008 -> 0x03F0, EL2008 ->
// 0x07D8); EK couplers use the 0x2C52-family suffix.
inline constexpr std::uint32_t kBeckhoffVendorId = 0x00000002;

// EK1100 bus coupler — the station's entry point (port 0 faces the master).
inline constexpr SlaveProfile kEk1100Profile{
    /*esc_type*/ 0x11, /*fmmu*/ 8, /*sm*/ 8, /*ram_kb*/ 8,
    /*vendor*/ kBeckhoffVendorId, /*product*/ 0x044C2C52, /*revision*/ 0x00120000,
    /*is_coupler*/ true, /*has_coe*/ false, /*has_aoe*/ false};

// EL1008 — 8-channel digital input, 24 V.
inline constexpr SlaveProfile kEl1008Profile{
    /*esc_type*/ 0x11, /*fmmu*/ 8, /*sm*/ 8, /*ram_kb*/ 8,
    /*vendor*/ kBeckhoffVendorId, /*product*/ 0x03F03052, /*revision*/ 0x00120000,
    /*is_coupler*/ false, /*has_coe*/ false, /*has_aoe*/ false};

// EL2008 — 8-channel digital output, 24 V. Same ET1100-class ESC as the EL1008;
// revision 0x00100000 is a baseline EL2008-0000 revision (pending the EL2xxx ESI
// in configs/esi/). The master writes its 1-byte output image here via LWR.
inline constexpr SlaveProfile kEl2008Profile{
    /*esc_type*/ 0x11, /*fmmu*/ 8, /*sm*/ 8, /*ram_kb*/ 8,
    /*vendor*/ kBeckhoffVendorId, /*product*/ 0x07D83052, /*revision*/ 0x00100000,
    /*is_coupler*/ false, /*has_coe*/ false, /*has_aoe*/ false};

// EL3001 — 1-channel analog input (12-bit). The first terminal with a CoE
// mailbox: settings (scaling, filter) and identity live in its Object
// Dictionary, read/written by the master over SDO. Product 0x0BB9_3052 (3001 ->
// 0x0BB9); revision 0x00100000 is a baseline pending the EL3xxx ESI.
inline constexpr SlaveProfile kEl3001Profile{
    /*esc_type*/ 0x11, /*fmmu*/ 8, /*sm*/ 8, /*ram_kb*/ 8,
    /*vendor*/ kBeckhoffVendorId, /*product*/ 0x0BB93052, /*revision*/ 0x00100000,
    /*is_coupler*/ false, /*has_coe*/ true, /*has_aoe*/ false};

// EL6224 — 4-channel IO-Link master (communication terminal). The second CoE
// terminal. Product 0x1850_3052 (6224 -> 0x1850), revision 0x00100000 — from the
// Beckhoff EL6xxx ESI (configs/esi/). Its mailbox-in SM sits at 0x1100 (vs the
// EL3001's 0x1080), but the mailbox handler reads the *live* SM addresses, so no
// special-casing is needed. With no IO-Link device configured the default process
// image is input-only: TxPDO 0x1A04 "DeviceState" = 4 bytes, one USINT per port
// (0xF100:01..04). Downstream IO-Link PD + ISDU are later M7 phases.
inline constexpr SlaveProfile kEl6224Profile{
    /*esc_type*/ 0x11, /*fmmu*/ 8, /*sm*/ 8, /*ram_kb*/ 8,
    /*vendor*/ kBeckhoffVendorId, /*product*/ 0x18503052, /*revision*/ 0x00100000,
    /*is_coupler*/ false, /*has_coe*/ true, /*has_aoe*/ true};

// Set this ESC's port descriptor (0x0007) and DL Status (0x0110) for its place
// in a linear E-bus chain. Port 0 always faces upstream (the master for the
// coupler, the previous slave for a terminal) and is reported linked + comm.
// Port 1 faces downstream and is reported linked only when another slave follows
// — otherwise its loop is closed. Ports 2/3 are always closed. The coupler's
// port 0 is MII (to the master); every other E-bus port is EBUS.
void apply_topology(Esc& esc, bool is_coupler, bool has_downstream) noexcept {
    std::uint16_t dl = kLinkPort0 | kCommPort0;  // upstream / master side
    dl |= has_downstream ? static_cast<std::uint16_t>(kLinkPort1 | kCommPort1)
                         : kLoopClosedPort1;
    dl |= kLoopClosedPort2 | kLoopClosedPort3;
    esc.set_u16(0x0110, dl);

    // Port descriptor: 2 bits/port — 0b11 MII, 0b10 EBUS, 0b00 not present.
    std::uint8_t port_desc = is_coupler ? 0x03 : 0x02;       // port 0
    if (has_downstream) {
        port_desc = static_cast<std::uint8_t>(port_desc | 0x08);  // port 1 EBUS
    }
    esc.set_u8(0x0007, port_desc);
}

void apply_profile(Esc& esc, const SlaveProfile& p) noexcept {
    esc.set_u8(0x0000, p.esc_type);       // Type
    esc.set_u8(0x0004, p.fmmu_count);     // FMMUs supported
    esc.set_u8(0x0005, p.sm_count);       // SyncManagers supported
    esc.set_u8(0x0006, p.ram_kb);         // process-data RAM (KB)
    esc.set_u16(kAlStatusRegister, static_cast<std::uint16_t>(AlState::kInit));

    // SII (EEPROM) identity area, word-addressed (ETG.1000.6): vendor ID at
    // word 0x08-0x09, product code at 0x0A-0x0B, revision at 0x0C-0x0D, serial
    // at 0x0E-0x0F. Config words 0x00-0x07 stay zero; we report "checksum OK"
    // in the EEPROM status, so the master does not validate the CRC word.
    esc.eeprom[0x08] = static_cast<std::uint16_t>(p.vendor_id & 0xFFFF);
    esc.eeprom[0x09] = static_cast<std::uint16_t>(p.vendor_id >> 16);
    esc.eeprom[0x0A] = static_cast<std::uint16_t>(p.product_code & 0xFFFF);
    esc.eeprom[0x0B] = static_cast<std::uint16_t>(p.product_code >> 16);
    esc.eeprom[0x0C] = static_cast<std::uint16_t>(p.revision & 0xFFFF);
    esc.eeprom[0x0D] = static_cast<std::uint16_t>(p.revision >> 16);

    // SII standard mailbox configuration (ETG.1000.6): words 0x18-0x1B give the
    // standard receive (master->slave, SM0) and send (slave->master, SM1)
    // mailbox offsets/sizes; word 0x1C is the supported-protocol bitmask. We
    // advertise CoE so the master sets up the mailbox SyncManagers on scan. The
    // master still programs the live SM addresses (0x0800/0x0808), which the
    // mailbox handler reads — these SII values just declare the capability.
    if (p.has_coe || p.has_aoe) {
        esc.eeprom[0x18] = 0x1000;  // std receive (SM0, mailbox out) offset
        esc.eeprom[0x19] = 0x0080;  // std receive size
        esc.eeprom[0x1A] = 0x1080;  // std send (SM1, mailbox in) offset
        esc.eeprom[0x1B] = 0x0080;  // std send size
        // Mailbox protocol bitmask: CoE = bit 2 (0x04), AoE = bit 0 (0x01). The
        // EL6224 IO-Link master advertises both (0x05) — it uses AoE for the
        // ADS/ISDU channel and CoE for SDOs.
        esc.eeprom[0x1C] = static_cast<std::uint16_t>(
            (p.has_coe ? 0x0004u : 0u) | (p.has_aoe ? 0x0001u : 0u));
    }
}

// Build the EL3001's CoE Object Dictionary: enough standard + device objects for
// a meaningful CoE-Online exchange (identity, the analog input value, and a
// couple of writable channel settings). Values up to 4 bytes (expedited SDO);
// the device-name string (0x1008) is left to the ESI for now.
void populate_el3001_od(Esc& esc) {
    esc.od.add(0x1000, 0x00, 0x00001389, 4, false);  // Device Type (input profile)
    esc.od.add(0x1018, 0x00, 4, 1, false);           // Identity: number of entries
    esc.od.add(0x1018, 0x01, kEl3001Profile.vendor_id, 4, false);    // Vendor ID
    esc.od.add(0x1018, 0x02, kEl3001Profile.product_code, 4, false); // Product code
    esc.od.add(0x1018, 0x03, kEl3001Profile.revision, 4, false);     // Revision
    esc.od.add(0x1018, 0x04, 0x00000000, 4, false);  // Serial number
    // 0x6000 AI Inputs (record) and 0x8000 AI Settings (record). A CoE record must
    // expose sub-index 0 = the highest sub-index, because the master reads it first
    // to learn the object's shape before touching any sub-entry; without it every
    // access aborts with "no such sub-index" (0x06090011).
    esc.od.add(0x6000, 0x00, 0x11, 1, false);        // AI Inputs: highest sub-index (0x11)
    esc.od.add(0x6000, 0x01, 0x0000, 2, false);      // AI status word (read-only)
    esc.od.add(0x6000, 0x11, 0x0000, 2, false);      // AI value INT16 (read-only)
    esc.od.add(0x8000, 0x00, 0x11, 1, false);        // AI Settings: highest sub-index (0x11)
    esc.od.add(0x8000, 0x06, 0, 1, true);            // "Enable filter" (writable)
    esc.od.add(0x8000, 0x11, 0, 2, true);            // "User scale offset" INT16 (writable)

    // SyncManager / PDO configuration objects. The master writes these over CoE
    // during PreOp->SafeOp to assign the process-data PDOs; without them the
    // SDOs abort and the input SyncManager is never set up (SafeOp fails).
    esc.od.add(0x1C00, 0x00, 4, 1, false);   // SM communication types: 4 entries
    esc.od.add(0x1C00, 0x01, 1, 1, false);   //   SM0 = Mailbox Out
    esc.od.add(0x1C00, 0x02, 2, 1, false);   //   SM1 = Mailbox In
    esc.od.add(0x1C00, 0x03, 3, 1, false);   //   SM2 = Process Data Out (unused)
    esc.od.add(0x1C00, 0x04, 4, 1, false);   //   SM3 = Process Data In (the AI)
    esc.od.add(0x1C12, 0x00, 0, 1, true);    // RxPDO assign: none (input-only terminal)
    esc.od.add(0x1C13, 0x00, 1, 1, true);    // TxPDO assign: 1 PDO
    esc.od.add(0x1C13, 0x01, 0x1A00, 2, true);  //   -> TxPDO 0x1A00
    esc.od.add(0x1A00, 0x00, 2, 1, false);   // TxPDO 0x1A00 mapping: 2 entries (4 bytes)
    esc.od.add(0x1A00, 0x01, 0x60000110, 4, false);  // 0x6000:01 status, 16 bits
    esc.od.add(0x1A00, 0x02, 0x60001110, 4, false);  // 0x6000:11 value, 16 bits
}

// Build the EL6224 IO-Link master's CoE Object Dictionary. Mirrors the Beckhoff
// EL6xxx ESI (rev 0x00100000) for the default, no-device-configured state: the
// process image is input-only and consists of TxPDO 0x1A04 "DeviceState" = 4
// bytes, one USINT per IO-Link port (0xF100:01..04, the port status). Identity +
// device type are read from the ESI. Per-port IO-Link process data (0x60xx) and
// ISDU parameter access are added in later M7 phases once a device is modelled.
void populate_el6224_od(Esc& esc) {
    esc.od.add(0x1000, 0x00, 0x184C1389, 4, false);  // Device Type (ESI default)
    esc.od.add(0x1018, 0x00, 4, 1, false);           // Identity: number of entries
    esc.od.add(0x1018, 0x01, kEl6224Profile.vendor_id, 4, false);    // Vendor ID
    esc.od.add(0x1018, 0x02, kEl6224Profile.product_code, 4, false); // Product code
    esc.od.add(0x1018, 0x03, kEl6224Profile.revision, 4, false);     // Revision
    esc.od.add(0x1018, 0x04, 0x00000000, 4, false);  // Serial number

    // 0xF100 "DeviceState Inputs": sub-0 = highest sub-index (4), then one USINT
    // per port (0x_0 = port disabled .. 0x_3 = IO-Link comm OP .. 0xA_ = no device).
    // Left at 0 (disabled) until a port is driven in phase 7.2.
    esc.od.add(0xF100, 0x00, 4, 1, false);     // highest sub-index
    esc.od.add(0xF100, 0x01, 0x00, 1, false);  // State Ch1
    esc.od.add(0xF100, 0x02, 0x00, 1, false);  // State Ch2
    esc.od.add(0xF100, 0x03, 0x00, 1, false);  // State Ch3
    esc.od.add(0xF100, 0x04, 0x00, 1, false);  // State Ch4

    // SyncManager / PDO configuration — mirror the EL6224 ESI default with no
    // IO-Link device configured. The master assigns ALL four per-port PDOs on each
    // SM plus DeviceState, and downloads every per-port mapping during
    // PreOp->SafeOp; each must exist and be writable or the transition aborts
    // ("download pdo 0x1A00 entries"). With no device the per-port PDOs are empty
    // (sub-0 = 0); only 0x1A04 (DeviceState) carries data, so the input image is
    // 4 bytes and the output image is empty.
    esc.od.add(0x1C00, 0x00, 4, 1, false);   // SM communication types: 4 entries
    esc.od.add(0x1C00, 0x01, 1, 1, false);   //   SM0 = Mailbox Out
    esc.od.add(0x1C00, 0x02, 2, 1, false);   //   SM1 = Mailbox In
    esc.od.add(0x1C00, 0x03, 3, 1, false);   //   SM2 = Process Data Out
    esc.od.add(0x1C00, 0x04, 4, 1, false);   //   SM3 = Process Data In

    // Per-port output PDOs 0x1600..0x1603 and input PDOs 0x1A00..0x1A03 — empty
    // (no device) but present and writable so the master can configure them.
    for (std::uint16_t ch = 0; ch < 4; ++ch) {
        esc.od.add(static_cast<std::uint16_t>(0x1600 + ch), 0x00, 0, 1, true);  // RxPDO Ch n
        esc.od.add(static_cast<std::uint16_t>(0x1A00 + ch), 0x00, 0, 1, true);  // TxPDO Ch n
    }
    // 0x1A04 "DeviceState" input PDO: 4 entries = the four port-status USINTs.
    esc.od.add(0x1A04, 0x00, 4, 1, false);
    esc.od.add(0x1A04, 0x01, 0xF1000108, 4, false);  // 0xF100:01 State Ch1, 8 bits
    esc.od.add(0x1A04, 0x02, 0xF1000208, 4, false);  // 0xF100:02 State Ch2, 8 bits
    esc.od.add(0x1A04, 0x03, 0xF1000308, 4, false);  // 0xF100:03 State Ch3, 8 bits
    esc.od.add(0x1A04, 0x04, 0xF1000408, 4, false);  // 0xF100:04 State Ch4, 8 bits

    // SM2 RxPDO assign = the four (empty) per-port output PDOs.
    esc.od.add(0x1C12, 0x00, 4, 1, true);
    esc.od.add(0x1C12, 0x01, 0x1600, 2, true);
    esc.od.add(0x1C12, 0x02, 0x1601, 2, true);
    esc.od.add(0x1C12, 0x03, 0x1602, 2, true);
    esc.od.add(0x1C12, 0x04, 0x1603, 2, true);
    // SM3 TxPDO assign = the four (empty) per-port input PDOs + DeviceState (0x1A04).
    esc.od.add(0x1C13, 0x00, 5, 1, true);
    esc.od.add(0x1C13, 0x01, 0x1A00, 2, true);
    esc.od.add(0x1C13, 0x02, 0x1A01, 2, true);
    esc.od.add(0x1C13, 0x03, 0x1A02, 2, true);
    esc.od.add(0x1C13, 0x04, 0x1A03, 2, true);
    esc.od.add(0x1C13, 0x05, 0x1A04, 2, true);
}

}  // namespace

Chain::Chain(std::size_t slave_count) {
    m_slaves.resize(slave_count);
    // Phase-1 topology, by position: slave 0 = EK1100 coupler, slave 1 = EL1008
    // (digital input), slave 2 = EL2008 (digital output), slave 3 = EL3001
    // (analog input, CoE), slave 4 = EL6224 (IO-Link master, CoE). Any further
    // slaves reuse the EL6224 profile for now.
    // (A YAML-driven topology replaces this hard-coding in a later milestone.)
    for (std::size_t i = 0; i < m_slaves.size(); ++i) {
        const SlaveProfile& p = (i == 0)   ? kEk1100Profile
                                : (i == 1) ? kEl1008Profile
                                : (i == 2) ? kEl2008Profile
                                : (i == 3) ? kEl3001Profile
                                           : kEl6224Profile;
        apply_profile(m_slaves[i], p);
        apply_topology(m_slaves[i], /*is_coupler=*/i == 0,
                       /*has_downstream=*/i + 1 < m_slaves.size());
        if (p.has_coe) {
            if (i == 3) {
                populate_el3001_od(m_slaves[i]);
            } else {
                populate_el6224_od(m_slaves[i]);
            }
        }
    }
}

bool Chain::set_process_input(std::size_t slave_index, std::uint8_t value) noexcept {
    if (slave_index >= m_slaves.size()) {
        return false;
    }
    Esc& esc = m_slaves[slave_index];
    for (std::uint16_t f = 0; f < 8; ++f) {
        const std::uint16_t base = static_cast<std::uint16_t>(0x0600 + f * 0x10);
        const auto activate = std::to_integer<std::uint8_t>(esc.registers[base + 0x0C]);
        const auto type = std::to_integer<std::uint8_t>(esc.registers[base + 0x0B]);
        if ((activate & 0x01) && (type & 0x01)) {  // active, read (input) FMMU
            esc.set_u8(esc.get_u16(base + 0x08), value);
            return true;
        }
    }
    return false;
}

bool Chain::set_analog_input(std::size_t slave_index, std::int16_t value,
                             std::uint16_t status) noexcept {
    if (slave_index >= m_slaves.size()) {
        return false;
    }
    Esc& esc = m_slaves[slave_index];
    for (std::uint16_t f = 0; f < 8; ++f) {
        const std::uint16_t base = static_cast<std::uint16_t>(0x0600 + f * 0x10);
        const auto activate = std::to_integer<std::uint8_t>(esc.registers[base + 0x0C]);
        const auto type = std::to_integer<std::uint8_t>(esc.registers[base + 0x0B]);
        const std::uint16_t length = esc.get_u16(base + 0x04);
        // A CoE terminal can have two read FMMUs: the 1-byte mailbox-status poll
        // (SM1 status 0x080D) and the process-data input (the SM3 buffer: status
        // word + INT16 value). Pick the multi-byte process-data one (length >= 2),
        // skipping the 1-byte status FMMU, then write status @ offset 0 and the
        // INT16 value @ offset 2 — the TxPDO 0x1A00 layout.
        if ((activate & 0x01) && (type & 0x01) && length >= 2) {
            const std::uint16_t phys = esc.get_u16(base + 0x08);
            esc.set_u16(phys, status);
            esc.set_u16(static_cast<std::uint16_t>(phys + 2),
                        static_cast<std::uint16_t>(value));
            return true;
        }
    }
    return false;
}

std::optional<std::uint8_t> Chain::output_byte(
    std::size_t slave_index) const noexcept {
    if (slave_index >= m_slaves.size()) {
        return std::nullopt;
    }
    const Esc& esc = m_slaves[slave_index];
    for (std::uint16_t f = 0; f < 8; ++f) {
        const std::uint16_t base = static_cast<std::uint16_t>(0x0600 + f * 0x10);
        const auto activate = std::to_integer<std::uint8_t>(esc.registers[base + 0x0C]);
        const auto type = std::to_integer<std::uint8_t>(esc.registers[base + 0x0B]);
        if ((activate & 0x01) && (type & 0x02)) {  // active, write (output) FMMU
            return std::to_integer<std::uint8_t>(esc.registers[esc.get_u16(base + 0x08)]);
        }
    }
    return std::nullopt;
}

bool Chain::process_frame(std::span<std::byte> frame) noexcept {
    bool serviced = false;

    const auto parsed = for_each_datagram(
        std::span<const std::byte>(frame), [&](const DatagramView& dg) {
            const Addressing mode = addressing_of(dg.cmd);
            if (mode == Addressing::kNone) {
                return;
            }
            const bool read = is_read(dg.cmd);
            const bool write = is_write(dg.cmd);
            std::uint16_t wkc = dg.wkc;

            switch (mode) {
                case Addressing::kBroadcast: {
                    std::uint16_t adp = dg.adp;
                    for (Esc& esc : m_slaves) {
                        service(esc, dg, frame, read, write, /*or_read=*/true);
                        if (read) ++wkc;
                        if (write) ++wkc;
                        ++adp;  // every slave increments the position field
                    }
                    detail::write_u16_le(frame, dg.address_offset, adp);
                    break;
                }
                case Addressing::kAutoIncrement: {
                    std::uint16_t adp = dg.adp;
                    for (Esc& esc : m_slaves) {
                        if (adp == 0) {  // this slave is the addressed one
                            service(esc, dg, frame, read, write, /*or_read=*/false);
                            if (read) ++wkc;
                            if (write) ++wkc;
                        }
                        ++adp;
                    }
                    detail::write_u16_le(frame, dg.address_offset, adp);
                    break;
                }
                case Addressing::kConfigured: {
                    for (Esc& esc : m_slaves) {
                        if (esc.configured_address == dg.adp) {
                            service(esc, dg, frame, read, write, /*or_read=*/false);
                            if (read) ++wkc;
                            if (write) ++wkc;
                        }
                    }
                    break;
                }
                case Addressing::kLogical: {
                    // Logical (LRD/LWR/LRW): each slave maps its process data
                    // into the master's logical image via its FMMUs. For every
                    // active FMMU whose logical window overlaps this datagram,
                    // copy the mapped bytes (input: registers->frame; output:
                    // frame->registers) and count the working counter.
                    const std::uint32_t log_start = dg.logical_address;
                    const std::uint32_t log_end =
                        log_start + static_cast<std::uint32_t>(dg.data.size());
                    for (Esc& esc : m_slaves) {
                        bool read_hit = false;
                        bool write_hit = false;
                        for (std::uint16_t f = 0; f < 8; ++f) {
                            const std::uint16_t base =
                                static_cast<std::uint16_t>(0x0600 + f * 0x10);
                            const auto activate =
                                std::to_integer<std::uint8_t>(esc.registers[base + 0x0C]);
                            if ((activate & 0x01) == 0) {
                                continue;
                            }
                            const std::uint32_t fmmu_log = esc.get_u32(base + 0x00);
                            const std::uint16_t fmmu_len = esc.get_u16(base + 0x04);
                            const std::uint16_t fmmu_phys = esc.get_u16(base + 0x08);
                            const auto type =
                                std::to_integer<std::uint8_t>(esc.registers[base + 0x0B]);
                            const std::uint32_t ov_start = std::max(fmmu_log, log_start);
                            const std::uint32_t ov_end =
                                std::min<std::uint32_t>(fmmu_log + fmmu_len, log_end);
                            if (ov_start >= ov_end) {
                                continue;
                            }
                            const std::size_t n = ov_end - ov_start;
                            const std::size_t frame_off =
                                dg.data_offset + (ov_start - log_start);
                            const std::size_t phys_off = fmmu_phys + (ov_start - fmmu_log);
                            if (phys_off + n > Esc::kRegisterSpace) {
                                continue;
                            }
                            if ((type & 0x01) && read) {  // input: slave -> frame
                                for (std::size_t i = 0; i < n; ++i) {
                                    frame[frame_off + i] = esc.registers[phys_off + i];
                                }
                                read_hit = true;
                                // Diagnostic: TwinCAT polls the mailbox-in (SM1)
                                // status byte (0x080D) through the process image
                                // via this FMMU+LRD — that is how it learns a
                                // reply is waiting. Log the byte on change so the
                                // rig shows whether the poll reaches us and
                                // carries the mailbox-full bit (0x08).
                                if (coe::logging_enabled() && phys_off <= kSm1Status &&
                                    kSm1Status < phys_off + n) {
                                    const std::uint8_t b = std::to_integer<std::uint8_t>(
                                        esc.registers[kSm1Status]);
                                    if (b != esc.mbx_status_poll) {
                                        esc.mbx_status_poll = b;
                                        std::fprintf(stderr,
                                                     "[mbx] master LRD-polled SM1-status 0x080D"
                                                     " -> 0x%02x (logical 0x%08x)\n",
                                                     static_cast<unsigned>(b),
                                                     static_cast<unsigned>(ov_start));
                                    }
                                }
                            }
                            if ((type & 0x02) && write) {  // output: frame -> slave
                                for (std::size_t i = 0; i < n; ++i) {
                                    esc.registers[phys_off + i] = frame[frame_off + i];
                                }
                                write_hit = true;
                            }
                        }
                        if (read_hit) {
                            ++wkc;
                        }
                        if (write_hit) {
                            wkc = static_cast<std::uint16_t>(
                                wkc + (dg.cmd == Command::kLrw ? 2 : 1));
                        }
                    }
                    break;
                }
                case Addressing::kNone:
                    break;
            }

            if (wkc != dg.wkc) {
                detail::write_u16_le(frame, dg.wkc_offset, wkc);
                serviced = true;
            }
        });

    return parsed.has_value() && serviced;
}

}  // namespace ecat
