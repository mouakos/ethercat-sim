# CLAUDE.md — ethercat-sim

## Project in one paragraph
Software EtherCAT slave-stack simulator that pretends to be a full station of Beckhoff
terminals (EK1100, EL1008, EL2008, EL3001, EL6224, an IO-Link temperature sensor) so a
real Beckhoff TwinCAT 3 PLC can talk to it over a real Ethernet cable. The slave stack
is our own code; we are explicitly NOT using TE1111. See `docs/REQUIREMENTS.md`,
`docs/ARCHITECTURE.html`, `docs/HANDOVER.md`, `docs/PLC_SETUP.md`, and the four-doc set
they form.

## Read these first, in order
1. `docs/HANDOVER.md` — current state, decisions, what's next. Always start here.
2. `docs/REQUIREMENTS.md` — what we're building and the acceptance criteria.
3. `docs/ARCHITECTURE.html` — the layer model and where each piece of code lives.
4. `docs/PLC_SETUP.md` — operational steps and the milestone checklist.

When the docs and the code disagree, the docs are the source of truth for intent;
update the code or the doc, then call it out in HANDOVER.md §3 (decisions log).

## Stack
- **C++20** for the slave core (L1/L2/L3). MSVC v145 (VS 2026), `/std:c++20 /W4 /permissive-`.
- **C# (.NET 8)** or **Python 3.12** for the device-model layer (L4). Not decided yet — see HANDOVER.md §3, "L4 language" row.
- **Python 3.12** for the control plane (L5/L6) and tests.
- **CMake 4.1+** as the build system. We do not check in `.vcxproj` files; they're generated.
- **Ninja** as the CMake generator.
- **Npcap SDK** for raw NIC access on Windows.

## Build commands
From the repo root, in a Developer PowerShell for VS 2026:
```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```
Release builds use `-DCMAKE_BUILD_TYPE=Release`. Always keep `CMAKE_EXPORT_COMPILE_COMMANDS=ON`
so editor tooling and code intelligence work.

## Python commands
```
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
pytest test/pysoem -v
```

## Code style
- C++: modern C++20. Prefer `std::span`, `std::string_view`, `std::expected` (or `tl::expected`)
  over raw pointers and out-params. RAII everywhere. No `new`/`delete` in application code.
  `noexcept` on move ops. `[[nodiscard]]` on getters and factories.
- C#: nullable reference types on; `var` only when the right-hand side makes the type obvious;
  records for DTOs; async/await for any I/O.
- Python: type-annotated (`mypy --strict` clean), formatted with `ruff format`, linted with `ruff`.
- No allocations on the EtherCAT hot path (L2 datagram dispatch). Preallocate at startup;
  use object pools where lifecycle requires it.

## Naming
- C++: `snake_case` for files and functions, `PascalCase` for types, `kSomething` for constants,
  `m_member` for non-public data members.
- C#: standard .NET conventions (PascalCase types/methods, camelCase locals).
- Python: PEP 8.

## Repo etiquette
- Branches: `main` is protected and only updated via PRs. Feature branches are
  `feat/<short-name>`, fixes are `fix/<short-name>`, docs-only changes are `docs/<short-name>`.
- Commits: imperative mood ("add SII emulator", not "added SII emulator"). Keep one logical
  change per commit. Reference issues with `#N`.
- PRs: include a short rationale and a checklist of what was tested.

## Project-specific conventions
- Every new device class gets a row in `docs/REQUIREMENTS.md` §4 *before* code is written for it.
- Every non-trivial decision gets a dated row in `docs/HANDOVER.md` §3.
- Every device's EEPROM (SII) emulation is derived from the Beckhoff ESI XML for that part —
  drop the ESI in `configs/esi/`, reference it by filename in the topology YAML, and the
  EEPROM generator produces the binary blob at build time.
- Topology config lives in `configs/stations/*.yaml` and is validated against
  `configs/schema/station.schema.json` (CI fails on schema errors).

## Things to never do without checking with the human
- Push to `main` directly.
- Make commits that touch more than one of: docs, C++ slave core, device models, tests.
  (Each PR stays focused.)
- Disable a failing test rather than fix it.
- Add a dependency without a one-line justification in the PR description.
- Modify `docs/REQUIREMENTS.md` §9 (acceptance criteria) without an explicit ask.

## Where things live
- `sim/ec-core/` — C++ slave stack (L1/L2/L3). Owns the NIC.
- `sim/devices/` — L4 device models, one subfolder per device class.
- `sim/control-plane/` — Python scripts, YAML loader, observability.
- `configs/` — schemas, station YAMLs, ESI references, IODD references.
- `test/pysoem/` — headless harness (pytest + pysoem).
- `test/unit/` — unit tests for device models and CoE OD.
- `test/twincat/` — TwinCAT XAE solutions used in the real-PLC rig.
- `tools/` — small helpers (ESI parser, EEPROM blob generator, Wireshark helpers).
- `docs/` — the four living documents.

## How to ask me (the human) for clarification
Prefer to make a concrete proposal and ask "should I do X, or Y?" over open-ended questions.
If a milestone in HANDOVER.md §4 is blocked on a decision, surface that explicitly.

## Useful one-liners
- Capture EtherCAT frames during a session:
  `"C:\Program Files\Wireshark\dumpcap.exe" -i <iface> -f "ether proto 0x88a4" -w session.pcapng`
- Show real-time–capable NICs in TwinCAT: menu `TwinCAT → Show Real Time Ethernet Compatible Devices…`
- List NICs visible to Npcap: `getmac /fo csv /v` (Windows), or use `tcpdump -D` after installing it via Npcap.
