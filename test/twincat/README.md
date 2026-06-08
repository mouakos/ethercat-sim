# test/twincat — real-PLC rig solutions

TwinCAT 3 XAE solutions that drive the **real** Beckhoff master against our software
slave (`ec-core --serve`) over a real Ethernet cable. Committing them here means the
exact PLC-side config is version-controlled and can never be lost to an unsaved IDE
session again (see HANDOVER §3, the "TwinCAT project not saved" incident).

## The two-layer rule (why a running PLC ≠ a saved project)

TwinCAT keeps two *separate* things. Losing one does not lose the other:

| | Engineering project | Runtime / boot config |
|---|---|---|
| What | `.sln` + `.tsproj` you edit in XAE | the compiled config that runs the I/O |
| Lives | here, in this repo | `C:\TwinCAT\3.1\Boot\` on the target |
| Saved by | **File → Save All** | **Activate Configuration** |
| Survives reboot | only if saved to a real path | yes (runtime auto-loads it) |

After *any* change you must do **both**: `File → Save All` (persists the source here)
**and** `Activate Configuration` (writes the boot config so the runtime survives a
reboot). They are different buttons.

## Layout

```
test/twincat/
  .gitignore                  # ignores _Boot/ _CompileInfo/ .vs/ *.tslock etc.
  README.md                   # this file
  rig-ek1100-el1008/          # solution: EK1100 coupler + EL1008 8x DI
    rig-ek1100-el1008.sln
    rig-ek1100-el1008/...     # .tsproj + I/O tree + Task 1
```

One subfolder per rig configuration. Add a new subfolder (don't overwrite) when the
emulated station changes — e.g. `rig-ek1100-el1008-el2008/` once the EL2008 lands.

## Rig facts (kept in sync with HANDOVER §5)

- **Master:** Windows Server 2019, TwinCAT 3, Intel I210-T1 NIC as the RT adapter.
- **Slave:** `ec-core --serve` on the dev machine, NIC
  `\Device\NPF_{DC25F4C3-68B2-480B-A3D5-156CBEDF29FC}`.
- **Mode:** Config-Mode → Activate Free Run, 10 ms cycle (held OP ~100 s clean).
- The slave (`ec-core`) must be **serving** before you scan, or the bus scan finds
  nothing.

## Recreating the project (summary — full steps in the chat / HANDOVER)

1. New TwinCAT XAE Project → **Save into `test/twincat/rig-ek1100-el1008/`**.
2. I/O → Devices → add the I210 as the **EtherCAT master (RT adapter)**.
3. With `ec-core --serve` running: right-click the device → **Scan** → accept
   EK1100 + EL1008. (Or add the boxes manually from the Beckhoff ESI.)
4. Add **Task 1** (10 ms / "10 ticks @ 1 ms base"), create an input variable, link it
   to the EL1008 channel.
5. **File → Save All**, then commit & push from this folder.
6. **Activate Configuration → Free Run** to run the bus.
