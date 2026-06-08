# PLC Setup — TwinCAT 3 + EtherCAT Simulator

**Document status:** Draft v0.3 — Windows-native simulator host (aligned with GETTING_STARTED.md)
**Last updated:** 2026-05-21

> Step-by-step instructions to bring up a TwinCAT 3 PLC against our simulator over a real Ethernet cable. The simulator is our own software EtherCAT slave stack — no Beckhoff TE1111 product involved.

> The simulator runs **natively on Windows** using **Npcap** to bind a NIC raw, because the development PC already has Visual Studio 2026. WSL2 cannot reliably bind raw `AF_PACKET` sockets to a host NIC, so it is not used. A native Linux simulator host (with `AF_PACKET`) is documented as a future variant in §4b for when a separate Linux box becomes available.

---

## 1. Topology

```
+---------------------+                 +---------------------+
|   PLC laptop / IPC  |                 |  Simulator host     |
|---------------------|                 |---------------------|
|  Windows 10/11      |                 |  Windows 10/11      |
|  TwinCAT XAE + XAR  |                 |  Our slave-stack    |
|  Intel NIC          |==[Ethernet]====>|  Any NIC + Npcap    |
|  (TwinCAT RT driver)|                 |  (raw L2 access)    |
+---------------------+                 +---------------------+
```

A direct cable from the PLC laptop's EtherCAT NIC to the simulator host's NIC. **No switch in between** — EtherCAT is a daisy-chain Layer-2 protocol; a normal switch will break the topology and Distributed Clocks.

Same-host operation (PLC and simulator on one machine) is supported as a variant: install two physical NICs, install the TwinCAT RT driver on the one connected to the PLC project, leave the other free for the simulator to bind via Npcap. Cable the two NICs together.

---

## 2. Hardware checklist

- **PLC PC:** laptop or IPC with **Windows 10/11** and a **supported Intel NIC chipset** (TwinCAT's real-time driver requires this; Realtek/USB-Ethernet adapters work only in demo mode with poor timing). Examples that work: Intel I210, I211, I219, I225, I226; 82574L, 82579LM.
- **TwinCAT 3** (XAE engineering + XAR runtime). Download from beckhoff.com; the free 7-day repeatable trial of XAR is fine for development.
- **Simulator PC:** this development PC (the one with Visual Studio 2026 installed). Any NIC. **Windows 10/11**, with **Npcap** for raw L2 access. (Native Linux on a separate box, using `AF_PACKET`, stays an option for the future — see §4b.)
- **Ethernet cable** directly between the two NICs. Any Cat5e or better, normal RJ45.

> **Verify Intel NIC compatibility on the PLC side before doing anything else.** In TwinCAT XAE, menu **TwinCAT → Show Real Time Ethernet Compatible Devices…** A real-time–capable adapter should appear under "Installed and ready to use devices (realtime capable)". If it lands under "(for demo use only)", real-time is not guaranteed and you should expect timing problems.

---

## 3. PLC host: install TwinCAT 3

1. Download **TwinCAT 3.1 XAE** from Beckhoff: https://www.beckhoff.com/download → Automation → TwinCAT 3. Choose the latest stable build (4024 or newer).
2. Disable Secure Boot in BIOS (required for TwinCAT's kernel-mode real-time driver).
3. Install with default options. A Visual Studio integration installs if Visual Studio is present; otherwise TwinCAT XAE Shell (a standalone VS Shell) is used.
4. Reboot.
5. Install the TwinCAT RT Ethernet driver on the EtherCAT NIC:
   - Open TwinCAT XAE (or the standalone shell).
   - Menu: **TwinCAT → Show Real Time Ethernet Compatible Devices…**
   - Select your Intel NIC under "Compatible devices" and click **Install**.
   - Verify it now appears under "Installed and ready to use devices (realtime capable)".
6. Ensure the **TwinCAT XAR** runtime is licensed or in trial mode: **TwinCAT → System Manager → SYSTEM → License** — request the 7-day trial of any missing component (it can be re-requested indefinitely for development).
7. Optional but recommended on the PLC NIC properties (Windows Device Manager → NIC → Advanced):
   - Disable "Large Send Offload"
   - Disable "TCP/UDP Checksum Offload"
   - Disable "Interrupt Moderation"
   - Disable "Energy Efficient Ethernet"
   - These reduce timing jitter for real-time traffic.

---

## 4. Simulator host: prepare for raw EtherCAT

### 4a. Windows (primary — matches the VS2026 + Claude Code setup)

> Most of this is already done if you followed `GETTING_STARTED.md`. Recap here for completeness.

1. Install **Npcap** and the **Npcap SDK** from https://npcap.com/#download (if not already done in `GETTING_STARTED.md` §3c). Tick "Install Npcap in WinPcap API-compatible Mode" during install.
2. Install Wireshark (it picks up Npcap automatically once Npcap is installed).
3. Dedicate a NIC to EtherCAT on the simulator host. **Disable both IPv4 and IPv6 bindings** on that NIC: Network Adapter properties → uncheck "Internet Protocol Version 4 (TCP/IPv4)" and "...Version 6 (TCP/IPv6)". This avoids Windows confusing itself by trying to assign an IP to a NIC carrying only raw EtherCAT.
4. NIC offload settings — open Device Manager → Network adapters → the chosen NIC → Properties → Advanced. Disable:
   - Large Send Offload (V1 IPv4), Large Send Offload V2 (IPv4 and IPv6)
   - TCP/UDP Checksum Offload (IPv4 and IPv6) — both Tx and Rx
   - Interrupt Moderation (or set Rate to "Off")
   - Energy Efficient Ethernet
   - Receive Side Scaling can stay enabled
5. Run the simulator binary as Administrator (Npcap requires elevation to send raw frames). For convenience during dev, right-click PowerShell → "Run as administrator" once, and launch the binary from there. We will revisit this when we package for distribution.

### 4b. Linux on a separate box (future variant)

If a Linux PC becomes available and we want lower-jitter raw access via `AF_PACKET`:

1. Install build tools and Wireshark for debugging:
   ```
   sudo apt-get install build-essential cmake git wireshark tcpdump ethtool
   ```
2. Identify the NIC you will dedicate to EtherCAT — call it `eth1` here. **Do not** assign it an IP address; we use it as a raw L2 interface.
3. Bring the interface up without an address:
   ```
   sudo ip link set eth1 up
   sudo ip addr flush dev eth1
   ```
4. Offload tweaks for jitter:
   ```
   sudo ethtool -K eth1 tso off gso off gro off lro off
   sudo ethtool -G eth1 rx 256 tx 256       # if supported
   ```
5. Decide on simulator privilege model:
   - Run as root (simplest), or
   - Grant `CAP_NET_RAW` to the binary: `sudo setcap cap_net_raw,cap_net_admin=eip /path/to/simulator`
6. For Tier 2/3 cycle work consider a `PREEMPT_RT` kernel and pinning the slave-core thread to an isolated CPU.

### 4c. Note on WSL2

WSL2 is **not** used for the slave stack. It cannot reliably bind `AF_PACKET` sockets to a host NIC — frames are visible inside the WSL2 VM but do not flow to the physical LAN. Workarounds exist (mirrored networking, USB/IP) but each comes with caveats that do not pay back for our use case. Native Windows + Npcap is the clean choice on this PC. The headless pysoem harness can run inside WSL2 against a loopback or virtual-NIC pair if useful for fast iteration without involving the physical wire.

---

## 5. PLC host: create the phase-1 station

> This is the "what the PLC thinks is there" — a normal TwinCAT project with the real terminals defined. The PLC does not know they are simulated.

Phase-1 station (matches REQUIREMENTS §4.1):

```
Term 1: EK1100 (Bus Coupler)
Term 2: EL1008 (8 DI)
Term 3: EL2008 (8 DO)
Term 4: EL3001 (1 AI, ±10 V)
Term 5: EL6224 (4-channel IO-Link master)
         Port 1: Generic IO-Link temperature sensor
         Ports 2-4: unused
```

Steps:

1. Open TwinCAT XAE. **File → New → TwinCAT Project**. Name it `SimRig01`.
2. Set the target system to **Local**. The system tray icon should be blue/green for Config / Run mode.
3. Right-click **I/O → Devices → Add New Item…**
4. Choose **EtherCAT → EtherCAT Master**. Pick the Intel NIC dedicated to EtherCAT (not the one used for SSH/Engineering).
5. Right-click the new EtherCAT device → **Add New Item… → Beckhoff Automation → EtherCAT Terminals → System → EK1100**.
6. Right-click the EK1100 → **Add New Item…** for each terminal in the list above (EL1008, EL2008, EL3001, EL6224) in order.
7. Configure the EL6224:
   - Select the EL6224 in the tree, go to the **IO-Link** tab.
   - On Port 1, right-click → **Create Device** (or use IODD Finder if you have a specific sensor's IODD).
   - For a generic temperature sensor: Vendor ID = `0x0001` (placeholder), Process length = 16 bit (signed), name "GenericTempSensor".
   - In Settings: **uncheck** "Check VendorID" and "Check DeviceID" for now; set Communication mode to Communication.
8. Set the **task cycle time**: in the EtherCAT Master → Sync Unit / under the Task that drives the master, set 10 ms cycle for the initial bring-up. Move to 4 ms later (Tier 2) and 1 ms only after Milestone 8.
9. **Build → Build Solution**.
10. **Do not Activate Configuration yet.** First confirm the simulator side is running.

---

## 6. Wiring the cable and verifying link

1. Power the simulator host with the dedicated NIC up but unbound (per §4).
2. Plug the Ethernet cable from the PLC laptop's EtherCAT NIC to the simulator host's dedicated NIC.
3. On the simulator host (Windows), confirm the NIC is up and link is detected:
   ```powershell
   Get-NetAdapter | Where-Object { $_.Status -eq 'Up' }
   ```
   The chosen EtherCAT NIC should appear with Status `Up` and LinkSpeed `100 Mbps` (EtherCAT runs at 100 Mbit/s; gigabit cards auto-negotiate down).
   On Linux variant (§4b), `ip link show eth1` instead — expect state UP, no IP.
4. On the PLC laptop, in TwinCAT XAE: select the EtherCAT Master → **Adapter** tab. The chosen NIC should show "Link" present (the row that displays carrier status). If no link, check the cable and that both NICs are up.

At this point, no EtherCAT frames are flying yet — TwinCAT only sends them after Activate Configuration. We use this point as Milestone 0 confirmation: physical link present.

---

## 7. Milestone 1 — Wire-level smoke test

> Goal: with no responses from us yet, watch what TwinCAT sends on the wire.

1. On the simulator host, start a Wireshark capture on the dedicated EtherCAT NIC filtered to `eth.type == 0x88a4` (the EtherCAT EtherType). On Windows the NIC appears under its Windows-assigned name (e.g. "Ethernet 2"); on Linux it is the `eth1`-style interface name from §4b.
2. On the PLC host in TwinCAT, **Activate Configuration** and acknowledge "OK (load I/O devices)" — TwinCAT will not be able to reach Op because we are not responding, but it will start sending master frames.
3. On the simulator host, observe in Wireshark:
   - Initial **broadcast read** frames asking each position on the chain to identify itself.
   - **Auto-Increment-Read** (APRD) and **Configured-Address-Read** (FPRD) datagrams.
   - Likely repeated retries because no slave is answering.
4. Pass condition: Wireshark shows the expected EtherCAT broadcast and read frames.
5. Stop TwinCAT (Config Mode) before moving on so the wire goes quiet.

This validates the cable, the NIC bindings, and gives us a trace of exactly what to respond to.

---

## 8. Milestone 2–3 — Slave reaches OP

> These are implementation milestones for the slave stack — see HANDOVER.md §4. From the PLC's perspective, here is how you watch progress.

In TwinCAT XAE while the simulator is running:

1. Select the EtherCAT Master in the tree → **Online** tab. The bottom shows slaves by configured position.
2. **Milestone 2 pass condition:** the slave appears as recognized (vendor ID + product code identified), state column reads `1` (Init).
3. **Milestone 3 pass condition:** state column reads `8` (Op), error column empty, Lost Frames counter does not climb over 60 seconds at 10 ms cycle.
4. If the slave stays in `1` or oscillates, double-click the slave → **Online** sub-tab → read the **AL Status Code** register. That code, looked up in the ETG.1000 spec or Beckhoff docs, tells you why the transition failed.

---

## 9. Milestone 4 — Force values to verify PD round-trip

1. On the PLC project's DI terminal (EL1008): select it → **Inputs** tab → expand. Bits should be visible.
2. On the simulator side, programmatically (or via your CLI) toggle the DI bit. The corresponding bit in TwinCAT's Online tab should flip.
3. On the PLC project's DO terminal (EL2008): select Outputs → right-click a bit → **Online Write 1**. The simulator should observe the bit set in its DO mirror.

---

## 10. Milestone 5 — CoE SDO round-trip

1. In the PLC project, select a slave that has CoE objects (e.g. EL3001 or EL6224) → **CoE - Online** tab.
2. Click **Update list**. TwinCAT issues SDO reads for each entry; values should populate.
3. Right-click an entry → **Write…** to test SDO writes.
4. Read-back to confirm.

---

## 11. Soak test (AC-02)

1. Set the PLC task cycle to 10 ms (already the default).
2. Let the configuration run for 1 hour.
3. Watch:
   - EtherCAT Master Online tab → **Lost Frames** counter stays at zero
   - All slaves stay in state `8` (Op)
   - No mailbox errors in the slave's Online sub-tab
4. Capture a 1-minute Wireshark trace at the start and at the 50-minute mark for evidence.

---

## 12. Optional: drive the PLC from tests (pyads)

Once the system is alive, the test rig drives the PLC laptop's TwinCAT from a Python test runner:

```python
import pyads, time

# AMS NetID of the PLC laptop's TwinCAT, port 851 = PLC runtime
plc = pyads.Connection("5.84.21.42.1.1", 851)
plc.open()

# Read a PLC variable mapped to the EL1008 first input
v = plc.read_by_name("GVL.DI[0]", pyads.PLCTYPE_BOOL)
assert v in (True, False)

# Write a PLC variable mapped to the EL2008 first output
plc.write_by_name("GVL.DO[0]", True, pyads.PLCTYPE_BOOL)

plc.close()
```

ADS works over TCP port 48898. From the simulator host, add a route in TwinCAT XAE → **Tools → Edit Routes** → Add the PLC laptop's IP and the AMS NetID Beckhoff assigned. Or run pyads on the PLC laptop itself for the simplest setup.

---

## 13. Troubleshooting cheat sheet

| Symptom | Likely cause | Action |
|---|---|---|
| NIC missing from TwinCAT compatible devices | Not an Intel chipset, or RT driver not installed | Install via **Show Real Time Ethernet Compatible Devices**. If still missing, the chipset is not supported. |
| Wireshark capture is empty on milestone 1 | Cable not plugged into the NIC TwinCAT is bound to, or wrong NIC chosen in TwinCAT | In TwinCAT EtherCAT Master → Adapter tab, click **Search…** and reselect. Confirm Link Status shows present. |
| Simulator binary can't open the NIC | Insufficient privileges, or NIC managed by NetworkManager (Linux) | Run as root or `setcap CAP_NET_RAW`. On Linux, `nmcli dev set eth1 managed no`. |
| Master reaches PreOp but not SafeOp | PDO mapping mismatch | Compare PDO assignments between the PLC project's slave and our emulated EEPROM/SDO responses. They must agree on PDO sizes. |
| Lost Frames climbing | NIC offload features interfering, or jitter exceeding tolerated window | In Windows Device Manager → NIC properties → disable TCP/UDP checksum offload, large send offload, interrupt moderation. On Linux, `ethtool -K` as in §4a. |
| Slaves stuck in INIT | EEPROM (SII) read failing or returning wrong vendor/product | Capture a Wireshark trace, find the EEPROM read datagrams, verify our responses against Beckhoff's ESI XML for the imitated terminal. |
| TwinCAT trial license expired | 7-day trial elapsed | Request a new trial (it can be repeated indefinitely for development). |
| PLC sees only the coupler, no terminals behind it | E-bus chain modeling wrong on the simulator | Topology scan walks slaves in port order. Verify our chain order matches the PLC project exactly and that port wiring (in/out) is set right per slave. |
| Same-host setup, both NICs visible but no link | TwinCAT RT driver bound to both NICs, or both have the same IP | Bind RT driver only to the PLC-side NIC. Leave the simulator-side NIC unbound from RT and unbound from IP. |

---

_End of PLC setup._
