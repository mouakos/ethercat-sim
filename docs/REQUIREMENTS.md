# Requirements — EtherCAT / IO-Link / CoE Hardware Simulator

**Document status:** Draft v0.3 — TE1111 removed; single-path commit
**Last updated:** 2026-05-21
**Owner:** _(to fill in)_

---

## 1. Purpose

Build a software simulator that mimics a **complete EtherCAT hardware configuration** — including all terminals, IO-Link master with downstream IO-Link devices, and CoE-configurable devices — so that a real Beckhoff TwinCAT 3 PLC running on an IPC or laptop can talk to the simulator over EtherCAT **as if it were physical hardware**. The PLC will have a normal, unmodified EtherCAT configuration; it must not know the slaves are simulated.

## 2. Goals

1. Run a real Beckhoff TwinCAT 3 PLC on an IPC or laptop with an unmodified EtherCAT configuration.
2. Provide a simulator that presents itself on the wire as one or more EtherCAT slaves, matching the ESI/ENI the PLC expects.
3. Simulate **all terminals** in a representative station: bus coupler, digital I/O, analog I/O, IO-Link master with a downstream IO-Link temperature sensor, and one CoE-configurable drive (CiA 402 minimum).
4. Provide a way to script device behavior (process data values, alarms, CoE object responses, IO-Link events).
5. Keep the language stack pragmatic: **C++**, **C#**, and/or **Python**. Mix is allowed; pick per layer based on fit.
6. Real cable: simulator on one PC (this development PC), PLC on another, real Ethernet cable between them. Same-host operation (PLC and simulator on one machine, e.g. via two NICs or a virtual NIC pair) is a supported variant — same code, two NICs.

## 3. Non-Goals (initial)

- Bit-accurate timing of the EtherCAT DC clocks. We will follow well enough to enter Op and stay there; nanosecond-precise DC is not on the table for a pure-software slave.
- Simulating physical-layer faults (broken cables, CRC storms) in v1.
- Safety-over-EtherCAT (FSoE). Out of scope until explicitly added.
- A graphical engineering tool to author ENI files — we consume them.
- Selling or distributing the simulator as a commercial EtherCAT product (would require ETG membership, Vendor ID, conformance testing — none of which we are pursuing for internal use).
- Using Beckhoff's TE1111 EtherCAT Simulation product. **Explicitly excluded** — that path is being explored elsewhere and is not this project.

## 4. In-Scope Devices

### 4.1 Phase 1 (day-one station)

| ID | Class | Model imitated | Notes |
|---|---|---|---|
| 1 | Bus coupler | EK1100-class | Entry point of the station |
| 2 | Digital input | EL1008-class (8 DI) | Static + scripted inputs |
| 3 | Digital output | EL2008-class (8 DO) | Reflect PLC writes back to a sink |
| 4 | Analog input | EL3001/3002-class | Configurable generator (ramp, sine, fixed) |
| 5 | IO-Link master | EL6224 (4 ports) | One port populated, three idle |
| 5a | IO-Link device | Generic temperature sensor | PD: temperature; ISDU: scale, offset, range |

### 4.2 Phase 2

| ID | Class | Model imitated | Notes |
|---|---|---|---|
| 6 | CoE drive | AX5000-like (CiA 402) | Full PDS/402 state machine |
| 7 | Analog output | EL4001/4002-class | Capture PLC values |

### 4.3 Future (not committed)

- Junction terminals (EK1521 / EK1122) for branching topologies
- Additional IO-Link devices (RFID, valve manifold, pressure)
- Encoder, more drives
- Hot-connect groups

## 5. Functional Requirements

### 5.1 EtherCAT slave behavior

- FR-EC-01: Implement the EtherCAT slave state machine (Init → PreOp → SafeOp → Op).
- FR-EC-02: Accept and respond to EtherCAT mailbox protocols required by the modeled devices (CoE mandatory; EoE optional).
- FR-EC-03: Expose the correct EEPROM / SII content so the master identifies vendor ID, product code, revision matching Beckhoff's ESI for the imitated terminal.
- FR-EC-04: Honor PDO mapping configured by the master via SDO writes (when applicable).
- FR-EC-05: Provide process data updates synchronized to the master cycle (best effort; target jitter documented in Architecture §7).
- FR-EC-06: Distributed Clocks: implement enough of the DC mechanism for TwinCAT to enter Op and stay there. Phase-accurate DC is a non-goal.
- FR-EC-07: Respond to the master's Auto-Increment and Configured-Address datagrams correctly, in chain order, so topology scan succeeds.

### 5.2 IO-Link behavior

- FR-IOL-01: For each simulated IO-Link master port, model port mode (IO-Link / DI / DO / disabled).
- FR-IOL-02: Provide ISDU (parameter) read/write for each downstream device.
- FR-IOL-03: Generate cyclic process data per device profile.
- FR-IOL-04: Generate events (warnings, errors) on script command.
- FR-IOL-05: For phase 1, support one temperature sensor returning a single 16-bit signed temperature in 0.1 °C steps, plus events on out-of-range.

### 5.3 CoE device behavior

- FR-COE-01: Maintain an object dictionary per device with proper data types and access rights.
- FR-COE-02: Support PDO assign/mapping objects (0x1C12 / 0x1C13 and the 0x1600 / 0x1A00 ranges).
- FR-COE-03: Support common profile state machines where modeled. For the phase-2 drive, full CiA 402 (Not Ready → Switch On Disabled → Ready To Switch On → Switched On → Operation Enabled, plus Quick Stop and Fault).

### 5.4 Configuration & scripting

- FR-CFG-01: Load station topology from a config file (YAML preferred) describing every slave and its initial state.
- FR-CFG-02: Optionally consume the master's exported ENI/XTI to auto-derive what each slave must claim to be.
- FR-CFG-03: Provide a scripting hook (Python preferred) to mutate process data and trigger events at runtime.
- FR-CFG-04: Allow recording and replay of an I/O session.

### 5.5 Observability

- FR-OBS-01: Log all mailbox traffic at a configurable verbosity.
- FR-OBS-02: Expose a live view (CLI or simple web UI) of current PDO values per slave.
- FR-OBS-03: Emit metrics: cycle time observed, missed cycles, mailbox errors.

### 5.6 Test harness

- FR-TST-01: Provide a headless EtherCAT master stand-in (built on SOEM / pysoem) that drives the simulator without TwinCAT, for CI.
- FR-TST-02: Provide a real-PLC test rig where a TwinCAT laptop talks to the simulator over a real cable, for acceptance gates.
- FR-TST-03: Test cases written in Python, using pysoem for the headless rig and ADS (pyads) for talking into the TwinCAT runtime on the PLC laptop.

## 6. Non-Functional Requirements

- NFR-01: Simulator runs on Linux (Ubuntu 22.04+ preferred for raw-socket simplicity) and on Windows 10/11 (via Npcap). Linux is the primary development target.
- NFR-02: PLC and simulator on two machines on the same cable (primary). Same-host operation with two NICs is a supported variant.
- NFR-03: Startup-to-Op for the phase-1 station: under 30 seconds.
- NFR-04: Footprint: under 500 MB RAM for a 20-slave configuration.
- NFR-05: Build reproducibly from a single repo with documented commands.
- NFR-06: **Cycle-time target tiers** (TwinCAT defaults to 10 ms; project goal is to work at 10 ms and scale down where feasible):
  - **Tier 1 (must work):** 10 ms PLC cycle, sustained Op for 1 hour, zero Lost Frames.
  - **Tier 2 (should work):** 4 ms PLC cycle, sustained Op for 1 hour, zero Lost Frames.
  - **Tier 3 (stretch):** 1 ms PLC cycle, sustained Op for 1 hour, Lost-Frame rate < 1 in 10⁶.
  - Below 1 ms: explicitly not committed.

## 7. Constraints & Assumptions

- **PLC of record: Beckhoff TwinCAT 3** (XAE for engineering, XAR for runtime). All decisions optimize for TwinCAT compatibility first; portability to CODESYS or Acontis EC-Master is a future concern.
- TwinCAT real-time only works with **Intel NIC chipsets** (supported list maintained by Beckhoff). Realtek and other chipsets work in "demo mode" without real-time guarantees. The **PLC host must have a supported Intel NIC.** The simulator host has no such constraint — we use raw sockets / Npcap and supply our own timing.
- A physical Ethernet cable between PLC and simulator is required (or two NICs on one host wired together). EtherCAT is a Layer-2 daisy-chain protocol; switches break Distributed Clocks and topology scanning.
- We are free to choose the language per layer; interop between the C++ slave core and the device-model layer is local IPC (shared memory and/or gRPC/ZeroMQ — decision in Architecture §9).

## 8. Open Questions

> All v0.1 open questions resolved in HANDOVER.md §3. New questions surfaced during research and after the TE1111 exclusion below.

1. **L2/L3 cycle-time tier we commit to first.** Tier 1 (10 ms) is the must-work bar. Do we try to hit Tier 2 (4 ms) in v1, or defer until phase 2?
2. **Topology config format.** YAML proposed; confirm before writing the third device model.
3. **L4 device-model language.** C# or Python — prototype one of each (DI in C#, temp sensor in Python) and pick.
4. **IPC mechanism between the C++ slave core and the L4 service.** Shared memory for hot-path PD, gRPC for slow-path SDO/IO-Link is the default proposal; needs validation under load.
5. **Phase-1 IO-Link temperature sensor.** Generic profile (easier) vs a specific real device's IODD (more realistic). Sticking with generic for v1 unless a specific one becomes important.
6. **ENI ↔ topology consistency.** YAML topology as source of truth (we generate hints for the PLC project) or PLC project as source of truth (we ingest its `.xti` export)?

## 9. Acceptance Criteria

### 9.1 Phase 1 (v1.0)

- AC-01: A real TwinCAT 3 project, configured against the simulator's claimed topology, reaches Op state on all phase-1 slaves over a real Ethernet cable.
- AC-02: PLC holds Op for 1 hour at 10 ms cycle with zero Lost Frames.
- AC-03: Toggling a DI in the simulator is visible in the PLC within two PLC cycles.
- AC-04: Writing a DO from the PLC is visible in the simulator within two PLC cycles.
- AC-05: Reading and writing a CoE SDO from the PLC returns the simulator's object dictionary values.
- AC-06: The IO-Link temperature sensor delivers cyclic PD; the PLC can read it. An ISDU write from the PLC changes a sensor parameter and the PLC can read it back.
- AC-07: A scripted event (sensor out-of-range warning) is observable in the PLC's diagnostics.
- AC-08: The headless harness (pysoem-based) reaches Op against the simulator and runs a CI smoke test.

### 9.2 Phase 2

- AC-09: The CoE drive reaches "Operation Enabled" in CiA 402 from the PLC side.
- AC-10: Analog output written by the PLC is captured by the simulator.
- AC-11: Tier 2 cycle (4 ms) demonstrated for 1 hour with zero Lost Frames.

## 10. Glossary

- **EtherCAT** — Ethernet for Control Automation Technology, fieldbus by EtherCAT Technology Group.
- **CoE** — CANopen over EtherCAT, mailbox protocol for object dictionary access.
- **PDO / SDO** — Process Data Object (cyclic) / Service Data Object (acyclic, mailbox).
- **ENI** — EtherCAT Network Information, the master's config file.
- **ESI** — EtherCAT Slave Information, slave XML describing itself.
- **SII / EEPROM** — Slave Information Interface; slave's on-board identification.
- **ESC** — EtherCAT Slave Controller; the silicon (ET1100, ET1200, LAN9252, AX58100 …) that processes EtherCAT frames on the fly in real hardware. **We are emulating this in software** — the core challenge of the project.
- **DC** — Distributed Clocks; EtherCAT's synchronization mechanism.
- **WKC** — Working Counter; per-datagram counter the master uses to detect missing responses.
- **IO-Link** — Point-to-point serial link below the fieldbus, IEC 61131-9.
- **ISDU** — Indexed Service Data Unit; IO-Link parameter access.
- **IODD** — IO Device Description, XML describing an IO-Link device.
- **TwinCAT** — Beckhoff's Windows-based PLC/motion runtime. XAE = engineering, XAR = runtime.
- **ADS** — Automation Device Specification; Beckhoff's RPC/messaging system. We use it only as a test-side client to drive the PLC, not as part of the simulator itself.
- **SOEM** — Simple Open EtherCAT Master; open-source EtherCAT master library (GPLv3 or commercial). `pysoem` is the Python binding. Used in the headless harness only.
- **CiA 402** — CANopen drive profile (state machine + object dictionary) used by most EtherCAT servo drives.
- **IPC** — Industrial PC.

---

_Edit this file directly as we go. New requirements get an ID and a one-liner; open questions get resolved or escalated._
