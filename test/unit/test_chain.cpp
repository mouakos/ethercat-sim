// Unit tests for the ESC register-file engine (ecat::Chain): addressing modes,
// read/write, working-counter rules, and state persistence.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "ecat/chain.hpp"
#include "ecat/coe.hpp"
#include "ecat/frame.hpp"
#include "ecat/station.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s: %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) {
        ++g_failures;
    }
}

// Build a single-datagram EtherCAT frame.
std::vector<std::byte> make_frame(std::uint8_t cmd, std::uint16_t adp,
                                  std::uint16_t ado,
                                  std::vector<std::uint8_t> data,
                                  std::uint16_t wkc = 0) {
    std::vector<std::byte> f;
    auto push = [&](unsigned b) { f.push_back(static_cast<std::byte>(b & 0xFF)); };

    for (int i = 0; i < 6; ++i) push(0x01);  // dst MAC
    for (int i = 0; i < 6; ++i) push(0x38);  // src MAC (master)
    push(0x88);
    push(0xa4);  // ethertype

    const std::size_t dlen = data.size();
    const std::uint16_t dgram_len = static_cast<std::uint16_t>(10 + dlen + 2);
    const std::uint16_t ecat_hdr = static_cast<std::uint16_t>((1u << 12) | (dgram_len & 0x7FF));
    push(ecat_hdr);
    push(ecat_hdr >> 8);

    push(cmd);
    push(0x00);  // index
    push(adp);
    push(adp >> 8);
    push(ado);
    push(ado >> 8);
    const std::uint16_t len_flags = static_cast<std::uint16_t>(dlen & 0x7FF);
    push(len_flags);
    push(len_flags >> 8);
    push(0x00);
    push(0x00);  // irq
    for (const std::uint8_t d : data) push(d);
    push(wkc);
    push(wkc >> 8);
    return f;
}

// Field offsets for a single-datagram frame built by make_frame.
constexpr std::size_t kDataOffset = 26;
std::uint16_t wkc_of(const std::vector<std::byte>& f, std::size_t data_len) {
    return ecat::detail::read_u16_le(std::span<const std::byte>(f), kDataOffset + data_len);
}
int data_byte(const std::vector<std::byte>& f, std::size_t i) {
    return static_cast<int>(std::to_integer<unsigned>(f[kDataOffset + i]));
}

constexpr std::uint8_t kAprd = 1, kApwr = 2, kFprd = 4, kBrd = 7, kBwr = 8;

}  // namespace

int main() {
    using ecat::AlState;
    using ecat::Chain;
    using ecat::Esc;

    // --- Broadcast read of AL Status: both slaves OR in Init, WKC = 2 ---
    {
        Chain chain(2);
        auto f = make_frame(kBrd, 0, 0x0130, {0x00, 0x00});
        check(chain.process_frame(f), "BRD AL Status serviced");
        check(data_byte(f, 0) == static_cast<int>(AlState::kInit), "AL Status reads Init");
        check(wkc_of(f, 2) == 2, "broadcast read WKC == slave count (2)");
    }

    // --- Auto-increment write addresses exactly one slave; state persists ---
    {
        Chain chain(2);
        auto w0 = make_frame(kApwr, 0x0000, 0x0120, {0xAB, 0xCD});  // slave 0
        check(chain.process_frame(w0), "APWR to slave 0 serviced");
        check(wkc_of(w0, 2) == 1, "auto-increment write WKC == 1");
        check(chain.slave(0).get_u16(0x0120) == 0xCDAB, "slave 0 register written");
        check(chain.slave(1).get_u16(0x0120) == 0x0000, "slave 1 untouched");

        auto w1 = make_frame(kApwr, 0xFFFF, 0x0120, {0x11, 0x22});  // slave 1
        check(chain.process_frame(w1), "APWR to slave 1 serviced");
        check(chain.slave(1).get_u16(0x0120) == 0x2211, "slave 1 register written");
        check(chain.slave(0).get_u16(0x0120) == 0xCDAB, "slave 0 value persisted");

        auto r0 = make_frame(kAprd, 0x0000, 0x0120, {0x00, 0x00});  // read slave 0
        check(chain.process_frame(r0), "APRD from slave 0 serviced");
        check(data_byte(r0, 0) == 0xAB && data_byte(r0, 1) == 0xCD, "APRD returns slave 0 data");
    }

    // --- Broadcast write reaches every slave, WKC = 2 ---
    {
        Chain chain(2);
        auto f = make_frame(kBwr, 0, 0x0080, {0x55});
        check(chain.process_frame(f), "BWR serviced");
        check(wkc_of(f, 1) == 2, "broadcast write WKC == 2");
        check(chain.slave(0).get_u16(0x0080) == 0x0055 &&
                  chain.slave(1).get_u16(0x0080) == 0x0055,
              "both slaves written by BWR");
    }

    // --- Station-address assignment then configured-address read ---
    {
        Chain chain(2);
        auto set_addr = make_frame(kApwr, 0x0000, 0x0010, {0xE9, 0x03});  // 1001
        check(chain.process_frame(set_addr), "station address write serviced");
        check(chain.slave(0).configured_address == 1001, "configured address set to 1001");

        auto fprd = make_frame(kFprd, 1001, 0x0010, {0x00, 0x00});
        check(chain.process_frame(fprd), "FPRD to station address serviced");
        check(wkc_of(fprd, 2) == 1, "configured read WKC == 1");
        check(data_byte(fprd, 0) == 0xE9 && data_byte(fprd, 1) == 0x03,
              "FPRD returns the station address");

        auto miss = make_frame(kFprd, 2002, 0x0010, {0x00, 0x00});  // no such address
        check(!chain.process_frame(miss), "FPRD to unknown address not serviced");
    }

    // --- ESM: writing AL Control (0x0120) advances AL Status (0x0130) ---
    {
        Chain chain(2);
        // Slaves start in Init.
        auto rd_init = make_frame(kBrd, 0, 0x0130, {0x00, 0x00});
        (void)chain.process_frame(rd_init);
        check(data_byte(rd_init, 0) == static_cast<int>(AlState::kInit),
              "AL Status starts at Init");

        // Master commands PreOp (0x02) on slave 0 via auto-increment write.
        auto to_preop = make_frame(kApwr, 0x0000, 0x0120, {0x02, 0x00});
        check(chain.process_frame(to_preop), "AL Control write serviced");
        check(chain.slave(0).get_u16(0x0130) == static_cast<std::uint16_t>(AlState::kPreOp),
              "slave 0 AL Status follows to PreOp");
        check(chain.slave(1).get_u16(0x0130) == static_cast<std::uint16_t>(AlState::kInit),
              "slave 1 AL Status unchanged (not addressed)");

        // Command Op (0x08) and confirm it follows; error bit (0x10) cleared.
        auto to_op = make_frame(kApwr, 0x0000, 0x0120, {0x08, 0x00});
        (void)chain.process_frame(to_op);
        check(chain.slave(0).get_u16(0x0130) == static_cast<std::uint16_t>(AlState::kOp),
              "slave 0 AL Status follows to Op");
    }

    // --- Slave profiles: ESC info block + DL Status port links populated ---
    {
        Chain chain(2);
        // EK1100 (slave 0): ESC type set, ports 0+1 linked.
        check(chain.slave(0).registers[0x0000] != std::byte{0}, "EK1100 ESC Type populated");
        const std::uint16_t dl0 = chain.slave(0).get_u16(0x0110);
        check((dl0 & (1u << 4)) != 0, "EK1100 reports physical link on port 0 (A)");
        check((dl0 & (1u << 5)) != 0, "EK1100 reports physical link on port 1 (B)");
        // EL1008 (slave 1): only port 0 linked.
        const std::uint16_t dl1 = chain.slave(1).get_u16(0x0110);
        check((dl1 & (1u << 4)) != 0, "EL1008 reports physical link on port 0 (A)");
        check((dl1 & (1u << 5)) == 0, "EL1008 reports no link on port 1");
        // A configured-address read of DL Status returns the populated value.
        auto set_addr = make_frame(kApwr, 0x0000, 0x0010, {0xE9, 0x03});  // 1001
        (void)chain.process_frame(set_addr);
        auto rd_dl = make_frame(kFprd, 1001, 0x0110, {0x00, 0x00});
        check(chain.process_frame(rd_dl), "FPRD of DL Status serviced");
        check(((data_byte(rd_dl, 0) | (data_byte(rd_dl, 1) << 8)) & (1u << 4)) != 0,
              "master reads link bit set on EK1100 port 0");
    }

    // --- SII EEPROM read: master reads identity (vendor/product) ---
    {
        Chain chain(2);
        // Master writes EEPROM read command (0x0100) + word address 0x0008
        // (Vendor ID) to slave 0, in one auto-increment write covering
        // 0x0502..0x0507: control=0x0100, addr lo=0x0008, addr hi=0x0000.
        auto eep_cmd = make_frame(kApwr, 0x0000, 0x0502,
                                  {0x00, 0x01, 0x08, 0x00, 0x00, 0x00});
        check(chain.process_frame(eep_cmd), "EEPROM read command serviced");
        // Slave 0 (EK1100) data register 0x0508 should now hold vendor ID low
        // word (0x0002) and the product code follows two words later.
        check(chain.slave(0).get_u16(0x0508) == 0x0002, "EEPROM data = Beckhoff vendor ID");
        check(chain.slave(0).get_u16(0x050C) == 0x2C52, "EEPROM data+2 = EK1100 product low word");

        // Now the master reads the data register back via FPRD.
        auto set_addr = make_frame(kApwr, 0x0000, 0x0010, {0xE9, 0x03});  // 1001
        (void)chain.process_frame(set_addr);
        auto rd_data = make_frame(kFprd, 1001, 0x0508, {0x00, 0x00});
        check(chain.process_frame(rd_data), "FPRD of EEPROM data serviced");
        check((data_byte(rd_data, 0) | (data_byte(rd_data, 1) << 8)) == 0x0002,
              "master reads vendor ID from EEPROM data register");
    }

    // --- Logical addressing (LRD) via FMMU: input PD mapped into the image ---
    {
        constexpr std::uint8_t kLrd = 10;
        Chain chain(2);
        Esc& el = chain.slave(1);  // EL1008
        // Put the input byte at physical 0x1000.
        el.registers[0x1000] = std::byte{0xA5};
        // Configure FMMU 0 on the EL1008: logical 0x00010000, length 1, physical
        // 0x1000, type read (0x01), active (0x01).
        el.set_u32(0x0600, 0x00010000);  // logical start
        el.set_u16(0x0604, 0x0001);      // length
        el.set_u16(0x0608, 0x1000);      // physical start
        el.set_u8(0x060B, 0x01);         // type = read
        el.set_u8(0x060C, 0x01);         // activate

        // Master LRD of logical 0x00010000, 1 byte. Frame helper puts the 32-bit
        // address in the adp/ado fields (logical_address spans both).
        auto lrd = make_frame(kLrd, 0x0000, 0x0001, {0x00});  // address = 0x00010000
        check(chain.process_frame(lrd), "LRD serviced via FMMU");
        check(data_byte(lrd, 0) == 0xA5, "LRD returns the EL1008 input byte through the FMMU");
        check(wkc_of(lrd, 1) == 1, "LRD working counter == 1 (one input slave)");

        // set_process_input writes through the active read FMMU; a fresh LRD
        // then returns the new value (Milestone 4 DI round-trip).
        check(chain.set_process_input(1, 0x42), "set_process_input writes via the read FMMU");
        auto lrd2 = make_frame(kLrd, 0x0000, 0x0001, {0x00});
        (void)chain.process_frame(lrd2);
        check(data_byte(lrd2, 0) == 0x42, "LRD reflects the updated DI value");
    }

    // --- Analog input: set_analog_input writes the PD FMMU, not the status FMMU ---
    {
        Chain chain(2);
        Esc& ai = chain.slave(1);
        // Two active read FMMUs, like a CoE analog terminal: FMMU0 = 1-byte
        // mailbox-status poll (phys 0x080D); FMMU1 = 4-byte process-data input
        // (phys 0x1180). set_analog_input must choose the multi-byte PD one.
        ai.set_u32(0x0600, 0x00090000); ai.set_u16(0x0604, 0x0001);  // FMMU0 len 1
        ai.set_u16(0x0608, 0x080D); ai.set_u8(0x060B, 0x01); ai.set_u8(0x060C, 0x01);
        ai.set_u32(0x0610, 0x000A0000); ai.set_u16(0x0614, 0x0004);  // FMMU1 len 4
        ai.set_u16(0x0618, 0x1180); ai.set_u8(0x061B, 0x01); ai.set_u8(0x061C, 0x01);

        check(chain.set_analog_input(1, static_cast<std::int16_t>(-1234), 0x8000),
              "set_analog_input writes via the multi-byte input FMMU");
        check(ai.get_u16(0x1180) == 0x8000,
              "status word (TxPDO Toggle bit) lands at PD offset 0");
        check(static_cast<std::int16_t>(ai.get_u16(0x1182)) == -1234,
              "INT16 value lands at PD offset 2");
        check(std::to_integer<unsigned>(ai.registers[0x080D]) == 0x00,
              "the 1-byte mailbox-status FMMU (0x080D) is left untouched");
    }

    // --- SM status byte is read-only to a master write (SAFEOP fix) ---
    {
        constexpr std::uint8_t kApwr = 2;
        Chain chain(4);
        Esc& coe = chain.slave(3);  // EL3001 (CoE: OD non-empty -> guard active)
        coe.set_u8(0x080D, 0x08);   // pretend a mailbox reply is staged (SM1 full)
        // TwinCAT re-writes the SM1 config: an 8-byte write at 0x0808 that spans the
        // SM1 status byte 0x080D. A blind copy would clear our mailbox-full bit; the
        // read-only guard must preserve it. (adp 0xFFFD addresses slave 3 of four.)
        auto cfg = make_frame(kApwr, 0xFFFD, 0x0808, std::vector<std::uint8_t>(8, 0));
        (void)chain.process_frame(cfg);
        check((std::to_integer<unsigned>(coe.registers[0x080D]) & 0x08) != 0,
              "SM1 mailbox-full survives a master SM-config write (SM status is RO)");
    }

    // --- Three-slave chain: EL2008 identity + position-aware topology (M6) ---
    {
        Chain chain(3);  // EK1100 + EL1008 + EL2008

        // Position-aware topology: with the EL2008 appended, the EL1008 is now a
        // middle terminal and reports a downstream link on port 1; the EL2008 is
        // the tail (port 0 up only). The EK1100 still links both ports.
        check((chain.slave(0).get_u16(0x0110) & (1u << 5)) != 0,
              "EK1100 links downstream port 1");
        const std::uint16_t dl1 = chain.slave(1).get_u16(0x0110);
        check((dl1 & (1u << 4)) != 0, "EL1008 (middle) links upstream port 0");
        check((dl1 & (1u << 5)) != 0, "EL1008 (middle) now links downstream port 1");
        const std::uint16_t dl2 = chain.slave(2).get_u16(0x0110);
        check((dl2 & (1u << 4)) != 0, "EL2008 links upstream port 0");
        check((dl2 & (1u << 5)) == 0, "EL2008 tail has no downstream link");

        // Identity: an SII read of slave 2's product-code word (0x0A) returns the
        // EL2008 code 0x07D8_3052. Auto-increment adp 0xFFFE addresses slave 2
        // (slave 0 sees 0xFFFE, slave 1 0xFFFF, slave 2 0x0000).
        auto eep = make_frame(kApwr, 0xFFFE, 0x0502,
                              {0x00, 0x01, 0x0A, 0x00, 0x00, 0x00});
        check(chain.process_frame(eep), "EEPROM product-code read on EL2008 serviced");
        check(chain.slave(2).get_u16(0x0508) == 0x3052, "EL2008 SII low word = 0x3052");
        check(chain.slave(2).get_u16(0x050A) == 0x07D8, "EL2008 SII high word = 0x07D8 (2008)");
    }

    // --- EL2008 output round-trip via a write FMMU (Milestone 6, AC-04) ---
    {
        constexpr std::uint8_t kLwr = 11;
        Chain chain(3);
        Esc& el = chain.slave(2);  // EL2008

        // No output FMMU yet -> output_byte is empty.
        check(!chain.output_byte(2).has_value(),
              "output_byte empty before an output FMMU is configured");

        // Configure FMMU 0 on the EL2008 as a WRITE (output) FMMU: logical
        // 0x00020000, length 1, physical 0x0F00, type write (0x02), active.
        el.set_u32(0x0600, 0x00020000);  // logical start
        el.set_u16(0x0604, 0x0001);      // length
        el.set_u16(0x0608, 0x0F00);      // physical start
        el.set_u8(0x060B, 0x02);         // type = write (output)
        el.set_u8(0x060C, 0x01);         // activate

        // Master LWR of logical 0x00020000, 1 byte = 0x5A (the DO image).
        auto lwr = make_frame(kLwr, 0x0000, 0x0002, {0x5A});  // address 0x00020000
        check(chain.process_frame(lwr), "LWR serviced via output FMMU");
        check(wkc_of(lwr, 1) == 1, "LWR working counter == 1 (one output slave)");
        auto out = chain.output_byte(2);
        check(out.has_value() && *out == 0x5A,
              "output_byte reflects the DO byte the master wrote");

        // Inputs are untouched: only the EL1008 (slave 1) carries a read FMMU in
        // the field, so the EL2008's read side stays empty.
        check(!chain.set_process_input(2, 0x11),
              "set_process_input fails on the EL2008 (no read FMMU)");
    }

    // --- CoE mailbox round-trip via SM0/SM1 (Milestone 5, step 2) ---
    {
        Chain chain(3);
        Esc& dev = chain.slave(2);  // give slave 2 an Object Dictionary (mock CoE device)
        dev.od.add(0x1018, 1, 0x00000002, 4, false);  // identity: vendor id (read-only)
        dev.od.add(0x8000, 0x11, 0, 2, true);         // a writable 16-bit setting

        // Master configures the mailbox SyncManagers: SM0 (out) @0x1000 len 128,
        // SM1 (in) @0x1080 len 128.
        dev.set_u16(0x0800, 0x1000); dev.set_u16(0x0802, 0x0080);
        dev.set_u16(0x0808, 0x1080); dev.set_u16(0x080A, 0x0080);

        // Mailbox frame: SDO upload request for 0x1018:01 (vendor id).
        std::vector<std::uint8_t> upload = {
            0x0A, 0x00,             // mailbox data length = 10
            0x00, 0x00,             // address
            0x00,                   // channel/priority
            0x13,                   // mailbox type = CoE, sequence counter = 1
            0x00, 0x20,             // CoE header: SDO request
            0x40,                   // SDO upload command
            0x18, 0x10,             // index 0x1018
            0x01,                   // sub-index 1
            0x00, 0x00, 0x00, 0x00  // data
        };
        // Auto-increment adp 0xFFFE addresses slave 2; write lands on SM0 (0x1000)
        // and the write covering 0x1000 triggers the mailbox service.
        auto wr = make_frame(kApwr, 0xFFFE, 0x1000, upload);
        check(chain.process_frame(wr), "mailbox-out write serviced");
        const auto& r = chain.slave(2).registers;
        // The reply and the "mailbox full" signal go up together (atomically), so a
        // fixed-cycle master reading the mailbox sees the staged reply and the full
        // bit in a single read.
        check(std::to_integer<unsigned>(r[0x1088]) == 0x43,
              "upload response staged in SM1 (control 0x43)");
        check(std::to_integer<unsigned>(r[0x108C]) == 0x02,
              "upload response carries the vendor id");
        // Mailbox header byte 5 (SM1 offset 5 -> 0x1085): low nibble CoE type 3,
        // bits 4..6 echo the request's sequence counter (ETG.1000.4). This request
        // carried counter 1, so the reply's header byte is 0x13.
        check(std::to_integer<unsigned>(r[0x1085]) == 0x13,
              "reply echoes the request's mailbox counter 1 (header byte 0x13)");
        check((std::to_integer<unsigned>(r[0x080D]) & 0x08) != 0,
              "SM1 mailbox-full set in the same step as the reply");
        check((chain.slave(2).get_u32(0x0220) & 0x200u) != 0,
              "AL Event Request SM1 bit set so the master knows to read the reply");

        // Master reads the mailbox-in buffer (SM1 @0x1080, length 0x80). A real
        // master reads the whole SM; the "mailbox full" bit clears on the read of
        // the SM's last byte (0x10FF), so the read must span the full 128 bytes.
        auto rd = make_frame(kAprd, 0xFFFE, 0x1080, std::vector<std::uint8_t>(128, 0));
        check(chain.process_frame(rd), "mailbox-in read serviced");
        check(data_byte(rd, 8) == 0x43, "master reads SDO upload response control 0x43");
        check(data_byte(rd, 12) == 0x02, "master reads the vendor id via CoE");
        check((std::to_integer<unsigned>(r[0x080D]) & 0x08) == 0,
              "SM1 mailbox-full cleared once the master reads the response");
        check((chain.slave(2).get_u32(0x0220) & 0x200u) == 0,
              "AL Event Request SM1 bit cleared once the master reads the reply");

        // SDO download: master writes 0x8000:0x11 = 0x00AA -> OD updated.
        std::vector<std::uint8_t> download = {
            0x0A, 0x00, 0x00, 0x00, 0x00, 0x23, 0x00, 0x20,  // byte 5: CoE + counter 2
            0x2B,                   // SDO download, 2 bytes
            0x00, 0x80,             // index 0x8000
            0x11,                   // sub-index 0x11
            0xAA, 0x00, 0x00, 0x00  // value 0x00AA
        };
        auto dn = make_frame(kApwr, 0xFFFE, 0x1000, download);
        check(chain.process_frame(dn), "mailbox download serviced");
        const ecat::coe::Entry* e = chain.slave(2).od.find(0x8000, 0x11);
        check(e != nullptr && e->value == 0x00AA, "CoE download updated the Object Dictionary");
        check(std::to_integer<unsigned>(r[0x1088]) == 0x60,
              "download response staged in SM1 (control 0x60)");
        // Second request carries counter 2 -> reply echoes it -> header byte 0x23.
        check(std::to_integer<unsigned>(r[0x1085]) == 0x23,
              "reply echoes the request's mailbox counter 2 (header byte 0x23)");

        // A slave without an OD ignores mailbox writes entirely.
        chain.slave(1).set_u16(0x0800, 0x1000); chain.slave(1).set_u16(0x0802, 0x0080);
        chain.slave(1).set_u16(0x0808, 0x1080);
        auto none = make_frame(kApwr, 0xFFFF, 0x1000, upload);  // adp 0xFFFF -> slave 1
        (void)chain.process_frame(none);
        check((std::to_integer<unsigned>(chain.slave(1).registers[0x080D]) & 0x08) == 0,
              "a slave with no Object Dictionary does not service mailbox traffic");
    }

    // --- EL3001 (slave 3): CoE-capable analog terminal (Milestone 5, step 3) ---
    {
        Chain chain(4);  // EK1100 + EL1008 + EL2008 + EL3001
        Esc& ai = chain.slave(3);

        // Identity in the SII: EL3001 product 0x0BB9_3052; mailbox declared CoE.
        check(ai.eeprom[0x0A] == 0x3052 && ai.eeprom[0x0B] == 0x0BB9,
              "EL3001 SII product code = 0x0BB93052");
        check(ai.eeprom[0x1C] == 0x0004, "EL3001 SII declares the CoE mailbox protocol");
        check(!ai.od.empty() && ai.od.find(0x1018, 1) != nullptr,
              "EL3001 has a CoE Object Dictionary with an identity object");

        // Master programs the mailbox SMs (as TwinCAT does in PreOp).
        ai.set_u16(0x0800, 0x1000); ai.set_u16(0x0802, 0x0080);
        ai.set_u16(0x0808, 0x1080); ai.set_u16(0x080A, 0x0080);

        // SDO upload of 0x1018:02 (product code) over the mailbox. Auto-increment
        // adp 0xFFFD addresses slave 3 (of four).
        std::vector<std::uint8_t> up = {
            0x0A, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x20,
            0x40, 0x18, 0x10, 0x02, 0x00, 0x00, 0x00, 0x00};
        auto wr = make_frame(kApwr, 0xFFFD, 0x1000, up);
        (void)chain.process_frame(wr);
        auto rd = make_frame(kAprd, 0xFFFD, 0x1080, std::vector<std::uint8_t>(128, 0));
        (void)chain.process_frame(rd);
        const std::uint32_t product =
            static_cast<std::uint32_t>(data_byte(rd, 12)) |
            (static_cast<std::uint32_t>(data_byte(rd, 13)) << 8) |
            (static_cast<std::uint32_t>(data_byte(rd, 14)) << 16) |
            (static_cast<std::uint32_t>(data_byte(rd, 15)) << 24);
        check(data_byte(rd, 8) == 0x43, "EL3001 SDO upload response control = 0x43");
        check(product == 0x0BB93052, "EL3001 returns its product code via CoE (0x1018:02)");

        // SDO download a writable channel setting 0x8000:11 = 0x0064.
        std::vector<std::uint8_t> dn = {
            0x0A, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x20,
            0x2B, 0x00, 0x80, 0x11, 0x64, 0x00, 0x00, 0x00};
        auto wr2 = make_frame(kApwr, 0xFFFD, 0x1000, dn);
        (void)chain.process_frame(wr2);
        const ecat::coe::Entry* setting = chain.slave(3).od.find(0x8000, 0x11);
        check(setting != nullptr && setting->value == 0x0064,
              "EL3001 CoE download updates a writable setting (0x8000:11)");

        // PDO-config objects the master writes during PreOp->SafeOp must exist
        // (writable) so the SDOs don't abort and the input SM gets set up.
        const ecat::coe::Entry* rxpdo = chain.slave(3).od.find(0x1C12, 0);
        check(rxpdo != nullptr && rxpdo->writable,
              "EL3001 OD has a writable RxPDO-assign (0x1C12:00)");
        const ecat::coe::Entry* txpdo = chain.slave(3).od.find(0x1C13, 1);
        check(txpdo != nullptr && txpdo->value == 0x1A00,
              "EL3001 TxPDO-assign (0x1C13:01) maps PDO 0x1A00");
        check(chain.slave(3).od.find(0x1C00, 4) != nullptr,
              "EL3001 OD has the SM communication-type object (0x1C00)");

        // The downstream-link topology stays correct: EL2008 is now a middle
        // terminal, EL3001 is the tail.
        check((chain.slave(2).get_u16(0x0110) & (1u << 5)) != 0,
              "EL2008 (now middle) links downstream port 1");
        check((chain.slave(3).get_u16(0x0110) & (1u << 5)) == 0,
              "EL3001 tail has no downstream link");
    }

    // --- EL6224 (slave 4): CoE IO-Link master, mailbox-in at 0x1100 (Milestone 7) ---
    {
        Chain chain(5);  // EK1100 + EL1008 + EL2008 + EL3001 + EL6224
        Esc& iol = chain.slave(4);

        // Identity in the SII: EL6224 product 0x1850_3052; mailbox declared CoE.
        check(iol.eeprom[0x0A] == 0x3052 && iol.eeprom[0x0B] == 0x1850,
              "EL6224 SII product code = 0x18503052");
        check(iol.eeprom[0x1C] == 0x0005,
              "EL6224 SII declares CoE + AoE mailbox protocols (0x0005)");
        check(!iol.od.empty() && iol.od.find(0x1018, 1) != nullptr,
              "EL6224 has a CoE Object Dictionary with an identity object");

        // Master programs the mailbox SMs. The EL6224's mailbox-in sits at 0x1100
        // (not 0x1080), exercising the live-address mailbox handling.
        iol.set_u16(0x0800, 0x1000); iol.set_u16(0x0802, 0x0100);
        iol.set_u16(0x0808, 0x1100); iol.set_u16(0x080A, 0x0100);

        // SDO upload of 0x1018:02 (product code). adp 0xFFFC addresses slave 4 of five.
        std::vector<std::uint8_t> up = {
            0x0A, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x20,
            0x40, 0x18, 0x10, 0x02, 0x00, 0x00, 0x00, 0x00};
        auto wr = make_frame(kApwr, 0xFFFC, 0x1000, up);
        (void)chain.process_frame(wr);
        auto rd = make_frame(kAprd, 0xFFFC, 0x1100, std::vector<std::uint8_t>(128, 0));
        (void)chain.process_frame(rd);
        const std::uint32_t product =
            static_cast<std::uint32_t>(data_byte(rd, 12)) |
            (static_cast<std::uint32_t>(data_byte(rd, 13)) << 8) |
            (static_cast<std::uint32_t>(data_byte(rd, 14)) << 16) |
            (static_cast<std::uint32_t>(data_byte(rd, 15)) << 24);
        check(data_byte(rd, 8) == 0x43, "EL6224 SDO upload response control = 0x43");
        check(product == 0x18503052,
              "EL6224 returns its product code via CoE at mailbox-in 0x1100 (0x1018:02)");

        // Default process image: the 4-byte DeviceState TxPDO (input-only).
        const ecat::coe::Entry* txcount = iol.od.find(0x1C13, 0);
        check(txcount != nullptr && txcount->value == 5,
              "EL6224 TxPDO-assign has 5 PDOs (per-port 0x1A00..03 + DeviceState)");
        const ecat::coe::Entry* txpdo1 = iol.od.find(0x1C13, 1);
        check(txpdo1 != nullptr && txpdo1->value == 0x1A00,
              "EL6224 TxPDO-assign (0x1C13:01) maps per-port PDO 0x1A00");
        const ecat::coe::Entry* txpdo5 = iol.od.find(0x1C13, 5);
        check(txpdo5 != nullptr && txpdo5->value == 0x1A04,
              "EL6224 TxPDO-assign (0x1C13:05) maps DeviceState PDO 0x1A04");
        check(iol.od.find(0x1A04, 4) != nullptr,
              "EL6224 TxPDO 0x1A04 maps all four port-state entries");
        check(iol.od.find(0x1A00, 0) != nullptr && iol.od.find(0x1A00, 0)->writable,
              "EL6224 per-port TxPDO 0x1A00 exists and is writable (configurable)");
        check(iol.od.find(0x1C12, 4) != nullptr,
              "EL6224 RxPDO-assign 0x1C12 carries the four per-port output PDOs");
        check(iol.od.find(0xF100, 4) != nullptr,
              "EL6224 has the DeviceState object 0xF100 (per-port status)");
        const ecat::coe::Entry* rxpdo = iol.od.find(0x1C12, 0);
        check(rxpdo != nullptr && rxpdo->writable,
              "EL6224 OD has a writable RxPDO-assign (0x1C12:00)");

        // Topology: EL3001 is now a middle terminal; EL6224 is the tail.
        check((chain.slave(3).get_u16(0x0110) & (1u << 5)) != 0,
              "EL3001 (now middle) links downstream port 1");
        check((chain.slave(4).get_u16(0x0110) & (1u << 5)) == 0,
              "EL6224 tail has no downstream link");
    }

    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
