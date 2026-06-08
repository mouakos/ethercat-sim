# Getting Started — Claude Code on this project

**Document status:** Draft v0.1 — read once, set up once, refer back rarely
**Last updated:** 2026-05-21
**Audience:** the human (you) setting up the development environment, and Claude Code (the agent) once it is running.

> This doc is a one-time setup checklist plus a short "how we work" guide. It assumes a fresh Windows PC with **Visual Studio 2026** already installed (with the C++ workload). Everything else — Claude Code, Git, Npcap, Python, the repo skeleton, the CLAUDE.md — is set up here.

---

## 1. What you need before starting

| Item | Why | Status |
|---|---|---|
| Visual Studio 2026 with **Desktop development with C++** workload | The MSVC compiler (cl.exe), CMake 4.1.1, Ninja, MSBuild — Claude Code drives these from the command line | ✅ you have this |
| Claude Pro or Max subscription, **or** an Anthropic API key | Claude Code authenticates against one of these | _check_ |
| A GitHub account | Version control + collaboration | ✅ you have this |
| Windows 10/11 64-bit | Required for VS2026 and the native Claude Code installer | assumed |
| Admin rights on the PC | Needed for Npcap install and (later) running the slave-stack binary | assumed |
| 10–20 GB of free disk for the repo, build outputs, and Wireshark captures | Build outputs add up | assumed |

Not needed:
- ❌ WSL2 — for the EtherCAT slave stack we need raw access to a physical NIC, and **WSL2 cannot reliably bind `AF_PACKET` sockets to host NICs**. We run the slave stack natively on Windows using Npcap. WSL2 stays available as a future option if we ever want to run the headless pysoem harness on Linux.
- ❌ Node.js — the **native Claude Code installer for Windows** does not need Node.

---

## 2. Install Claude Code on Windows (native, no WSL)

Anthropic's native installer is the recommended path on Windows. It does not require Node.js and works in regular PowerShell.

1. Open **PowerShell** (not "as Administrator" — install as your normal user).
2. Allow the install script to run for your user profile, if needed:
   ```powershell
   Get-ExecutionPolicy -Scope CurrentUser
   ```
   If the result is `Restricted` or `AllSigned`, set it to `RemoteSigned`:
   ```powershell
   Set-ExecutionPolicy -Scope CurrentUser -ExecutionPolicy RemoteSigned
   ```
3. Install Claude Code:
   ```powershell
   irm https://claude.ai/install.ps1 | iex
   ```
4. Close PowerShell and open a **new** one (so PATH refreshes), then verify:
   ```powershell
   claude --version
   ```
   If `claude` is not recognized, the installer occasionally fails to append its install folder to your user PATH on certain Windows configurations (e.g. domain-joined machines where the user profile contains a dot). The binary lives at:
   ```
   %LOCALAPPDATA%\anthropic\claude\claude.exe
   ```
   Add `%LOCALAPPDATA%\anthropic\claude` to your user PATH manually if needed (System Properties → Environment Variables → User Path → Edit → New).
5. First-run authentication:
   ```powershell
   claude
   ```
   A browser tab opens. Sign in with the account on the Claude Pro/Max subscription, or paste an Anthropic API key when prompted. Authentication persists; you do not have to repeat it.

> If you ever need to refer to the official documentation, the Claude Code docs are at https://docs.claude.com/en/docs/claude-code/overview. Treat the docs as authoritative and any third-party blog post (including this one) as best-effort guidance.

---

## 3. Install the rest of the toolchain

These pieces are not Claude-Code-specific but Claude Code expects them to be available when it runs build/test commands on its own.

### 3a. Git

If `git --version` does not work in PowerShell, install **Git for Windows** from https://git-scm.com/download/win. Default options are fine. This also installs Git Bash, which Claude Code uses to run shell commands on Windows.

Configure your identity once:
```powershell
git config --global user.name  "Your Name"
git config --global user.email "you@example.com"
```

Authenticate against GitHub. Two good options:
- **GitHub CLI** (cleanest): install from https://cli.github.com/, then `gh auth login` and follow the browser prompt. Subsequent `git push` / `git pull` over HTTPS work without re-entering credentials.
- **SSH keys**: generate with `ssh-keygen -t ed25519 -C "you@example.com"`, add the public key under GitHub → Settings → SSH and GPG keys.

### 3b. Python

VS2026 may have installed a Python; if not, install **Python 3.12** from https://www.python.org/downloads/ — tick "Add python.exe to PATH" in the installer. Verify:
```powershell
python --version
pip --version
```

### 3c. Npcap (for the EtherCAT slave to bind a NIC raw)

We only need this when we get to Milestone 1 (wire-level smoke test), but install it now to avoid an interruption later.

1. Download the latest **Npcap installer** and the **Npcap SDK** from https://npcap.com/#download.
2. Run the installer. During setup:
   - Tick **"Install Npcap in WinPcap API-compatible Mode"** — this maximizes compatibility with libraries written against the old WinPcap API.
   - Tick **"Support raw 802.11 traffic"** if available; harmless if not relevant to us.
3. Unzip the SDK to a stable location, e.g. `C:\dev\sdk\npcap-sdk`. We'll point CMake at it from there.
4. Note for license clarity: the free Npcap can be installed on up to 5 systems for free; our use is internal development testing on this PC, which is covered.

### 3d. Visual Studio 2026 — confirm the right components

From the Visual Studio Installer, make sure these are ticked under your VS2026 installation:
- **Desktop development with C++** workload (gives MSVC, Windows SDK, CMake, Ninja).
- Under that workload's individual components, confirm:
  - **MSVC v145 — VS 2026 C++ x64/x86 build tools (latest)**
  - **Windows 11 SDK** (latest available)
  - **C++ CMake tools for Windows**
  - **vcpkg package manager** (optional but convenient)

Confirm in a **Developer PowerShell for VS 2026** (Start menu → search "Developer PowerShell"):
```powershell
cl
cmake --version
ninja --version
```
You should see MSVC ≥ 19.50 (which is the 14.50 / v145 toolset that ships with VS2026 18.0+), CMake ≥ 4.1, Ninja present.

> **Why a Developer PowerShell?** It has the MSVC environment variables (INCLUDE, LIB, PATH) baked in for the current session. Claude Code will run builds from here.

---

## 4. The repo

### 4a. Create on GitHub

1. Go to https://github.com/new.
2. Owner = your account. Name = `ethercat-sim` (or whatever you prefer).
3. **Private** is the safer default; we are impersonating Beckhoff terminals and embedding their ESI references — private avoids accidentally publishing a public clone-and-go EtherCAT impersonator.
4. Initialize **without** a README, .gitignore, or license (we'll add these locally to keep history clean).
5. Create.

### 4b. Clone and set up locally

Pick a path that **does not** contain spaces or non-ASCII characters — MSVC and some CMake macros are happier that way. Example: `C:\dev\ethercat-sim`.

In a Developer PowerShell for VS 2026:
```powershell
cd C:\dev
git clone https://github.com/<your-username>/ethercat-sim.git
cd ethercat-sim
```

### 4c. Drop in the docs

Copy the four documents from this conversation into `docs/`:
```
ethercat-sim/
  docs/
    REQUIREMENTS.md
    HANDOVER.md
    PLC_SETUP.md
    ARCHITECTURE.html
    GETTING_STARTED.md      # this file
```

### 4d. Add the `.gitignore`

Create `.gitignore` at the repo root with:
```
# Build outputs
build/
out/
*.obj
*.exe
*.dll
*.pdb
*.ilk
*.exp
*.lib
.vs/

# CMake
CMakeCache.txt
CMakeFiles/
cmake_install.cmake
compile_commands.json
CMakeUserPresets.json

# Python
__pycache__/
*.pyc
.venv/
venv/
*.egg-info/

# IDE / editor
.vscode/
*.user
*.suo

# Logs and captures
*.log
*.pcap
*.pcapng

# Local Claude Code session state (not project memory)
.claude/state/
```

### 4e. Add the `CLAUDE.md` — most impactful single step

This is the file Claude Code automatically loads at the start of every session. Create `CLAUDE.md` at the repo root with the content below. Adjust paths and conventions as the project evolves.

```markdown
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
```

Commit:
```powershell
git add docs CLAUDE.md .gitignore
git commit -m "docs: seed living documents, CLAUDE.md, gitignore"
git push -u origin main
```

### 4f. Skeleton directories

Create the empty directories now so they appear in the tree from day one:
```powershell
New-Item -ItemType Directory sim\ec-core, sim\devices, sim\control-plane,
    configs\schema, configs\stations, configs\esi, configs\iodd,
    test\pysoem, test\unit, test\twincat,
    tools | Out-Null

# Placeholder files so git tracks the empty dirs
foreach ($d in @(
  "sim\ec-core","sim\devices","sim\control-plane",
  "configs\schema","configs\stations","configs\esi","configs\iodd",
  "test\pysoem","test\unit","test\twincat","tools")) {
  New-Item -ItemType File "$d\.gitkeep" | Out-Null
}

git add .
git commit -m "chore: skeleton directories"
git push
```

---

## 5. First Claude Code session

In the **Developer PowerShell for VS 2026**, from the repo root:

```powershell
cd C:\dev\ethercat-sim
claude
```

Claude Code starts. It will load `CLAUDE.md` and the `docs/` files automatically (the root CLAUDE.md is loaded eagerly; the docs are loaded lazily as Claude reads them).

A productive first prompt:

> Read the four documents in `docs/`. Then read CLAUDE.md. Then summarize back to me in 10 lines what you understand the project is, what state we are in, and what you would do next. Do not write code or change files yet.

Read the summary carefully. If anything is wrong, correct it once — Claude Code will adjust for the rest of the session, and the correction is a candidate to add to `CLAUDE.md`.

A productive second prompt:

> Generate the top-level `CMakeLists.txt` and a `sim/ec-core/CMakeLists.txt` for a single executable that links against the Npcap SDK. Hardcode the Npcap SDK path as `C:/dev/sdk/npcap-sdk` for now — we can make it a CMake option later. The executable should be empty `main()` returning 0; we just want a buildable skeleton. Show me the files before writing them.

The "show me before writing" part keeps you in control. Once you see Claude's plan, type `y` (or use **plan mode** — Shift+Tab — to formalize this).

---

## 6. How we work with Claude Code

These are recommendations, not rules. Tune as you go and add the bits that work into `CLAUDE.md`.

### 6a. Explore → Plan → Implement → Commit

Anthropic recommends this loop, and it matches the milestone structure in `docs/HANDOVER.md` §4.
- **Explore:** ask Claude to read relevant files and summarize. Don't ask for code yet.
- **Plan:** ask for a step-by-step plan with the exact files to change. Use plan mode (Shift+Tab) if the change spans several files.
- **Implement:** approve the plan; Claude writes the code.
- **Commit:** review the diff. If it's good, commit with a focused message and push.

### 6b. One change at a time

The fastest way to lose a Claude Code session is to ask it to "do milestone 3 and milestone 4 together." Always work one milestone at a time. Inside a milestone, work one file at a time.

### 6c. Keep `CLAUDE.md` tight

A bloated `CLAUDE.md` makes every session more expensive (more tokens loaded into context). The version in §4e is already on the long side; trim sections that prove unused after a few weeks.

Use the in-session `#` shortcut to capture a one-off instruction ("from now on, prefer `std::string_view` over `const std::string&` in interfaces") — at the end of the session, decide whether to promote it into `CLAUDE.md` or let it stay session-only.

### 6d. Subdirectory CLAUDE.md files

When a subdirectory has its own conventions, add a `CLAUDE.md` there. Claude Code loads it **lazily** — only when it reads files in that subdirectory. Good candidates as the project grows:
- `sim/ec-core/CLAUDE.md` — performance-critical rules, no-allocation conventions, datagram-handling patterns.
- `sim/devices/CLAUDE.md` — object dictionary conventions, profile state-machine patterns.
- `test/CLAUDE.md` — test naming, fixture conventions.

Do not duplicate the root `CLAUDE.md`. Subdirectory files extend, they don't replace.

### 6e. Use git as the safety net

Always work in a feature branch. Before any non-trivial Claude Code task:
```powershell
git switch -c feat/milestone-2-sii-emulation
```
After the task, review with `git diff`, run the build and tests, then PR into `main`. Even on a solo project, the PR view is the best way to catch unintended changes Claude made in passing.

### 6f. Wireshark in the loop

EtherCAT debugging without Wireshark is painful. Install Wireshark (it picks up Npcap automatically if Npcap is already installed). Filter `eth.type == 0x88a4` for EtherCAT frames. Save captures into `.gitignored` paths inside the repo for ad-hoc analysis; they don't belong in version control.

### 6g. The headless harness is your fastest feedback loop

Once Milestone 9 (CI green) is reached, the pysoem harness becomes the fastest way to validate slave-side changes. Ask Claude Code to write the pysoem test alongside any L2/L3 change; that test is reusable and runs without TwinCAT.

---

## 7. Optional but worth considering

These are not setup blockers — defer until you actually want them.

### 7a. Claude Code skills

Skills are reusable procedure folders Claude Code can load on demand. Two that would fit this project well:
- A **C++ coding standards** skill that enforces the conventions in `CLAUDE.md` §"Code style".
- A **release-prep** skill that runs the soak test, captures a Wireshark trace, and updates `docs/HANDOVER.md` §2 (current state).

These are nice-to-have after the project has a working v1.

### 7b. Clangd MCP server for code intelligence

If the C++ codebase grows past a few thousand lines, the **clangd MCP server** gives Claude Code symbol-level intelligence (jump to definition, find references) instead of grep-style search. It needs `compile_commands.json` (which our CMake setup already emits) and the clangd binary. Skip until it actually feels needed.

### 7c. Pre-commit hooks

Once the toolchain settles, add `pre-commit` with:
- `clang-format` for C++
- `ruff` for Python
- `cmake-format` for CMakeLists.txt
- A topology-YAML schema validator

This keeps PRs focused on substantive review instead of formatting nits.

---

## 8. Daily flow once set up

```
1. cd C:\dev\ethercat-sim
2. git switch -c feat/<milestone>     # or continue on an existing branch
3. claude                              # start Claude Code
4. Skim docs/HANDOVER.md §3 and §4 to remind yourself where we are.
5. Work the next milestone task with Claude.
6. cmake --build build && ctest --test-dir build --output-on-failure
7. git add -p ; git commit -m "<focused message>" ; git push
8. Open a PR on GitHub. Self-review. Merge.
9. Update docs/HANDOVER.md (§2 checkboxes, §3 decisions log if needed).
```

That's it.

---

## 9. Quick troubleshooting

| Symptom | Action |
|---|---|
| `claude: command not found` | Check that `%LOCALAPPDATA%\anthropic\claude` is on your user PATH; re-open PowerShell. |
| Build fails to find `pcap.h` or `wpcap.lib` | Verify the Npcap SDK path in the top-level `CMakeLists.txt`. Default we set: `C:/dev/sdk/npcap-sdk`. |
| `cmake` not found | You opened a regular PowerShell, not the Developer PowerShell for VS 2026. CMake is on the Developer PowerShell's PATH. |
| Claude Code does not appear to read `docs/HANDOVER.md` | Confirm the file is in `docs/`, not `Docs/`. Git is case-sensitive even on Windows; an accidental case mismatch breaks the link. |
| Claude is making changes outside what you asked for | Stop, run `git status` / `git diff`, revert with `git restore <file>`. Then refine the prompt to be narrower. Plan mode (Shift+Tab) prevents this category of surprise. |
| GitHub push fails with auth error | Re-run `gh auth login`, or check that your SSH key is added to the GitHub account. |

---

_End of getting-started._
