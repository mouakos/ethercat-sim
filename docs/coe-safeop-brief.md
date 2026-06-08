# CoE SAFEOP mailbox problem — briefing for external analysis

> **STATUS (2026‑06‑06): CoE protocol works; OP reachable but NOT yet stable.**
> The CoE mailbox transport is essentially solved (see §11): PRE‑OP SDO read/write
> is solid (AC‑05), the cyclic `0x1c12` reply now collects cleanly, and the EL3001
> *can* reach **OP** and serve a live analog value. **But under cyclic operation it
> flaps OP↔PRE‑OP intermittently**, with TwinCAT `Frame missed` warnings and
> `clear sm pdos (0x1C12)` timeouts. The leading explanation is **responder
> frame‑loss / jitter — the known Tier‑1 userspace‑timing limit — not a CoE gap**
> (the PRE‑OP path is timing‑slack and works; the cyclic PreOp→SafeOp mailbox
> handshake is timing‑fragile and one dropped frame aborts it). An earlier
> "SOLVED → OP" claim here was **premature**: that OP depended on a manual PRE‑OP
> CoE‑Online browse pre‑completing the PDO config; a clean INIT→OP is unreliable.
> See §11.9 (transport fixes) and §11.10 (the stability finding). The open work is
> **stability (responder jitter), not protocol.**
>
> **Purpose of this document.** We were stuck on one specific problem in a
> software EtherCAT slave simulator: getting a CoE analog-input terminal past
> the PRE‑OP → SAFE‑OP state transition with a real Beckhoff TwinCAT 3 master.
> This file is self‑contained so an LLM with general EtherCAT/CoE knowledge but
> **no access to our codebase or prior conversation** can brainstorm attacks.
> If you know EtherCAT mailbox internals or how TwinCAT's master collects
> mailbox replies in cyclic operation, jump to **§6 (the problem)** and
> **§9 (the precise question)**.

---

## 1. The system

We are building a **software EtherCAT slave stack** in C++20 that impersonates a
station of Beckhoff terminals so that a **real, unmodified Beckhoff TwinCAT 3
PLC** (the EtherCAT *master*) talks to it over a real Ethernet cable as if it
were physical hardware. We own a NIC via Npcap, receive the master's EtherCAT
frames, mutate them in place (addressing, working counters, register/SM/FMMU
reads & writes, mailbox), and send them back. We are **not** using Beckhoff's
TE1111 — the slave stack is entirely our own code.

The emulated chain is: **EK1100** (bus coupler) + **EL1008** (8× digital in) +
**EL2008** (8× digital out) + **EL3001** (1× analog in, **the CoE device**).

**Current status:**
- EK1100 + EL1008 + EL2008 reach and hold **OP**; digital I/O round-trips work.
- EL3001 reaches **PRE‑OP**. Acyclic **CoE SDO read *and* write work at PRE‑OP**
  (see §5). The EL3001 **cannot get past PRE‑OP → SAFE‑OP** (see §6). That last
  gap is the whole subject of this document.

---

## 2. The goal with CoE

CoE (CANopen over EtherCAT) is the **acyclic mailbox parameter channel**. The
EL3001 is our first terminal with a mailbox. Two things must work:

1. **SDO read/write** of object-dictionary entries (identity, settings, the
   analog value) — **DONE** (our acceptance criterion "AC‑05").
2. **PDO configuration over SDO during PRE‑OP → SAFE‑OP**, so the terminal goes
   operational and its **analog value flows as cyclic process data** the PLC
   reads every cycle — **BLOCKED**. This is the goal we need help with.

---

## 3. EtherCAT / CoE background (skip if you know it)

**SyncManagers (SMs)** are the slave's communication buffers. For a mailbox
slave:
- **SM0** = mailbox **out** (master → slave): the master writes SDO requests here.
- **SM1** = mailbox **in** (slave → master): the slave writes SDO replies here.
- **SM2 / SM3** = cyclic process data (outputs / inputs), set up at SAFE‑OP.

Each SM has config registers and a **status byte**. The status byte's
**bit 3 = "mailbox full"** (1 = a message has been written to the buffer and is
waiting to be read; cleared when the reader finishes reading the buffer).

**Mailbox protocol:** the master writes a request into SM0, the slave writes a
reply into SM1. Because slaves cannot initiate traffic, the **master must poll**
SM1 to collect replies. Each mailbox message starts with a 6-byte header
(length, address, channel/priority, and a **type nibble + 3-bit sequence
counter**), then the CoE payload (a 2-byte CoE header whose *service* field says
SDO-request/SDO-response/SDO-information/etc., then the SDO itself).

**ESM (state machine):** INIT → PRE‑OP → SAFE‑OP → OP. The mailbox/CoE becomes
available at **PRE‑OP**. The **PDO assignment/mapping** objects (0x1C12 RxPDO
assign, 0x1C13 TxPDO assign, 0x1A00 TxPDO mapping, 0x1C00 SM types) are written
by the master **via SDO downloads during PRE‑OP → SAFE‑OP** (the standard
reconfig sequence: set assignment count to 0, set mapping count to 0, write
mapping entries, restore counts).

**Two ways a master collects mailbox replies** (both observed from TwinCAT):
- **(a) Simple / fixed-cycle FPRD:** the master directly FPRD-reads the SM1
  buffer (and/or polls the SM1 status with FPRD), on a cycle.
- **(b) "Efficient" FMMU + LRD watch:** the master maps the SM1 **status byte**
  into its cyclic process image via an **FMMU**, and watches the mailbox-full
  bit with a single cyclic **LRD** ("one LRD checks many slaves at once"); when
  it sees full, it issues a targeted **FPRD** of that slave's SM1 buffer.

**Relevant addresses in our setup** (master-programmed, observed on the wire):
- SM0 (mailbox-out) buffer: physical **0x1000**, length 0x80 (128 B).
- SM1 (mailbox-in) buffer: physical **0x1080**, length 0x80 (128 B), 1-buffer
  (mailbox) mode, ECAT-read, enabled.
- SM1 **status byte**: physical **0x080D** (bit 3 = mailbox full).
- The master maps **0x080D → logical 0x09000000** (1 byte, read) via an FMMU and
  LRD-reads it cyclically. (Confirmed from the FMMU descriptor on the wire.)
- AL Control 0x0120, AL Status 0x0130, AL Status Code 0x0134, AL Event Request
  0x0220 (we set bit 9 = SyncManager-1 event when a reply is posted).
- EtherCAT commands seen: FPRD (4) configured-addr read, FPWR (5) configured-addr
  write, LRD (10) logical read, LWR (11), BRD (7) broadcast read.
- EL3001 configured station address: **0x03EC (1004)**.

---

## 4. How our slave models the mailbox (what we do and don't)

- We receive each EtherCAT frame, walk its datagrams, and apply reads/writes to
  a per-slave 64 KB register image, incrementing the working counter for each
  serviced datagram.
- **Mailbox handling is synchronous:** when the master's FPWR writes a request
  into SM0 (0x1000), we **immediately** parse the CoE/SDO, build the reply, write
  it into the SM1 buffer (0x1080), **set the SM1 status full bit (0x080D bit 3)**
  and the AL-Event SM1 bit (0x0220), all within processing that one frame.
- We **echo the request's 3-bit mailbox sequence counter** into the reply header
  (this was essential — see §5).
- The mailbox-full bit is **cleared** when the master reads the SM1 buffer
  (specifically, when a read covers the SM's last byte 0x10FF — the ESC
  "buffer read complete" event).
- **We do NOT model the SM0 (mailbox-out) status/handshake at all.** On a real
  ESC, writing a request to SM0 sets SM0's "full" bit; the slave's PDI reading
  SM0 clears it (so the master can see "the slave consumed my request"). We never
  set or clear SM0's status — we just read the request bytes synchronously.
- We do **not** implement segmented/normal SDO transfer (only expedited, ≤4-byte
  values) and only a minimal SDO-Information service.

---

## 5. What works: SDO read/write at PRE‑OP (for contrast)

With the master held at **PRE‑OP** (TwinCAT "Free Run" off), TwinCAT polls the
mailbox via the **simple fixed-cycle FPRD** of SM1 (0x1080) and matches a reply
to the request it sent **by the mailbox sequence counter**. Two fixes made this
work; both are relevant clues:

1. **The reply must echo the request's mailbox counter.** We first used an
   independent incrementing counter; TwinCAT then re-read the same reply
   thousands of times and never accepted it ("Object 0x…. could not be read").
   Once the reply carries the **same** 3-bit counter as the request, TwinCAT
   pairs reply↔request and accepts it. (Confirmed on a 21k-frame capture:
   request 0x1000 counter 2, our independent reply counter 3 → rejected,
   re-read 2895×; with the echo, accepted and it advanced.)
2. **CoE records need sub-index 0** (the "highest sub-index" count). The master
   reads sub-0 first to learn an object's shape; without it every record access
   aborts with 0x06090011.

Result at PRE‑OP: SDO **upload** of 0x1000 returns our live value, accepted;
SDO **download** of 0x8000:11 updates our object dictionary, accepted. So the
**mailbox transport itself is correct** in the simple-poll mode.

---

## 6. The problem: PRE‑OP → SAFE‑OP times out (cyclic mailbox mode)

When TwinCAT drives the bus toward OP (Free Run on → cyclic operation), the
EL3001 tries PRE‑OP → SAFE‑OP. TwinCAT begins the PDO reconfig with an SDO
**download of 0x1C12:00 = 0** ("clear RxPDO assign"). **It times out**, the
state change is aborted, and the whole bring-up loops and eventually gives up.

**The precise, repeatable observation** (from wire captures + counters built
into our slave):

- **In the INIT→PRE‑OP phase** (before cyclic operation), TwinCAT reads our SM1
  buffer with an **FPRD of 0x1080 (length 128)** and **collects** our replies.
  E.g., a Startup-list SDO write (`download 0x8000:11`) is reliably collected.
  **So FPRD-based collection works.**

- **In the PRE‑OP→SAFE‑OP phase** (cyclic; the FMMU mapping 0x080D→0x09000000 is
  now set up), the sequence is:
  1. TwinCAT FPWR-writes the `0x1C12:00 = 0` request into SM0 (0x1000). Our slave
     services it (working counter increments), stages the SDO-download reply into
     SM1 (0x1080), and sets the SM1 full bit (0x080D bit 3 = 0x08) + the AL-event.
  2. TwinCAT's **cyclic LRD reads the mapped status (logical 0x09000000 = our
     0x080D) and returns 0x08** — i.e. **TwinCAT *sees* "mailbox full".**
  3. **TwinCAT then does NOT issue an FPRD of the SM1 buffer (0x1080)** to collect
     the reply. The reply is never read; the download times out.
  - Net effect: the `0x1C12` reply is **staged but never collected**. The
    bring-up restarts (INIT→PRE‑OP startup SDO collected again, then 0x1C12 fails
    again) and after ~13 attempts TwinCAT gives up.

- **Quantified by our slave's counters over one bring-up:** ~28 mailbox replies
  *staged*, only ~13 *collected*; the ~15 uncollected are all the cyclic-phase
  `0x1C12` (and a couple of `0x1C13`) replies. The collected ones are the
  INIT→PRE‑OP `0x8000:11` startup writes.

- **Intermittency:** in one capture, TwinCAT *did* FPRD some buffers and even
  advanced from `0x1C12` to `0x1C13` once or twice — so it is not a flat
  "never reads," it is "reads in the simple phase, almost never reads in the
  cyclic/LRD-watch phase," and is flaky when it does.

**In one sentence:** *In cyclic (Free Run) operation TwinCAT maps and watches our
SM1-status byte, reads it as "mailbox full," but does not then FPRD the SM1
buffer to collect the reply — whereas in the simple PRE‑OP phase it FPRDs and
collects fine.*

---

## 7. What we have already tried (none fixed the cyclic path)

1. **Echo the request mailbox counter** — fixed the PRE‑OP simple-poll case
   (§5); no effect on the cyclic case.
2. **Set the SM1 full bit atomically with staging the reply** (vs. raising it a
   few frames later). We tried a deferred full bit (to mimic processing latency);
   it made the simple-poll case worse and didn't help the cyclic case. Atomic is
   what we kept.
3. **Set the AL Event Request SM1 bit (0x0220, bit 9)** alongside the full bit.
   No effect; TwinCAT in cyclic mode reads the FMMU-mapped 0x080D, not 0x0220
   (we never see it FPRD 0x0220).
4. **Clear the full bit on the master's read of the SM's last byte (0x10FF)**
   rather than the first — correct ESC "buffer-read-complete" semantics. No
   change to the cyclic behavior.
5. **Add sub-index 0 to records** (§5) — needed for writes generally; no effect
   on the cyclic collection.

We have NOT yet: modeled the SM0 (mailbox-out) status handshake; modeled the
mailbox SyncManager as a proper buffered state machine; checked exhaustively
what working counter TwinCAT expects on the mapped-status LRD; or checked whether
TwinCAT expects the SM1 *buffer* (not just its status) to also be FMMU-mapped.

---

## 8. Hypotheses (ranked)

1. **SM0 (mailbox-out) handshake (leading).** On a real ESC, the master can see
   SM0 go *full* (it wrote a request) then *empty* (the slave's PDI consumed it).
   TwinCAT's *cyclic* mailbox task may gate "now read SM1" on first observing
   "SM0 consumed." We never set/clear SM0's status, so TwinCAT may be waiting for
   a "request consumed" transition that never happens — and therefore never
   schedules the SM1 FPRD. (Why would the simple PRE‑OP phase work then? Possibly
   the simple poll doesn't gate on SM0, but the cyclic mailbox state machine
   does.)
2. **Working counter / WcState on the mapped-status LRD.** If the cyclic LRD that
   reads the mapped 0x080D doesn't get the working counter TwinCAT expects, it
   may flag the read invalid and never act on the "full" bit. (TwinCAT shows
   WcState=1 for the EL3001, though that may just be "no valid inputs yet" at
   PRE‑OP.)
3. **The buffer must also be mapped.** Maybe in cyclic mode TwinCAT expects the
   SM1 *buffer* mapped into the process image (read via LRD), not collected via a
   separate FPRD — and only the *status* is mapped in our case.
4. **Timing/pipeline race** between our synchronous staging and TwinCAT's cyclic
   frame pipeline / mailbox task scheduling.
5. **A SyncManager config/flag** (e.g., the SM1 PDI-controlled/ECAT-controlled
   bits, IRQ enables, or the SM "operation mode") that we report incorrectly,
   causing TwinCAT's cyclic mailbox state machine to not engage.

---

## 9. The precise question for brainstorming

> In TwinCAT 3 (the EtherCAT master), during the PRE‑OP → SAFE‑OP transition with
> the bus in cyclic operation, the master maps a slave's **SM1 (mailbox-in)
> status byte into its process image via an FMMU and reads it with a cyclic LRD**.
> Our software slave correctly sets the **mailbox-full bit (SM status bit 3)**
> when it stages an SDO reply, and TwinCAT's LRD **does read that bit as set**.
> But TwinCAT then **does not issue the FPRD of the SM1 buffer** to collect the
> reply, so the PDO-config SDO (0x1C12) times out and SAFE‑OP fails. In the
> simple INIT→PRE‑OP phase (no cyclic LRD-watch yet) TwinCAT FPRDs the buffer and
> collects replies fine.
>
> **What does a real Beckhoff ESC do — in its SyncManager / mailbox state, its
> register values, or the handshake it presents — that makes TwinCAT's *cyclic*
> mailbox task actually issue the SM1 buffer read after seeing "full"? What are
> we most likely failing to model (SM0 mailbox-out handshake? SM status/activate
> bits? working counter on the mapped-status LRD? a required AL-event or
> interrupt? the buffer also being mapped)? And what's the cheapest experiment to
> disambiguate?**

---

## 10. Useful facts for whoever debugs this

- We can capture all EtherCAT traffic (dumpcap, filter `ether proto 0x88a4`) and
  dissect with tshark; the EtherCAT/`ecat_mailbox` dissectors decode SMs, FMMUs,
  CoE, and the mailbox counter. The master's MAC and our slave's frames are
  distinguishable.
- Our slave has lightweight counters ("tell-tales") for: mailbox requests,
  replies staged, replies collected, **reply-uncollected** (we staged a reply
  while the previous full bit was still set — the canary that flags exactly this
  failure), counter-retry, SDO aborts, etc.
- The reconfig sequence TwinCAT uses (per Beckhoff docs): clear assignment count
  → clear mapping count → write mapping entries → restore mapping count → restore
  assignment count, done during PRE‑OP → SAFE‑OP. It dies on the **first** of
  these SDOs (`0x1C12:00 = 0`).
- "Free Run off" (PRE‑OP) → simple FPRD poll → CoE works. "Free Run on" (cyclic)
  → FMMU+LRD watch → SAFE‑OP mailbox stalls. That on/off switch is the cleanest
  way to reproduce both modes.

---

## 11. UPDATE (2026‑06‑06): wire‑trace findings — most of §6–§8 is now superseded

We instrumented every mailbox access (greppable `// TELLTALE` counters + an
`[mbx-read]` per‑read diagnostic logging `ado/len/reaches_last`), captured the live
SAFE‑OP attempt with dumpcap, and dissected it with tshark. This changes the
picture a lot. **The earlier "TwinCAT sees full but never FPRDs the buffer" was
wrong, and the SM0‑handshake hypothesis (§8, ranked #1) is disproven.**

### 11.1 TwinCAT's actual mailbox protocol on the wire
Decoded from the capture (configured‑address FPWR/FPRD to the EL3001):

- **Write a request** = two datagrams: `FPWR 0x1000 len 16` (the content) **then
  `FPWR 0x107F len 1`** — a one‑byte write to the **SM0 *last* byte** (0x1000+0x80‑1).
  That last‑byte write is the ESC "mailbox written/commit" event.
- **Read a reply** = three reads: `FPRD 0x1080 len 2` (the length header), **`FPRD
  0x10FF len 1`** (the **SM1 *last* byte** = the ESC "mailbox read complete" event,
  which clears the full bit), and `FPRD 0x1080 len 16` (the content).
- So the SM's **last byte is the commit/complete handshake on both sides.** Counts
  in one 90 s capture: `0x1080 len2 ×630`, `0x10ff len1 ×628`, `0x1080 len16 ×628`,
  plus `0x1080 len128 ×4` (the simple full‑SM read, see below).

### 11.2 There are two distinct mailbox paths, and they behave differently
- **Simple/full path** (used for the *startup‑list* SDO `0x8000:11`, and all of
  PRE‑OP/AC‑05): one `FPRD 0x1080 len 128` that spans the whole SM (so it reaches
  0x10FF and clears). **This path works** — TwinCAT accepts the reply and advances.
- **Optimized/split path** (used for the PRE‑OP→SAFE‑OP PDO‑config SDOs, starting
  with `0x1C12:00 = 0`): the 3‑read sequence above. **This path never completes the
  bring‑up.** TwinCAT reads our `0x1C12` reply but never proceeds to `0x1C13`.

### 11.3 SM0 is NOT watched (kills the §8 #1 hypothesis)
- Tell‑tale `sm0-read` stayed **0** for the entire run; the capture shows **no FPRD
  of 0x0805** and the slave's FMMUs map only **0x080D** (SM1 status) → logical, not
  0x0805. TwinCAT does not gate anything on the SM0 status byte. Modelling the SM0
  full/empty handshake (we did, both instant and as evidence) changed nothing.

### 11.4 The counter is not the gate either (three schemes tried)
Reply mailbox sequence counter (header byte 5, bits 4‑6), against `0x1C12` whose
*request* counter is 1 (the 2nd request after the cnt‑0 `0x8000`):
- **Echo the request counter** (reply cnt=1): TwinCAT **peek‑rejects** — reads only
  the 2‑byte length header, never the rest, retries forever. (This is the AC‑05
  PRE‑OP behaviour and it's what we shipped/kept, because PRE‑OP requests are cnt 0.)
- **Constant 0** (reply cnt=0): TwinCAT **reads the whole reply** (header + 0x10FF +
  content) but still does **not** accept it / advance — retries `0x1C12`, restarts.
- **Slave's own incrementing counter** (1,2,3…, ETG.1000.4‑correct): same as
  constant‑0 — reply fully read, not accepted, loops.
- **Write trigger:** triggering `run_mailbox` on the `0x107F` commit (symmetric with
  the read‑side 0x10FF clear) was tried; it *regressed* — the optimized path then
  left the `0x1C12` reply unread and SAFE‑OP timed out faster. Triggering on the
  content write (0x1000) gets the reply read. Both still loop.

### 11.5 The sharp, still‑open question
TwinCAT **accepts** the `0x8000:11` SDO (simple full‑SM path) and advances to
`0x1C12`, then for `0x1C12:00 = 0` (optimized split path) it **reads our valid
download response but never advances** — `0x1C13` *never appears on the wire in 90 s*
— it retries `0x1C12` (~16 ms apart) and after ~2 s (`TimeoutMailbox`) restarts the
whole transition. The reply is a textbook expedited download response (`scs=3`,
`0x60`, index/sub echoed, abort‑free). Tell‑tales while stuck: `uncollected=0`,
`abort=0`, `collected` tracks `staged`. The behaviour is **intermittent** (sometimes
the `0x1C12` reply is staged and not even read before the restart), which smells like
a **timing/ordering** mismatch between our synchronous reply and TwinCAT's optimized
state machine — or a **layer above the SDO transport** (the SM2/SM3 process‑data /
FMMU activation, or the AL‑Control SAFE‑OP write) failing *after* `0x1C12` and forcing
the full restart. New leading suspects, in order: (a) something about how the
optimized path expects the `0x1C12` *result*/timing specifically; (b) the SM3 input
PDO / process‑data SM isn't presented the way SAFE‑OP needs (we accept AL‑Control
optimistically but may not satisfy the actual SM/PDO checks); (c) a genuine
race in our one‑shot synchronous mailbox vs. the master's multi‑datagram cycle.

### 11.6 Config TwinCAT uses for the EL3001 (from its ESI/TwinCAT project)
`StateMBoxPolling=true`, `MboxDataLinkLayer=true`, `TimeoutMailbox=2000`.
SM0=mbx‑out `0x1000` len `0x80` ctrl `0x26`; SM1=mbx‑in `0x1080` len `0x80` ctrl
`0x22`; SM2=PD‑out `0x1100` **len 0** (input‑only); SM3=PD‑in `0x1180` len `4`
ctrl `0x20`. FMMU0 → phys `0x1180` (SM3 input PD) read; FMMU1 → phys `0x080D`
(SM1 status) read. RxPDO assign `0x1C12` is cleared then left empty; TxPDO assign
`0x1C13` → `0x1A00` (`0x6000:01` status 16‑bit + `0x6000:11` value INT16).

### 11.8 KEY: the full PDO config completes at PRE‑OP (Free Run off) — SAFE‑OP is cyclic‑timing‑specific
Re‑verifying AC‑05 with the consolidated code (echo counter + last‑byte clear)
revealed that at **PRE‑OP (Free Run off)** TwinCAT runs the **entire** reconfig
sequence and **every SDO is collected and accepted**:
`0x1C12:00=0` → `0x1C13:00=0` → `0x1C13:01=0x1A00` (TxPDO mapping) → `0x1C13:00=1`,
then startup `0x8000:11` and `0x1000` uploads — **including via the optimized split
read** (`FPRD len2` + `FPRD len16`) for `0x1C12`/`0x1C13:00`. (`0x1008` device‑name
upload aborts `0x06020000` — expected, unsupported.) This is the sequence that, in
**cyclic mode (Free Run on)**, dies at `0x1C12` and never emits `0x1C13`.

Implications: **the OD/PDO content is correct** (the master accepts the whole
config), and **the optimized split mailbox path itself works** when timing is
relaxed. So the SAFE‑OP failure is **not** transport/content — it is specific to the
**cyclic (RT) timing** of Free‑Run‑on: our synchronous one‑shot reply, or the
last‑byte commit/complete sequencing, interacts badly with the master's RT cycle (or
a layer above the SDO fails only under the cyclic bring‑up). This is now the sharpest
framing of the open question: *what is different about the cyclic mailbox poll's
timing/sequencing that makes the same SDOs that succeed at PRE‑OP fail at the
PRE‑OP→SAFE‑OP transition?*

### 11.9 Transport fixes (2026‑06‑06): the cyclic mailbox path now works
Three fixes, all wire‑trace‑driven, got the cyclic `0x1c12` mailbox SDO to be
delivered, read, and collected cleanly (telltales `collected=staged`, `retry=0`,
`uncollected=0`), on top of the AC‑05 transport (echo counter + clear on SM1 last
byte `0x10FF` + trigger on the SM0 content write):
1. **Single‑buffer backpressure ("deferral").** TwinCAT's optimized path writes the
   *next* PDO‑config request (e.g. `0x1C13`) **before reading the previous reply**
   (`0x1C12`). Our synchronous model staged the new reply over the unread one → the
   master never got `0x1C12`'s reply. **Fix:** if a mailbox‑out request arrives while
   SM1 is still full, **defer** it and run it from SM0 once the master collects the
   pending reply (one reply at a time, in order).
2. **SM status read‑only.** The SM status bytes (`0x0805`/`0x080D`) are ESC‑managed
   and read‑only to the master, but a master SM‑config write is 8 bytes and *spans*
   `0x080D`. In cyclic bring‑up TwinCAT re‑writes the SM config repeatedly, so our
   blind copy was **clobbering the mailbox‑full bit** right after we staged a reply →
   its LRD saw "empty", never FPRDed. **Fix:** preserve the SM status bytes across
   master writes. This is what made the cyclic `0x1c12` reply collect reliably.
3. **Keep the echo counter.** The master pairs reply→request by the **echoed** mailbox
   counter (same as PRE‑OP/AC‑05). The constant‑0 / slave‑incrementing experiments
   were red herrings that broke the required echo while we chased the clobber.

**This does NOT by itself give stable OP — see §11.10.** Earlier this section
claimed "SOLVED → OP"; that was premature (that OP run had a manual PRE‑OP browse
pre‑completing the PDO config first; a clean INIT→OP is unreliable). What is solid:
the CoE *transport* and *content* (the master accepts the whole config when timing
allows; OP is reachable).

For reference, the two root causes the deferral addresses, both required:
1. **Clobber.** TwinCAT's optimized path writes the *next* PDO‑config request (e.g.
   `0x1C13`) **before reading the previous reply** (`0x1C12`). Our synchronous model
   staged the new reply over the unread one → the master never got `0x1C12`'s reply,
   looped. **Fix:** if a mailbox‑out request arrives while SM1 is still full, **defer
   it**; run it from SM0 only once the master collects the pending reply (so replies
   are delivered one at a time, in order). One `deferred` event is enough per cycle.
2. **Counter pairing.** The master pairs reply→request by the **echoed** mailbox
   counter (same as PRE‑OP/AC‑05). The SafeOp experiments that replaced echo with
   constant‑0 or a slave‑incrementing counter were **red herrings** — they broke the
   required echo while we were actually chasing the clobber, which masked that the
   deferral alone (with echo kept) was the fix.

So `§11.4`'s "counter is not the gate" was half‑right (echo is needed, but echo alone
isn't sufficient — the clobber also had to be fixed). SM0 handshake (`§11.3`) remains
irrelevant (kept as harmless modelling).

### 11.10 STABILITY is the real remaining issue (2026‑06‑06): intermittent OP, Tier‑1 jitter
With the §11.9 transport fixes the EL3001 **does reach OP and serve a live AI value**
— but **not reliably**. Across clean INIT→OP attempts it **flaps OP↔PRE‑OP**: some
runs reach OP then drop; others never leave PRE‑OP. Symptoms each time:
`Term 7 (EL3001): state change aborted (SAFEOP→PREOP)` + `Timeout: clear sm pdos
(0x1C12)`, and TwinCAT `Device 2 (EtherCAT): Frame missed 10 times` warnings.

Crucially, when it fails the mailbox telltales are *healthy* (`0x1c12` collected,
`retry=0`, `uncollected=0`) — so it is **not** a CoE protocol/transport fault. The
signature (intermittent; PRE‑OP‑slack works / cyclic‑tight fails; frame‑missed
warnings) points to **responder frame loss / scheduling jitter** — our userspace
Npcap responder dropping the odd frame. Cyclic process data tolerates a dropped
frame (re‑sent next cycle); the PreOp→SafeOp **mailbox handshake does not** — a lost
round‑trip trips the `clear sm pdos` timeout and aborts. This is the known **Tier‑1
userspace‑timing limitation** (HANDOVER §7 risk; AD‑10 kernel‑driver escalation),
now the gating factor for *stable* CoE OP. Turning off the verbose `[coe]`/`[mbx]`
logging (a per‑event blocking `fprintf` on the responder thread) did **not** fix it,
so logging stalls are at most a contributor, not the cause.

**Open question for stability work / brainstorm:** how to make the userspace
responder lose ~zero frames during the (bursty, timing‑tight) cyclic mailbox PS
transition — measure where drops occur (Npcap delivery vs our processing vs send),
reduce per‑frame latency, async/ring‑buffer the logging, pin/isolate the core
harder, or escalate to a kernel‑level capture/inject path (AD‑10). Also: does
TwinCAT's `clear sm pdos` timeout have a configurable budget we could relax for
testing to confirm the timing diagnosis?

### 11.7 Current code state
AC‑05 transport (echo counter, clear on SM1 last byte `0x10FF`, trigger on the SM0
content write) + the **single‑buffer deferral** + the **SM‑status read‑only** guard
+ a (harmless, unused) SM0 model — together these make the cyclic mailbox transport
solid. Hot‑path CoE logging is **off by default** now (it stalls the responder; flip
`set_logging(true)` to diagnose). A changing **AI sine value** is driven into the
EL3001's SM3 input PDO (shows once OP holds). Instrumentation: `sm0-read`,
`mbx-buf-read`, `deferred` evidence counters + the `[mbx-read]` range log (greppable
`// TELLTALE`, removable per `telltale.hpp`). All unit tests pass; AC‑05 PRE‑OP
read/write re‑verified on the rig. **OP is reachable but not yet stable (§11.10).**
