# Handover — EtherCAT / IO-Link / CoE Hardware Simulator

**Document status:** Draft v0.19 — M5 done (EL3001 holds OP). **MILESTONE 7 — EL6224. AoE NetId-download ack VERIFIED (PR #54; 42-byte reply collected at SM1 0x1100). Configurable per-port PDOs modelled (PR #55). ROOT CAUSE NARROWED via `--log` with an active OP click: it's a TWO-CoE-SLAVE mailbox-collection bug, NOT missing PDO entries. Sequence: TwinCAT clears `0x1c12:00`/`0x1c13:00` on the EL3001 (replies staged + collected at 0x1080) and `0x1a00:00` on the EL6224 (reply staged at 0x1100) — but the master then LRD-polls mailbox-status `0x08` (full) and reads the EL3001's `0x1080`, NEVER the EL6224's `0x1100`, so the EL6224's clear reply is never collected → `download pdo 0x1A00 entries` times out → PREOP. The EL6224 alone collected its AoE reply at 0x1100 fine; it only starves once BOTH CoE mailboxes are full at once → almost certainly both slaves' mailbox-full bit (`0x080D`) maps into the SAME logical LRD byte, so the master can't tell which is full. RESUME: capture `el6224_pdo.pcapng` (start `--serve <dev> --slaves 5 --log` + dumpcap, click `Op`, ~30 s), then decode the MBoxState FMMU writes (FPWR `0x0610`) for BOTH the EL3001 (station 0x03ec) and EL6224 (0x03ed) — compare their `Log Start` addresses; if equal, that's the collision. Fix = ensure each CoE slave's mailbox-full is independently visible (distinct logical byte / correct per-slave FMMU LRD handling). `--slaves 4` = working EL3001 chain. `--slaves N` (#53), `--log` (#57).** §3 (2026-06-07) + §5.6
**Last updated:** 2026-06-07
**Intended audience:** The engineer (you or a teammate) picking this up on the PC.

> Single source of truth for "where are we." State, decisions, what's done, what's next, and how to resume.

---

## 1. Project in one paragraph

We are building a simulator that pretends to be a full EtherCAT station — coupler, digital and analog I/O, an IO-Link master with downstream IO-Link devices, and CoE-configurable devices like drives. A real **Beckhoff TwinCAT 3** PLC running on an IPC or laptop talks to the simulator over a **real Ethernet cable**. The simulator is our own software EtherCAT slave stack written in C++, with device behavior modeled in C# and/or Python on top. A headless test harness built on SOEM lets us regression-test without a TwinCAT laptop. **We are not using Beckhoff's TE1111 simulation product** — that path is being explored separately.

## 2. Current state

- ☑ Requirements doc seeded and updated to v0.3 (`REQUIREMENTS.md`)
- ☑ Architecture doc seeded and updated to v0.4 (`ARCHITECTURE.html`)
- ☑ Handover doc seeded and updated to v0.4 (this file)
- ☑ PLC setup doc updated to v0.3 (`PLC_SETUP.md`)
- ☑ Getting-started doc added at v0.1 (`GETTING_STARTED.md`) — Claude Code + toolchain setup
- ☑ All v0.1 open questions resolved (see §3 decisions log)
- ☑ TE1111 / Mode-A path explicitly removed
- ☑ Simulator host OS chosen: Windows native + Npcap (aligned with the dev PC's VS2026)
- ☑ Toolchain installed on the dev PC per `GETTING_STARTED.md` §3 (VS2026/MSVC v145, CMake 4.3, Ninja, Python 3.13, Git, GitHub CLI, Npcap + SDK)
- ☑ Repo created on GitHub (`roberthendriksen/ethercat-sim`, private); CLAUDE.md, .gitignore, docs, and skeleton dirs committed
- ☑ First Claude Code session run and a buildable skeleton committed — CMake + Npcap-linked `ec-core` (Stage 0, PR #2)
- ☐ Language-per-layer assignments confirmed in code (current proposals in Architecture §3)
- ☑ "Wire smoke test" (Milestone 1) — captured the EtherCAT broadcast frames TwinCAT sends (Wireshark/tshark). Finding: TwinCAT loops **BRD of AL Status (`0x0130`)** at ~200/s, WKC=0, until a slave answers.
- ☑ Own capture/parse + responder (`ec-core --offline/--serve`); ESC register-file engine (broadcast/auto-inc/configured addressing, WKC rules); minimal ESM (AL Control→AL Status). Stages 1–2 + M2a, PRs #3–#8.
- ☑ **MILESTONE 3 ACHIEVED — EL1008 (+EK1100) reaches AND HOLDS OP under TwinCAT.** Held OP ~102 s continuously at a 10 ms cycle (M3 pass bar is 60 s), one rare drop near the end. Path: ESC info block + DL Status port topology (cleared LNK_MIS), SII EEPROM state machine + identity from ESI (cleared VPRS), minimal ESM (AL Control→AL Status), and FMMU logical addressing LRD/LWR (process-data WKC satisfied). PRs #8–#11. The make-or-break risk is retired.
- ◑ Soak hardening (toward AC-02): at 10 ms the slave holds OP ~100–107 s clean, ~1 drop per ~100 s. Drops are **frame-loss driven** (master never reads AL Status Code `0x0134` → not a protocol/DC/state error). Responder runs at ABOVE_NORMAL_PRIORITY_CLASS + THREAD_PRIORITY_HIGHEST (TIME_CRITICAL backfired — starves Npcap delivery; PR #14). Biggest "extra drop" cause found: **other CPU load during the soak** (e.g. concurrent analysis). A perfectly clean multi-hour soak is the kernel-driver escalation (AD-10); the userspace path clears the 60 s M3 bar comfortably.
- ☑ **MILESTONE 4 — input process-data round-trip.** `ec-core` drives the EL1008's 8 DI as a walking bit through the FMMU; TwinCAT's scope shows DI1 pulsing in pattern (software-generated PD → cable → FMMU → TwinCAT image). Responder pinned to a dedicated core (32 logical CPUs on the Server); diagnostics run on other cores without perturbing the sim. 47k+ LRD/run serviced, 0 send failures. PR #16.
- ☑ **TwinCAT rig project version-controlled** at `test/twincat/rig-ek1100-el1008/` (the EK1100+EL1008[+EL2008] XAE `.sln`/`.tsproj`), so the PLC-side config is reproducible and survives unsaved-IDE-session loss (it was rebuilt by hand twice). `.gitignore` drops the regenerated `_Boot/`/`_CompileInfo/`/`.vs/`/`*.bak` artifacts. PRs #19, #20.
- ◑ **MILESTONE 6 (in progress) — chain grows: EL2008 added.** 3-slave chain (EK1100 + EL1008 + EL2008) reaches and **holds OP** on real TwinCAT (0 Lost Frames, 0 Tx/Rx errors), and the **DO output round-trip works (AC-04)** — a channel written on the PLC (EL2008 → Online → Write) appears inside `ec-core` as the matching output byte. Port topology is now position-aware (the chain EK1100→EL1008→EL2008 formed correctly on TwinCAT's scan). PR #21. Still to add for full M6: EL3001 (analog — needs M5 CoE) and EL6224 (IO-Link). **Note M5 (mailbox/CoE) was leap-frogged** — revisit before EL3001.
- ☑ **MILESTONE 5 (mailbox/CoE) — AC-05 ACHIEVED on the real PLC.** The **EL3001** (analog, slave 3) — the first CoE terminal — with a full CoE/SDO engine + per-slave Object Dictionary (PRs #23–#27, #33, #34). On the rig, with the EtherCAT master at **PREOP** (Free Run off), TwinCAT now both **reads and writes** our CoE objects: an SDO **upload** of `0x1000` returns our live OD value `0x00001389` (TwinCAT *accepts* it and advances through the dictionary), and an SDO **download** of `0x8000:11 = 0x04D6` (via the Startup list) updates our OD and is acknowledged. **Two root causes cracked (2026-06-06, see §3):** (1) the reply must **echo the request's mailbox sequence counter** (header byte 5, bits 4..6) — TwinCAT reads SM1 on a fixed cycle and pairs reply-to-request by that counter; an independent counter is re-read forever ("could not be read"). (2) CoE **records need sub-index 0** (the highest sub-index) or every record access aborts `0x06090011`. Added the **SDO-Information service** (CoE 0x08) too. **Two root causes cracked** as above (counter echo, record sub-0).
- ☑ **MILESTONE 5 — EL3001 reaches & HOLDS OP under cyclic TwinCAT (2026-06-06).** With the EL3001's TwinCAT mailbox polling set to **Cyclic**, it reaches OP from a clean start and holds (`WcState=0`, live AI value). The earlier flap was diagnosed (TwinCAT's State-Change mailbox-read path, not jitter) and worked around; see the UPDATE below + §3/§5.6. _Historic detail of the investigation follows:_ Under cyclic TwinCAT (Free Run on) the **EL3001 [previously] flapped OP↔PREOP**. The cyclic mailbox transport is solid now (three wire-trace fixes: **single-buffer deferral** of a request that arrives while a reply is unread; **SM-status read-only** so the master's SM-config re-writes don't clobber our `0x080D` mailbox-full bit; kept the **echo counter**). The remaining failure is **stability, not protocol**: intermittent `state change aborted (SAFEOP→PREOP)` + `Timeout: clear sm pdos (0x1C12)`, with `Device 2: Frame missed 10 times` warnings — telltales stay healthy (`0x1c12` collected, `retry=0`, `uncollected=0`). Leading cause: **responder frame-loss / jitter (Tier-1 userspace-timing limit, §7 risk / AD-10)** — the PreOp→SafeOp mailbox handshake is timing-fragile (a dropped frame trips the timeout) where cyclic process data tolerates drops. Turning off hot-path logging did not fix it. An earlier "SAFEOP/OP SOLVED" note was premature (that OP depended on a manual PREOP browse pre-configuring the PDOs). Full write-up: `docs/coe-safeop-brief.md` §11.9 (transport) + §11.10 (stability). **UPDATE 2026-06-06 — RESOLVED (see §3 + §5.6):** measured `drop=0` over a 64 s flapping run → **jitter is NOT the cause**; tshark of `cleanstart.pcapng` showed the EL3001 stuck at PREOP because TwinCAT writes the `0x1c12:00` PDO-config SDO but **never FPRDs our reply via its State-Change mailbox path** (our LRD correctly reports mailbox-full `0x08`), timing out (`clear sm pdos 0x1C12`) before SAFEOP. **Fix confirmed:** set the EL3001's TwinCAT mailbox polling to **Cyclic** → EL3001 reaches & **HOLDS OP** from a clean start (`WcState=0`, live AI value). M5 done for the EL3001. The drop-in default (State-Change) path is parked pending a real-slave eavesdrop reference. Remaining CoE: segmented transfer (`0x1008` / live browse), EMCY, settings-take-effect (§5.6).
- ☐ Phase-1 station (EK1100 + DI + DO + AI + IO-Link master + temp sensor) reaches OP and holds 1 hour at 10 ms (AC-01, AC-02)
- ☐ Headless harness (pysoem) green in CI

## 3. Key decisions log

| Date | Decision | Rationale | Status |
|---|---|---|---|
| 2026-05-21 | Three living docs: Requirements (md), Architecture (html), Handover (md) | Mobile-friendly authoring, easy to read on PC later | Done |
| 2026-05-21 | Add fourth doc: `PLC_SETUP.md` — step-by-step PLC + simulator wiring | Separate operational instructions from architecture | Done |
| 2026-05-21 | **PLC target: Beckhoff TwinCAT 3** (XAE engineering, XAR runtime) | User's installed environment. All compatibility tuning targets TwinCAT. | Adopted |
| 2026-05-21 | **Day-one IO-Link device: temperature sensor, generic profile** | Simple PD (one signed 16-bit value), useful ISDU set (scale, offset, alarms), easy to reason about. Specific real device deferred. | Adopted |
| 2026-05-21 | **CiA 402 drive in phase 2, not phase 1** | The drive state machine is the most complex single piece. Get coupler + DI + DO + AI + IO-Link working first. | Adopted |
| 2026-05-21 | **Topology grows from one EK1100 branch.** Architecture allows EK1521/EK1122 later without rework. | User goal of growable topology, but not over-engineering for day one. | Adopted |
| 2026-05-21 | **Headless PLC stand-in built on SOEM (`pysoem` for tests, `libsoem` for perf tests).** | SOEM is open source, GPLv3, well-supported, and `pysoem` makes test code trivial. Lets CI run without a TwinCAT machine. | Adopted |
| 2026-05-21 | **Slave-stack licensing:** internal use only — not a commercial product. We use Beckhoff vendor IDs in the EEPROM mirror because we *impersonate* their terminals. No ETG membership or Vendor ID needed because we are not shipping a device. | ETG rules apply to manufacturers shipping EtherCAT products. We are not. | Adopted |
| 2026-05-21 | **No use of GPL slave-stack code in our own slave layer.** SOES (GPLv2) is intended for embedded ESC-attached use anyway; we keep slave-side code on a clean license. SOEM (master, headless harness) being GPL is fine because the harness is a separate tool used only internally. | Keep slave-side code on a clean license boundary. | Adopted |
| 2026-05-21 | **TE1111 explicitly excluded from this project.** That path is being explored elsewhere. This project commits to "build our own software slave stack on a real cable." | User direction. Removes the safety-net option but sharpens the project scope. | Adopted |
| 2026-05-21 | **Simulator host OS: Windows native (with Npcap).** Linux + AF_PACKET is a future variant when a separate Linux box becomes available. WSL2 is not used (cannot reliably bind raw to a host NIC). | Aligned with the dev PC having VS2026 + Claude Code on Windows. | Adopted |
| 2026-05-21 | **Toolchain on the dev PC: VS2026 (MSVC v145, C++20 default), CMake 4.1+ with Ninja, Python 3.12, Git, GitHub CLI, Npcap SDK.** | All available on Windows; CMake+Ninja gives Claude Code a clean command-line build and `compile_commands.json`. | Adopted |
| 2026-05-21 | **Claude Code setup: native Windows installer (no WSL, no Node).** Repo-root `CLAUDE.md` is the single most impactful configuration item; subdirectory `CLAUDE.md` files added lazily as scopes mature. | Native installer is the current recommended path; lazy loading of subdir CLAUDE.md keeps context cost low. | Adopted |
| 2026-05-28 | **Milestone 1 passed; first OP committed to non-DC.** Repo + full toolchain stood up; TwinCAT EK1100+EL1008 rig built on the PLC laptop (Intel I219-LM, RT driver installed). Capture shows TwinCAT looping **BRD of AL Status (`0x0130`)** at ~200/s with WKC=0 — this is the Stage-2 entry point: answer that BRD and increment the WKC (×2, since we emulate both the EK1100 and EL1008 ESCs) to be detected. The EL1008 exposes **no DC operation mode** in TwinCAT, so Milestone 3 will run non-DC, removing the DC risk for first OP. | Real-cable bring-up; trace-driven slave development. | Done |
| 2026-06-03 | **PLC host pivoted off the IT-locked laptop.** The corporate laptop's TwinCAT was forced into *user-mode runtime* by IT-enforced Virtualization-Based Security (Event Viewer: "Realtime ethernet adapter Device 1 is running in the user mode runtime"), which cannot drive EtherCAT — master never left INIT regardless of our (correct) responses. VBS is GPO/MDM-locked (no clean bypass). Replacement host: a self-controlled Windows Server 2019 Datacenter box (Xeon E5-2689, legacy BIOS, VBS already "Not enabled"), now running TwinCAT 3.1 Build 4024.75 in **kernel-mode RT** with an Intel NIC ("Ethernet 2", RT driver bound, realtime-capable). Ordered an Intel I210-T1 as backup NIC; fallback OS path is Win 10/11 Pro on the same box. | Deep-research (cited) confirmed the blocker is VBS, not the OS edition; Server 2019 + VBS-off + supported Intel NIC gives kernel-mode RT. | Done |
| 2026-06-03 | **MILESTONE 3 ACHIEVED: software slave reaches and holds OP under TwinCAT.** After the MAC-preservation breakthrough, four register-content slices took the EK1100+EL1008 to OP: (1) ESC info block + DL Status port topology → cleared LNK_MIS; (2) SII EEPROM read state machine + identity from the ESI (report checksum OK, no CRC) → cleared VPRS; (3) minimal ESM (AL Control→AL Status mirror) → state transitions accepted; (4) FMMU logical addressing (LRD/LWR map process data into the logical image) → cyclic working counter satisfied. At a 10 ms cycle the slaves held OP ~102 s continuously (M3 bar: 60 s); at 4 ms they oscillated (latency-bound). One drop per ~100 s remains (userspace responder jitter). The project's central risk is retired. | Trace-driven; satisfy TwinCAT's init-command sequence. | Done |
| 2026-06-03 | **MAC-preservation was the breakthrough; Milestone 3 viability PROVEN.** Working rig: Server 2019 + kernel-mode RT + dedicated Intel NIC (IP unbound), managed via RDP over the Realtek/LAN, EtherCAT master run in **Config-Mode Free Run**. The unlock: our Stage-2 responder rewrote the reply source MAC to a synthetic slave MAC; the lenient user-mode laptop tolerated it but the **kernel-mode RT master only accepts returned frames carrying its own source MAC** (as a real ring preserves it) — so it silently dropped every reply and polled BRD forever. Fix (PR #8): reply with the master's frame unchanged (source MAC preserved), detect our own echoes by content fingerprint. Instantly the master ran the **full init sequence** (identity 0x0000, DL Control 0x0101, SII 0x0500/0502/0508, SM 0x0800, FMMU 0x0600, DC 0x0910+, AL Control 0x0120). Added a minimal ESM (AL Control→AL Status mirror). Current block: slaves at INIT with `LNK_MIS`/`VPRS`/`INIT_ERR` — register file returns zeros for ESC info block, DL Status port links, and SII identity. **The make-or-break question (can a software slave drive a real kernel-mode TwinCAT master through bring-up?) is answered YES.** | Real-cable, kernel-RT bring-up; trace-driven. | Done |
| 2026-06-03 | **Approach validated against commercial prior art (acontis EC-Simulator); reach OP by satisfying TwinCAT's init-command sequence.** EC-Simulator is a shipping *software-only* slave-side simulator ("no special hardware") that runs a real master (incl. TwinCAT) over a real cable (HiL mode) — proving Milestone 3 is feasible in principle. Key implications now in ARCHITECTURE §2/§4.4/AD-11: (a) HiL-over-real-cable needs **no master-side add-on** (TwinCAT just sees a real network), which is why our approach works without TE1111; (b) the master drives a **fixed init-command sequence** (DL Control 0x0101 → station addr 0x0010 → SII 0x0500 → SM 0x0800 → FMMU 0x0600 → AL Control 0x0120), readable directly from each slave's *Advanced Settings → Init Commands* tab — this IS the Milestone 2–3 build order; (c) acontis hits ≤1 ms via a kernel RT driver, reinforcing that our userspace path caps near Tier 1. We keep hand-rolling (no ET9300 SSC). | User asked to analyze acontis and sharpen architecture. | Done |
| 2026-06-03 | **TwinCAT rig engineering project committed to the repo** at `test/twincat/rig-ek1100-el1008/`. | The EK1100+EL1008 project was lost twice to unsaved IDE sessions — VS keeps a newly-created solution in a *temp* location until "Save new projects when created" is on, so File→Save All persisted nothing findable; and a running runtime (boot config in `C:\TwinCAT\3.1\Boot\`) is decoupled from the editable `.sln`/`.tsproj`, so "the PLC is running" ≠ "the project is saved". Committing the source makes the PLC-side config reproducible; `.gitignore` drops regenerated artifacts. Verified the committed project drives TwinCAT to OP with the live DI round-trip. PRs #19, #20. | Stop losing the rig config; reproducibility. | Done |
| 2026-06-03 | **MILESTONE 6 STARTED: EL2008 digital output added; 3-slave chain holds OP; DO round-trip works (AC-04).** Appended an EL2008 (8 DO, Beckhoff product `0x07D83052`) as slave 2. Made port topology **position-aware** (`apply_topology`) instead of per-profile, so the EL1008 auto-becomes a middle terminal (links downstream port 1) and future terminals drop in without per-profile edits. On TwinCAT's re-scan the chain EK1100→EL1008→EL2008 formed cleanly, all three reached **OP** (0 Lost Frames), and a DO written on the PLC (EL2008 → Online → Write a channel) appeared inside `ec-core` as the matching output byte. The output-FMMU write path already existed (WKC +1 LWR / +2 LRW); added `kEl2008Profile`, `Chain::output_byte()`, and a `--serve` "EL2008 DO=" print. PR #21. **Scan gotcha:** TwinCAT's *device* scan also lists ~18 phantom `SERCONCHIP` (SERCOS-III) adapters — select only the EtherCAT device, else it OOMs (ADS 1802) instantiating bogus masters. EL2008 revision set to a baseline `0x00100000` pending the EL2xxx ESI (TwinCAT accepted it on scan). | Trace-driven; mirror the M4 input round-trip on the output side. | Done |
| 2026-06-03 | **MILESTONE 5 (mailbox/CoE) — engine + write path work; TwinCAT read-back is the open blocker.** Built a CoE/SDO engine (`ecat/coe.{hpp,cpp}`: `ObjectDictionary` + expedited SDO upload/download/abort) wired into the chain's mailbox SyncManager handshake (SM0 mailbox-out / SM1 mailbox-in, SM1-status `0x080D` + AL-Event `0x0220` signalling), with `[coe]`/`[mbx]` serve-time logging (FR-OBS-01). Added the **EL3001** (slave 3, product `0x0BB93052`) with an SII CoE-mailbox declaration (SII words 0x18–0x1C) + an OD (Device Type, identity 0x1018, settings 0x8000, and PDO-config objects 0x1C00/0x1C12/0x1C13/0x1A00). PRs #23–#27. **On the rig:** CoE recognized (CoE-Online tab populates from the ESI), and TwinCAT's PreOp→SafeOp PDO-assign **downloads are serviced** (`download 0x1C12:00` etc., no longer abort). **Blocker (characterized via coe2/coe3/coe4 captures + slave logging):** TwinCAT writes SDO requests to SM0 and ACKs them (WKC=1); our slave processes them and stages the reply at SM1 (sets SM1-full + AL-Event), but TwinCAT **never reads SM1** — no read of `0x1080`/`0x0805`/`0x080D`/`0x0220` in any capture; AL-status reads are only 6 bytes (`0x0130`–`0x0135`). So SDO **uploads** report "Object could not be read" and the EL3001 holds at PREOP (input SM stays disabled because the PDO config can't complete). The read-back gating is **internal to TwinCAT** (not on the wire); slave-side SM1-status + AL-Event signalling did not change it. **Decision pending (user to pick next session):** (a) validate against a SOEM/pysoem master to prove the engine + isolate the TwinCAT handshake (also advances M9); (b) model the full SM mailbox buffer-state machine (low confidence); (c) park CoE, move on. Also unimplemented: SDO Information service (Get OD List), which "Update List" online needs. | Trace-driven; CoE write path proven, read-back is a TwinCAT-internal handshake invisible on the wire. | In progress |
| 2026-06-03 | **M5 CoE read-back — MECHANISM FOUND (refines the row above).** Per `docs/coe-deep-dive-twincat.docx` §3, TwinCAT's efficient mailbox poll **maps a mailbox-full status bit into the process image via an FMMU and watches it with cyclic LRD** (not FPRD) — "one LRD checks many slaves at once." Confirmed on the wire: `coe2.pcapng` frame 5654 — the EL3001's **FMMU 1** maps **physical `0x080D` (SM1 status byte) → logical `0x09000000` (SAFEOP image) / `0x01300000`-ish (PREOP), 1 byte, Read**. So TwinCAT reads our SM1-status byte *through the LRD process image*; bit 3 (mailbox-full) set ⇒ it then FPRDs SM1. **That is the read path we never serviced** — we set `0x080D` bit 3 but the LRD that maps it isn't being answered with the bit (many `coe4` LRDs return WKC 0 = no slave mapped that logical region). **Plan (continue on TwinCAT — mechanism known):** instrument the LRD→`0x080D` FMMU path, re-scan the rig, verify our slave answers that LRD with the mailbox-full bit, fix, then confirm TwinCAT FPRDs SM1 and the SDO completes. pysoem stays a parallel validation; the SDO Information service is still needed for CoE-Online "Update List". Steps in §5.6. | The deep-dive doc + the FMMU descriptor cracked the "invisible gating" mystery. | In progress |
| 2026-06-06 | **CORRECTION — cyclic CoE *transport* solved; stable OP NOT achieved (supersedes the "SAFEOP/OP SOLVED" row below, which was premature).** A clean‑restart soak showed the earlier "OP" was **not reproducible**: it depended on a manual PREOP CoE‑Online browse pre‑completing the PDO config, after which Free‑Run‑on coasted to OP. From a clean INIT→OP the EL3001 **flaps OP↔PREOP** intermittently. What *is* solid: the cyclic mailbox transport, after a 3rd fix — **SM status bytes (`0x0805`/`0x080D`) made read‑only to master writes** (TwinCAT's 8‑byte SM‑config re‑write spans `0x080D` and was clobbering our mailbox‑full bit each cyclic retry → its LRD saw "empty", never FPRDed). With deferral + SM‑status‑RO + echo, the cyclic `0x1c12` reply now collects cleanly (`collected=staged`, `retry=0`, `uncollected=0`). The remaining failure is **stability, not protocol**: `state change aborted (SAFEOP→PREOP)` + `Timeout: clear sm pdos (0x1C12)` + `Frame missed 10 times` warnings → **responder frame‑loss / jitter** (Tier‑1 userspace limit, §7 / AD‑10); the PreOp→SafeOp mailbox handshake is timing‑fragile where process data tolerates drops. Turning off hot‑path logging did not fix it (ruled out as sole cause). Also added: changing AI sine value into the EL3001 SM3 input PDO. Write‑up: `docs/coe-safeop-brief.md` §11.9 (transport) + §11.10 (stability). **Next: responder jitter/frame‑loss work.** | Honest re‑assessment after the clean‑restart soak; protocol works, stability is Tier‑1. | Transport done; stable OP open |
| 2026-06-06 | **SAFEOP/OP SOLVED — EL3001 reaches OP under cyclic TwinCAT (supersedes the "NOT solved" row below).** [SUPERSEDED by the CORRECTION row above — the OP was not reproducible from a clean start.] The missing piece was **single-buffer mailbox backpressure**: TwinCAT's optimized PDO-config path writes the *next* request (`0x1C13`) **before reading the previous reply** (`0x1C12`), and our synchronous responder clobbered the unread reply → the cyclic bring-up looped. **Fix:** when a mailbox-out request arrives while SM1 is still full, **defer** it and service it from SM0 once the master collects the pending reply (one reply at a time, in order). Combined with the AC-05 transport kept intact (**echo** counter + clear on SM1 last byte `0x10FF` + trigger on SM0 content write); the counter experiments (0/incrementing) during the investigation were red herrings that broke the required echo while we chased the clobber. On the rig: EK1100+EL1008+EL2008+**EL3001 all OP**; AC-05 PREOP read/write re-verified (no regression). **Open follow-ups (not bring-up):** EL3001 shows `WcState=1`/`Value=0` — its SM3 **AI input PDO isn't populated yet** (serve a changing value, part of "the rest"); and a **clean-restart soak** (INIT→OP from a fresh ec-core) to confirm reproducibility. Full write-up: `docs/coe-safeop-brief.md` §11.9. | The deferral + kept-echo were the two true root causes; trace-driven. | Done (OP on rig) |
| 2026-06-06 | **SAFEOP cyclic mailbox — deep wire-trace investigation; NOT solved; SM0 hypothesis disproven.** Instrumented every mailbox access (greppable `// TELLTALE` counters + an `[mbx-read]` per-read `ado/len/reaches_last` log), captured the live SAFEOP attempt (dumpcap) and dissected with tshark. Findings (full detail in `docs/coe-safeop-brief.md` §11): (1) TwinCAT uses **last-byte commit/complete** handshakes — writes a request as content `FPWR 0x1000 len16` **+ `FPWR 0x107F len1`** (SM0 last byte), reads a reply as header `FPRD 0x1080 len2` **+ `FPRD 0x10FF len1`** (SM1 last byte) + content `len16`. (2) **Two mailbox paths**: the *simple* full-SM `FPRD len128` (used for the `0x8000` startup SDO + all PREOP/AC-05) **works**; the *optimized* split path (used for `0x1C12` PDO-config) **never completes**. (3) **SM0 is not watched** — `sm0-read=0`, no FPRD of `0x0805`, FMMU maps only `0x080D` → the ranked-#1 SM0-handshake hypothesis is **dead**. (4) The **counter is not the gate**: echo → `0x1C12` reply peek-rejected; constant-0 and slave-incrementing → reply fully read but still not accepted; loops & restarts after `TimeoutMailbox` (2 s). `0x1C13` **never appears on the wire**. Open suspects: the optimized path's acceptance of the `0x1C12` *result*; SM2/SM3 process-data setup failing after `0x1C12`; or a race in our synchronous one-shot reply vs the master's multi-datagram cycle. **Consolidated code to the AC-05-verified transport** (echo counter, clear on SM1 last byte `0x10FF`, trigger on SM0 content write) + dormant SM0/deferral modelling + the new tell-tales. Next: hand `coe-safeop-brief.md` §11 to external brainstorming; **re-verify PREOP CoE (AC-05) on the rig** to confirm no regression. | Trace-driven; converted guesses into wire measurements, eliminated SM0 + counter as the cause. | Investigated, open |
| 2026-06-06 | **M5 CoE — SOLVED on the rig (AC-05 ✅), two root causes.** Driving the bus at **PREOP only (Free Run off)** makes TwinCAT poll the mailbox via simple **fixed-cycle FPRD of SM1 `0x1080`** (it reads `0x1080`/`0x10FF` ~7500×, never the SM status `0x080D`/`0x0805`). In that mode it pairs reply-to-request by the **mailbox sequence counter** (header byte 5 bits 4..6): a 21k-frame capture showed request `0x1000` cnt 2, our *independent* reply cnt 3 → re-read 2895× and rejected ("could not be read"). **Fix 1: echo the request's counter in the reply** (`run_mailbox`, PR #34). Then writes failed because TwinCAT reads a record's **sub-index 0 (count) first** and we returned `0x06090011`; **Fix 2: add sub-index 0 to records `0x6000`/`0x8000`**. Result: upload `0x1000 = 0x1389` accepted + advances; download `0x8000:11 = 0x04D6` (Startup list) accepted. Also added the **SDO-Information service** (CoE 0x08) for online OD browsing. Prior 2026-06-03 LRD-watch analysis applies only to the **Free-Run/cyclic** mode (still unsolved → blocks SAFEOP/OP); the PREOP CoE path is independent and now works. | Trace-driven: the counter mismatch and the missing record sub-0 were both visible on the wire. | Done (AC-05) |
| 2026-06-06 | **Step 3 (stable OP) starts with measurement; responder-health instrumentation landed (PR #44).** Added `net::PcapSource::stats()` (pcap_stats recv/drop/ifdrop) + a `[resp]` health line in `run_serve` (every ~2 s, off the per-frame path): Npcap **drop delta per window**, our **per-frame service latency** (`svc_max`/`slow>1ms`), and rx/serviced/sendfail/echo counts. This tells a *driver* drop (we were too slow → climbing drop, low svc_max) from a *send/hot-path* stall (high svc_max) from *no drop at all* (loss is elsewhere). Pure observability, no stack behaviour change; tagged `// TELLTALE`. **Also triaged an external LLM's follow-up suggestions:** ✅ already done — SM-status preservation, echo counter, hot-path logging off; ❌ **rejected "defer at the commit point (FPWR `0x107F`) instead of the content write"** — we *measured* that on the rig and it regressed (the `0x1C12` reply was left unread); the content-write trigger is AC-05-verified and stays; ✅ **queued the cheap confirmation experiment** — set TwinCAT `TimeoutMailbox`=10000 ms and re-run clean INIT→OP: if flapping drops sharply, the cause is pure jitter. | Measure before optimizing; convert the jitter hypothesis into wire/driver numbers. | Instrumentation done; rig measurement next |
| 2026-06-06 | **Jitter theory DISPROVEN; real blocker found by wire trace — TwinCAT won't collect the EL3001's `0x1c12` reply via its State-Change mailbox path.** PR #44's `[resp]` measurement over a 64 s clean INIT→OP (user-confirmed OP-click + flap) showed **`drop=0`, `slow>1ms=0`, `svc_max`≤750 µs, `sendfail=0`** — zero frame loss, so the Tier-1 jitter/AD-10 hypothesis is dead. tshark of `cleanstart.pcapng` then showed the true cause: the EL3001 **never leaves PREOP** (master only ever writes it AL Control `0x02`/`0x12`, never SAFEOP — the "state change aborted" is TwinCAT's *internal* SM giving up in PreOp→SafeOp prep). Each attempt: master downloads `0x8000:17` (Startup-list SDO) and **reads our reply** via direct synchronous `FPRD 0x1080` ✓; then downloads `0x1c12:00=0` ("clear sm pdos", auto-PDO-config) and **never FPRDs our reply** → SDO timeout → abort → retry ~15 s. Our slave is correct: FMMU1 maps `0x080D`→logical `0x09000000`; after staging the `0x1c12` reply our LRD returns `0x08` (full), WKC 1, ~40k×, clean `00`→`08` edge — **TwinCAT sees full and still won't read.** So the gap is **TwinCAT's State-Change (event-driven) mailbox-read path** (used for auto-PDO-config), not our transport/flags/timing. **Next (clean, no code):** switch EL3001 mailbox polling to **Cyclic** in TwinCAT (Advanced→Mailbox) so it FPRDs on a timer; if it then reaches SAFEOP/OP the diagnosis is confirmed. Then decide whether to also satisfy the State-Change path slave-side (candidate levers: AL-Event `0x0220`/mask `0x0204`). The deferral / SM-status-RO / echo fixes remain correct, just not the last blocker. | Measured (drop=0) + wire-traced (cleanstart.pcapng) — converted the guess into evidence. | Diagnosed; fix experiment pending |
| 2026-06-06 | **EL3001 reaches & HOLDS OP under cyclic TwinCAT — M5 done for the EL3001 (mailbox polling = Cyclic).** Confirming the wire diagnosis: setting the EL3001's TwinCAT mailbox polling to **Cyclic** (Advanced→Mailbox, ~10 ms, not State Change) made TwinCAT FPRD the mailbox on a timer, collect the `0x1c12` reply, finish PreOp→SafeOp, and reach **OP from a clean `--serve` start**. Soak-confirmed: holds OP, `WcState=0`, live sine **AI value updates** (`EL3001 AI = … (served)`), `[resp]` `drop=0`. Reproducible (unlike the browse-dependent "premature OP"). The Cyclic setting must be committed in the rig project (`test/twincat/...`). **(B) drop-in default (State-Change) path = PARKED:** pure capture analysis is exhausted (no working reference; TwinCAT never reads AL-Event `0x0220`/`0x0204`; we present FMMU `0x080D`→`0x09000000`=`0x08` full correctly yet TwinCAT won't FPRD — trigger is TwinCAT-internal). Cracking it needs a **real Beckhoff terminal eavesdrop** (user expects HW in a few weeks) to diff a real ESC's `0x080D` presentation vs ours. Cyclic polling is the supported path meanwhile. **CoE remaining ("the rest"):** (1) segmented SDO transfer (>4-byte objects: `0x1008`, full CoE-Online browse) — the big one; (2) EMCY; (3) make `0x8000` settings take effect; details in §5.6. | Cyclic-polling experiment confirmed the wire diagnosis; banked the working path, parked the drop-in version for a real-slave reference. | EL3001 OP done; CoE polish + B parked |
| 2026-06-07 | **MILESTONE 7 STARTED — EL6224 IO-Link master added (phase 7.1, code).** Added the EL6224 4-channel IO-Link master as slave 4 (5th ESC), derived from the Beckhoff EL6xxx ESI now in `configs/esi/` (PR #48 code, #49 ESI). Identity: product **0x18503052**, revision 0x00100000, device type 0x184C1389, name "EL6224". Its mailbox-in SM is at **0x1100** (vs EL3001's 0x1080) — no handler change needed (the mailbox handler reads live SM addresses). Default no-device process image is **input-only**: TxPDO **0x1A04 "DeviceState" = 4 bytes**, one USINT per port (**0xF100:01..04** port status), SM2 outputs empty. `populate_el6224_od()` mirrors the ESI's default PDO/OD; `kEmulatedSlaveCount` 4→5. ctest 5/5. **Like the EL3001 it's a CoE device → set its TwinCAT mailbox polling to Cyclic to reach OP.** **Pending:** rig scan (remove Device 2 → re-Scan → set EL3001+EL6224 mailbox polling = Cyclic → Save All + commit rig project → OP); then 7.2 (model a port-1 temp sensor → live PD), 7.3 (ISDU over CoE), per §5.6 plan. Possible trace-driven tweak if TwinCAT's PDO-assign differs from the ESI default (esp. the mandatory SM2 RxPDOs). | EL3001 pattern + ESI-derived OD; the engine is device-agnostic so this was a profile + OD addition. | 7.1 code done; rig OP pending |
| 2026-06-07 | **M7 phase 7.1 BLOCKED — EL6224 needs AoE + master-configurable PDOs (7.1 was under-scoped).** The rig scan + TwinCAT error log (`el6224_bringup.pcapng`, in dev-box home) show two new requirements the simple terminals don't have: (1) **`Timeout: 'AoE Init Cmd (download NetId)'`** — the EL6224 IO-Link master speaks **AoE (ADS-over-EtherCAT)**, a mailbox protocol we don't implement; TwinCAT downloads an ADS NetId to the master during init, we never answer → no SAFEOP. (2) **`Timeout: 'download pdo 0x1A00 entries'`** — the EL6224's PDOs are **master-configurable** (TwinCAT writes the `0x1A00` mapping), unlike the EL3001's fixed PDO; our OD modeled `0x1A04` fixed. **The EL3001 also regressed to PREOP — its code is byte-unchanged, so this is COLLATERAL:** the EL6224's failed init flaps and floods the bus (`Device 2: Frame missed 10 times`), dragging the EL3001's Cyclic mailbox reads past their timeout (`clear sm pdos 0x1C12/0x1C13`). **Plan:** (a) **park the EL6224** to restore the EL3001 OP — set `kEmulatedSlaveCount` back to 4 (the EL6224 profile/OD/test stay, just not instantiated) **and** revert the rig TwinCAT project to its 4-box config; (b) implement **AoE**: declare AoE in the SII (`0x1C |= 0x0001`), handle the AoE mailbox (type 1) — respond to the NetId-download init cmd with an ADS success ack (this is also the channel IO-Link ISDU rides on, 7.3); (c) add the **configurable `0x1A00` PDO** (accept TwinCAT's mapping writes); then re-enable (count→5) and re-scan. Honest note: the "EL3001 pattern" was insufficient — the IO-Link master is the hardest phase-1 device by design (AoE + IO-Link semantics). | Error-log/trace driven; calibration miss in 7.1 planning (AoE not anticipated). | 7.1 blocked; AoE is the real work |
| 2026-06-07 | **EL6224 final blocker NARROWED — two-CoE-slave mailbox-collection bug (not missing PDO entries).** AoE works (verified) and the EL6224's PDO-config *clears* are serviced without abort (PR #54/#55). With `--log` during an active OP click, the failure is precise: TwinCAT clears the EL3001's `0x1c12:00`/`0x1c13:00` (replies staged + collected at mbx-in `0x1080`) and the EL6224's `0x1a00:00` (reply staged at mbx-in `0x1100`), but then LRD-polls mailbox-status `0x08` and reads the EL3001's `0x1080`, **never the EL6224's `0x1100`** — so the EL6224's clear reply is never collected → `download pdo 0x1A00 entries` times out → PREOP. The EL6224 collected its AoE reply at `0x1100` fine earlier; it only starves once **both** CoE mailboxes are full simultaneously → strong hypothesis: both slaves' mailbox-full bit (`0x080D`) is mapped to the **same logical LRD byte**, so the master can't disambiguate. **Next:** capture `el6224_pdo.pcapng` during an OP push, decode the MBoxState FMMU writes (FPWR `0x0610`) for the EL3001 (0x03ec) and EL6224 (0x03ed) and compare `Log Start`; if equal → the collision is confirmed; fix = each CoE slave's mailbox-full independently visible. | `--log` + active OP click disambiguated candidate (a) vs (b); it's (b). | Narrowed; capture-to-fix pending |
| _TBD_ | Topology config format: YAML vs JSON vs TOML | YAML proposed; confirm before writing third device model | Open |
| _TBD_ | IPC mechanism between C++ slave core and L4 device-model service | shared memory + gRPC proposed; validate under load | Open |
| _TBD_ | L4 language: C# or Python (or both, per device class) | Pick one to start; phase-2 may add the other | Open |
| _TBD_ | First cycle tier we commit to: 10 ms only, or 10 ms + 4 ms? | Tier 1 must work; Tier 2 is the ambition for v1 | Open |

Append rows here when a non-trivial choice is made. Don't delete — supersede with a new dated row referring back.

## 4. What's next (prioritized)

> Single path now. The plan is a tight ladder of progressively harder milestones, each one proving a specific capability before moving on.

### Milestone 0 — Repo skeleton & dev environment

> Step-by-step instructions: see `GETTING_STARTED.md`. Summary:
- Install Claude Code, Git, GitHub CLI, Python 3.12, Npcap (+ SDK) on the dev PC. (Visual Studio 2026 with the C++ workload is already installed.)
- Create the GitHub repo, clone locally, drop the five docs into `docs/`, commit the CLAUDE.md and `.gitignore`, push.
- Create the directory skeleton (`sim/`, `configs/`, `test/`, `tools/`).
- Confirm the TwinCAT laptop has a supported Intel NIC. (See `PLC_SETUP.md` §2.)
- Cable between the two PCs.

### Milestone 1 — Wire-level smoke test

- Simulator: bind the NIC raw, log incoming frames, do not respond yet.
- PLC: TwinCAT project with one EK1100 + one EL1008. Activate config; the master will start broadcasting "scan" frames.
- Pass condition: simulator captures the expected EtherCAT broadcast frames. We see what TwinCAT sends.
- Outcome: validates the raw-socket setup and gives us a Wireshark-able trace of what we must respond to.

### Milestone 2 — Single slave reaches Init

- Implement enough of the ESC emulation that the master can read the simulator's "EEPROM" via the SII state machine.
- Slave appears in TwinCAT's Online tab as a recognized EL1008.
- Pass condition: TwinCAT identifies the slave by vendor ID + product code; state = Init.

### Milestone 3 — Single slave reaches OP

- Implement ESM transitions: Init → PreOp → SafeOp → Op. Implement SyncManagers and FMMUs for the PD areas.
- Pass condition: one EL1008-imitating slave is in OP under TwinCAT, holds for 60 seconds at 10 ms cycle, zero Lost Frames.
- This is the **make-or-break milestone** — if we can hold OP on a software slave, the rest is implementation work. If not, we need to revisit the approach.

### Milestone 4 — Process data round-trip

- Toggle a DI bit in the simulator → PLC sees it.
- PLC writes a DO bit → simulator sees it.
- Pass condition: bidirectional PD round-trip working at 10 ms cycle.

### Milestone 5 — Mailbox + CoE ☑ (AC-05 achieved 2026-06-06)

- ☑ Mailbox SyncManagers + CoE protocol (SDO read/write) — `sim/ec-core/src/ecat/coe.cpp`, mailbox handshake in `chain.cpp`.
- ☑ Per-device Object Dictionary (EL3001) — `populate_el3001_od()`.
- ☑ Pass condition met: on the rig (master at PREOP), TwinCAT **reads** `0x1000`→`0x1389` (accepted) and **writes** `0x8000:11`→`0x04D6` (OD updated). Keys: echo the request mailbox counter; records carry sub-index 0; SDO-Information service added.
- ☐ Follow-ups (polish, not blocking AC-05): device-name string `0x1008` (segmented transfer) + fuller OD so the CoE-Online **list browse** populates live instead of falling back to offline.

### Milestone 6 — Chain of slaves

- Add EL2008, EL3001, then EL6224 to the simulator's modeled chain. Topology scan must find them in correct order with correct port wiring.
- Pass condition: full phase-1 chain (minus the IO-Link device) in OP.

### Milestone 7 — IO-Link temperature sensor

- Model the EL6224's IO-Link master logic. Add the generic temperature sensor on port 1.
- Pass condition: PLC reads sensor PD; PLC writes an ISDU parameter and reads it back.

### Milestone 8 — Phase-1 acceptance

- Run the full AC-01 to AC-08 checklist from REQUIREMENTS §9.1.
- Pass condition: all green; 1-hour soak test at 10 ms cycle clean.

### Milestone 9 — Headless harness + CI

- pysoem-based driver runs the same exchanges as the TwinCAT laptop, on a Linux box without TwinCAT.
- Pass condition: CI green on every commit.

### 4.1 Track in parallel (not blocking)

- **L4 device-model layer prototypes.** Write the DI terminal model in C# and the temp sensor in Python; compare and pick.
- **Observability.** Live PD view (CLI is fine), structured logs, basic metrics.
- **YAML topology schema.** Freeze before milestone 6.

### 4.2 Cycle-tier exercise

After milestone 8 is green at 10 ms, push the PLC cycle to 4 ms (Tier 2) and then to 1 ms (Tier 3). Document where things break and decide whether to optimize or accept the limit.

### 4.3 Risk-driven escalations

- If milestone 3 fails — TwinCAT will not stably accept frames from our software slave — capture detailed Wireshark traces and the TwinCAT error register contents, and bring a clear question back. This is the moment when the project's viability is decided.

### 4.4 Repo skeleton (target)

```
/sim
  /ec-core              # C++ slave stack: L1 wire, L2 ESM/SM/FMMU, L3 mailbox/CoE
  /ec-core/eeprom-mirror # Per-device EEPROM (SII) contents matching real ESI
  /devices              # L4 device models (C# and/or Python)
  /control-plane        # L5/L6: scripting, YAML loader, web UI
/configs
  /schema               # JSON Schema for the YAML topology
  /stations             # Concrete station YAMLs
  /esi                  # Beckhoff ESI XML references
  /iodd                 # IODD XML references
/test
  /pysoem               # Headless harness
  /unit                 # Unit tests for device models and CoE OD
  /twincat              # TwinCAT XAE solutions used for acceptance
/scripts                # Runtime scripting hooks
/docs                   # The four living documents
/tools                  # Helpers: ESI parser, Wireshark dissector helpers, etc.
```

## 5. How to resume work on the PC (CURRENT — M0–M4 + EL2008 done; M5/AC-05 CoE done)

> State as of 2026-06-06 (end of session): our C++ software slave presents a
> **4-slave chain** (EK1100 + EL1008 + EL2008 + **EL3001**). The first three
> reach/hold **OP** with both process-data round-trips working (EL1008 DI / M4,
> EL2008 DO / M6-AC-04). **M5/AC-05 is achieved:** with the bus at **PREOP
> (Free Run off)**, TwinCAT both **reads** (`0x1000`→`0x1389`) and **writes**
> (`0x8000:11`→`0x04D6`) our CoE objects — see §2 and §3 (2026-06-06 row). The
> fixes: echo the request's mailbox counter, give records a sub-index 0, add the
> SDO-Information service, clear mailbox-full on the SM last-byte read, and
> **tell-tale instrumentation** (grep `TELLTALE`; removable; the
> `reply-uncollected` canary flags exactly the SAFEOP symptom). PRs #32–#38.
> All code on `main`; working tree clean.
>
> **AGREED NEXT PLAN (2026-06-06):** do "the rest" (segmented SDO transfer for the
> device-name string → full live CoE-Online browse; Emergency/EMCY; a changing
> sensor value + settings that take effect; the PLC dummy-controller program over
> SDO), then take one more **SAFEOP swing** (model the full mailbox SM state
> machine — esp. the **SM0 mailbox-out handshake**, the leading hypothesis).
>
> **SAFEOP is BLOCKED and characterized.** In cyclic mode (Free Run on) TwinCAT
> maps SM1-status (`0x080D`→logical `0x09000000`) and LRD-watches it; it reads
> "mailbox full" but **does not FPRD the SM1 buffer** to collect the `0x1C12`
> reply, so PREOP→SAFEOP times out. In the simple PREOP phase it FPRDs and
> collects fine. **Full write-up for analysis: `docs/coe-safeop-brief.md`**
> (self-contained briefing — also handed to an external LLM for brainstorming).
> See §3 (2026-06-03 + 2026-06-06 rows) and §5.6.
>
> Captures `coe2/coe3/coe4.pcapng` + `*_capture.pcapng` from the investigations
> are on the dev box (gitignored). **For the SAFEOP path, resume at §5.6 +
> `docs/coe-safeop-brief.md`.**

### 5.1 The working rig (two machines + a cable)
- **Sim PC** (this dev machine, VS2026 + Claude Code): runs `ec-core`. EtherCAT
  NIC = Intel **I225-V "Ethernet"**, libpcap handle
  `\Device\NPF_{DC25F4C3-68B2-480B-A3D5-156CBEDF29FC}` (re-derive any time with
  `ec-core --list`). It has a link-local IP; leaving IP bound is fine.
- **PLC = Windows Server 2019 box** (Xeon E5-2689, 32 logical CPUs, legacy BIOS,
  VBS already off). TwinCAT 3.1 **Build 4024.75**, **kernel-mode RT**. EtherCAT
  master bound to its Intel **"Ethernet 2"** NIC (MAC `00:1b:21:f3:cb:72`),
  **IPv4/IPv6 unbound** on that NIC. **Manage the Server via RDP to its Realtek
  LAN IP `192.168.178.105`** — NOT over the EtherCAT cable (RDP and EtherCAT
  cannot share a NIC).
- **Cable**: direct, Sim-PC "Ethernet" ↔ Server "Ethernet 2". No switch.
- **TwinCAT rig project** lives in the repo at
  `test/twincat/rig-ek1100-el1008/rig-ek1100-el1008.sln` (cloned on the Server,
  which also has Git). Open it in XAE; after any change do **both** File→Save All
  (persists the source here) **and** Activate (writes the runtime). To pick up a
  new terminal: remove `Device 2 (EtherCAT)`, re-Scan, select **only** the
  EtherCAT device (not the phantom `SERCONCHIP` SERCOS adapters), accept the
  boxes, Save All + commit.
- The corporate laptop is abandoned (IT-locked VBS forced user-mode runtime,
  which can't drive EtherCAT — see §3, 2026-06-03).

### 5.2 Build & test (Sim PC, x64 VS 2026 environment)
The plain build commands are in CLAUDE.md. The exact invocation that works from
a non-VS shell (initialises x64 MSVC first; the `vswhere not recognized` line it
prints is harmless):
```
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" && cd /d C:\dev\EthercatSim && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON && cmake --build build && ctest --test-dir build --output-on-failure'
```
If a rebuild fails with "file in use", kill a stale responder first:
`Get-Process ec-core | Stop-Process -Force`.

### 5.3 Run the slave + bring it to OP
1. Sim PC: `./build/sim/ec-core/ec-core.exe --serve "\Device\NPF_{DC25F4C3-68B2-480B-A3D5-156CBEDF29FC}"`
   (runs until Ctrl-C; optional trailing seconds arg. No admin needed. Prints
   "responder pinned to core 31".)
2. Server/TwinCAT: EtherCAT master must be **transmitting** — use **Config Mode
   → Activate Configuration → then the master's Online tab → "Activate Free
   Run? Yes"**, with **Freerun Cycle = 10 ms** (Adapter tab). "System in Run
   mode" alone does NOT transmit. Click **Op** in the master Online tab to drive
   the bring-up.
3. Expect: slaves climb INIT→PREOP→SAFEOP→**OP** and hold ~100 s (rare drops =
   userspace jitter, see §7). EL1008 DI walks (Channel 1→8, one per second) —
   watch Term 2 inputs / the DI1 scope in TwinCAT.

### 5.4 Capture & diagnose (the trace-driven loop)
- Record: `"C:\Program Files\Wireshark\dumpcap.exe" -i <handle> -f "ether proto 0x88a4" -w cap.pcapng -a duration:N`
- Dissect: `"C:\Program Files\Wireshark\tshark.exe" -r cap.pcapng -Y "ecat ..." ...`
  Our reply frames have source MAC `02:ec:ca:00:00:01`; filter `eth.src != 02:ec:ca:00:00:01` for master-only.
- **Run heavy analysis (tshark) pinned to cores OTHER than the top** (e.g.
  `start /affinity 0x0F /b ...`) — `ec-core` pins itself to the top core, so this
  keeps diagnostics from causing OP drops. A *third* concurrent dumpcap does not
  capture our TX frames (Npcap artifact) — trust `ec-core`'s own "responded"
  count, not the third capture.

### 5.5 Code map (where the slave logic lives)
- `sim/ec-core/src/ecat/frame.hpp` — non-allocating EtherCAT frame/datagram parser.
- `sim/ec-core/src/ecat/chain.{hpp,cpp}` — the heart: per-ESC 64 KB register file,
  addressing (broadcast/auto-inc/configured/**logical-FMMU**), WKC rules, ESC info
  block + DL Status topology, **SII EEPROM** read state machine + identity,
  minimal **ESM** (AL Control→AL Status), `set_process_input` (DI), `output_byte`
  (observe DO), and the **CoE mailbox handshake** (`run_mailbox`: SM0-in → OD →
  SM1-out + SM1-status `0x080D` + AL-Event `0x0220`). Phase-1 profiles hard-coded
  (`kEk1100/kEl1008/kEl2008/kEl3001Profile`, **4 slaves**); `apply_topology` makes
  ports position-aware; `populate_el3001_od` builds the EL3001 OD.
- `sim/ec-core/src/ecat/coe.{hpp,cpp}` — CoE/SDO engine: `ObjectDictionary` +
  `process_mailbox` (expedited SDO upload/download/abort, ETG.1000.5/.6) +
  `set_logging`/`logging_enabled`. **No SDO Information service yet.**
- `sim/ec-core/src/ecat/station.cpp` — slave MAC constant + helpers.
- `sim/ec-core/src/net/pcap_source.cpp` — Npcap RAII (immediate mode), send, and
  `stats()` (pcap_stats recv/drop/ifdrop → `CaptureCounters`) for responder-health.
- `sim/ec-core/src/net/priority.cpp` — ABOVE_NORMAL priority + top-core affinity.
- `sim/ec-core/src/main.cpp` — `--list/--offline/--iface/--serve` CLI; serve loop
  (echo dedup, source-MAC preserved, walking-DI driver, EL2008 "DO=" print,
  `coe::set_logging(true)` → `[coe]`/`[mbx]` mailbox logging).
- `test/unit/test_*.cpp` — frame parser, station MAC, chain engine (EL2008/EL3001
  identity, position-aware DL Status, LWR output, **CoE mailbox round-trip**),
  and `test_coe` (the SDO engine).

### 5.6 RESUME HERE — **EL3001 reaches & HOLDS OP** (set TwinCAT mailbox polling = Cyclic); M5 done for EL3001

**✅ RESOLVED (2026-06-06, soak-confirmed).** With the EL3001's TwinCAT mailbox polling
set to **Cyclic** (EL3001 box → EtherCAT → Advanced Settings → Mailbox → Cyclic, ~10 ms,
*not* State Change), the EL3001 reaches **OP from a clean `--serve` start and HOLDS**:
`WcState=0`, the live sine **AI value updates** in TwinCAT (console prints
`EL3001 AI = … (served)`), and `[resp]` stays `drop=0`. This is reproducible (unlike the
earlier browse-dependent "premature OP"). **The Cyclic setting lives in the rig project —
make sure it's committed in `test/twincat/...`.** Diagnosis was exactly right (below).

**(B) "make it work with TwinCAT's *default* State-Change polling" — PARKED, needs a
real-slave reference.** Pure capture analysis is exhausted: no capture shows TwinCAT's
State-Change reader ever firing (nothing working to diff); TwinCAT never accesses the
AL-Event regs (`0x0220`/`0x0204`); and we already present the documented signal correctly
(FMMU `0x080D`→`0x09000000` LRD returns `0x08` full, clean edge) yet TwinCAT won't FPRD.
The trigger is TwinCAT-internal / off-wire. **Plan:** when a **physical Beckhoff CoE
terminal** is available (user expects one in a few weeks), **eavesdrop a real
PreOp→SafeOp** with State-Change polling and diff what the real ESC presents at `0x080D`
vs us — that's the one missing reference. Until then, Cyclic polling is the supported path.

**Why it was failing (wire-verified, kept for the record):** AC-05 solid; under cyclic
TwinCAT the EL3001 stayed at PREOP because:

1. **Frame loss / Tier-1 jitter is NOT the cause.** PR #44's `[resp]` line over a 64 s
   clean INIT→OP (user confirmed OP was being clicked + it flapped): **`drop=0`,
   `slow>1ms=0`, `svc_max`≈100–750 µs, `sendfail=0`** the whole time. The userspace
   responder keeps up perfectly. **Do not chase jitter / AD-10.**
2. **The real failure, from `cleanstart.pcapng` (tshark):** the EL3001's PreOp→SafeOp
   transition dies in the mailbox PDO-config, *before* the master ever commands SAFEOP
   on the wire (master only ever writes EL3001 AL Control `0x02`/`0x12` = PREOP /
   PREOP+ErrAck; AL Status always `0x02`). Per attempt (~every 15 s):
   - download `0x8000:17` (Startup-list SDO) → master **reads our reply** via a direct
     synchronous `FPRD 0x1080` ✓
   - download `0x1c12:00=0` ("clear sm pdos", auto-PDO-config) → master **never FPRDs
     our reply** → SDO times out → `clear sm pdos (0x1C12)` → abort → retry.

1. **Frame loss / Tier-1 jitter is NOT the cause.** PR #44's `[resp]` line over a 64 s
   clean INIT→OP (user confirmed OP was being clicked + it flapped): **`drop=0`,
   `slow>1ms=0`, `svc_max`≈100–750 µs, `sendfail=0`** the whole time. The userspace
   responder keeps up perfectly. **Do not chase jitter / AD-10.**
2. **The real failure, from `cleanstart.pcapng` (tshark):** the EL3001's PreOp→SafeOp
   transition dies in the mailbox PDO-config, *before* the master ever commands SAFEOP
   on the wire (master only ever writes EL3001 AL Control `0x02`/`0x12` = PREOP /
   PREOP+ErrAck; AL Status always `0x02`). Per attempt (~every 15 s):
   - download `0x8000:17` (Startup-list SDO) → master **reads our reply** via a direct
     synchronous `FPRD 0x1080` ✓
   - download `0x1c12:00=0` ("clear sm pdos", auto-PDO-config) → master **never FPRDs
     our reply** → SDO times out → `clear sm pdos (0x1C12)` → abort → retry.

**Our slave is behaving correctly** — this is the key, non-obvious part. EL3001 FMMU1
(`0x0610`) maps phys `0x080D` (SM1 status) → logical `0x09000000`. After we stage the
`0x1c12` reply we set mailbox-full; our LRD of `0x09000000` returns **`0x08` (full),
WKC 1, ~40k times**, with a clean `00`→`08` edge. **TwinCAT sees "full" and still
won't FPRD.** So the gap is in **TwinCAT's State-Change (event-driven) mailbox-read
path**, which it uses for auto-PDO-config — not in our transport, flags, or timing.
(The `0x8000` SDO works precisely because it's read synchronously, not via that path.)

The 2026-06-06 transport fixes (deferral, SM-status read-only, echo counter) are all
still correct and necessary — they're just not the last blocker. Full evidence in this
section; older jitter framing in `coe-safeop-brief.md` §11.10 is now superseded.

**CoE — what's done vs what remains (engine = `coe.{hpp,cpp}`, per-device OD =
`populate_*_od()` in `chain.cpp`):**

Done: expedited SDO upload/download (≤4-byte values), SDO abort, the SDO-Information
service *as long as the reply fits one 128-byte mailbox* (object list / VAR/ARRAY object
desc / entry desc), the full PreOp→SafeOp→OP PDO-config, and a live input PDO. Remaining:

1. **Segmented (normal) SDO transfer — the big one.** `coe::Entry` only holds a
   `uint32` (≤4 bytes); strings/arrays/long descriptions need a multi-frame transfer and
   a byte-buffer entry type. Unlocks: device-name `0x1008`, full **CoE-Online "Update
   List"** browse (today it truncates at 128 B — see `coe.cpp` "would overflow the
   mailbox"), and any >4-byte parameter. Transport already does multi-frame reads, so
   this is CoE-layer protocol work, well-scoped.
2. **EMCY (Emergency) messages** — async slave→master error notify (e.g. AI over/under-
   range). Small: one mailbox message type.
3. **Make `0x8000` settings take effect** — we store the downloaded value in the OD but
   don't apply it (e.g. user scale/offset doesn't change the AI). Device-model (L4) wiring.
4. **(Optional)** Complete-Access SDO; richer abort coverage; a PLC dummy-controller
   program exercising it.

**After CoE "the rest":** EL6224 (IO-Link, M7), then AC-01..08 soak (M8). **Adding more
CoE devices is cheap** — one `kXxxProfile`, one `populate_xxx_od()`, an SII from the ESI,
a topology row; the engine is device-agnostic and serves whatever the OD contains. The
only costly OD entries are >4-byte ones (need item 1, segmented transfer) and
device-specific dynamic behavior (item 3, or e.g. a CiA-402 drive's state machine).

**Captures on disk (gitignored, dev box):** `cleanstart.pcapng` = the clean failing
flap (primary evidence); `live.pcapng` = EL3001 reached ~OP (premature, pre-configured);
`safeop2_capture`/`reject_capture`/`coe2..4` = earlier SAFEOP/SDO probes.

<details><summary>Superseded jitter "next steps" (kept for history)</summary>

The `[resp]` reading guide (driver-drop vs hot-path-stall) and the `TimeoutMailbox`=
10000 ms confirmation experiment are now moot — `drop=0` already ruled out frame loss.
Original plan was: measure drops; reduce per-frame latency / jitter via async logging,
core pinning/isolation, TX double-buffering; then escalate to a kernel capture/inject
path (AD-10). All shelved: there is nothing to optimize (zero drops measured).

</details>

**Tools in place (greppable `// TELLTALE`; removable per `telltale.hpp`):**
- Tell-tales: `sm0-read`, `mbx-buf-read`, `deferred`, `uncollected`, `retry`, etc.
- **`[resp]` responder-health line (PR #44)** — every ~2 s in `--serve`: `npcap
  recv/drop(+delta)/ifdrop`, `svc_max`/`slow>1ms` per window, rx/serviced/sendfail/echo.
  Backed by `net::PcapSource::stats()` (pcap_stats). Always prints (off the per-frame
  path); this is the step-1 measurement. Reading guide in "Next steps" §1 above.
- `[mbx-read]` per-read diagnostic (`ado/len/reaches_last/hdr_seq`) and `[coe]`/`[mbx]`
  logs — gated by `coe::set_logging(true)` (off by default; flip to diagnose).
- Capture: `dumpcap -i 7 -f "ether proto 0x88a4" -a duration:90 -w live.pcapng`
  (NIC `\Device\NPF_{DC25F4C3-...}` = dumpcap iface 7); dissect with tshark. Kept this
  session: `live.pcapng`, `cleanstart.pcapng`, `DevEthercatSimsafeop.pcapng`.

Parallel/optional: **pysoem/SOEM** harness to validate the engine headlessly (M9).

## 6. Environment notes

- Documents are intentionally self-contained:
  - `REQUIREMENTS.md`, `HANDOVER.md`, `PLC_SETUP.md` — plain Markdown.
  - `ARCHITECTURE.html` — single HTML file, inline SVG, no external assets.
- Authored on mobile, intended for PC use.

## 7. Risks & watch-items

- **Software slave on a generic NIC may not keep up with TwinCAT's cycle.** A real EtherCAT Slave Controller (ESC chip) processes frames in nanoseconds while they pass through. We are doing it in microseconds-to-milliseconds. Milestone 3 is where we find out whether 10 ms cycles are achievable. If not, we will likely need to accept slower cycles (e.g. 50 ms) as our supported floor, or rethink the approach entirely. **Prior-art datapoint:** the commercial acontis EC-Simulator reaches ≤1 ms cycles, but does so with a *kernel real-time Ethernet driver*. Our userspace Npcap path measured 1–14 ms jitter on detection replies (immediate mode helped — see decisions log 2026-06-03); this confirms Tier 1 (10 ms) is the realistic bar and a kernel-level capture/inject driver is the escalation if Tier 2/3 are needed.
- **Distributed Clocks.** Real ESCs latch a 64-bit DC value into a register as frames pass through. We can't do this nondeterministically in software. We will respond to DC datagrams with synthesized values that pass plausibility checks — enough for TwinCAT to enter Op. If TwinCAT enforces stricter DC behavior than expected, this is a second high-risk area.
- **PLC NIC compatibility.** TwinCAT real-time only works on supported Intel NIC chipsets. Confirm the PLC laptop has one before milestone 1.
- **TwinCAT vendor-quirk discovery.** We will inevitably hit mailbox or CoE edge cases TwinCAT expects in a particular way. Plan for an iterative debug loop with Wireshark.
- **Configuration drift.** With each device class having its own object dictionary + IODD + ESI, the config surface gets large fast. Decide the YAML schema before the third device model.

## 8. Working agreements (proposed)

- Edit all four docs in place. Bump the version line at the top when something material changes.
- Open questions go in `REQUIREMENTS.md` §8. Resolutions move to the Decisions log here in §3.
- Every new device class gets a row in `REQUIREMENTS.md` §4 _before_ code is written for it.
- Diagrams in `ARCHITECTURE.html` are authoritative; if code diverges, update the diagram in the same commit.
- `PLC_SETUP.md` stays operational and current — if a screen changed in TwinCAT, the doc gets a screenshot or a sentence saying so.

## 9. Contact / context

_(Add names, calendars, repo URL, PLC test rig location, etc. once known.)_

---

_End of handover._
