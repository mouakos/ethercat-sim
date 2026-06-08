#!/usr/bin/env python3
"""ec-core process control — check whether the ec-core slave is running, and stop it.

A small dev helper so the EtherCAT slave responder (``ec-core.exe``) can be
inspected and stopped without hunting for it by hand. The build fails with a
"file in use" link error while the .exe is running, so this is the one-liner to
clear the way before a rebuild.

Usage::

    python tools/ec_core_ctl.py status   # is ec-core running? print PID(s)
    python tools/ec_core_ctl.py kill      # terminate all ec-core.exe processes

Windows-only (ec-core runs on Windows via Npcap). Dependency-free: it shells out
to the built-in ``tasklist`` / ``taskkill`` commands.
"""

from __future__ import annotations

import csv
import io
import subprocess
import sys

PROCESS_NAME = "ec-core.exe"


def find_pids() -> list[int]:
    """Return the PIDs of running ec-core.exe processes (empty list if none)."""
    result = subprocess.run(
        ["tasklist", "/fi", f"imagename eq {PROCESS_NAME}", "/fo", "csv", "/nh"],
        capture_output=True,
        text=True,
        check=False,
    )
    # tasklist prints "INFO: No tasks are running ..." (not CSV) when nothing matches.
    if "No tasks" in result.stdout or not result.stdout.strip():
        return []
    pids: list[int] = []
    for row in csv.reader(io.StringIO(result.stdout)):
        if len(row) >= 2 and row[0].strip().lower() == PROCESS_NAME.lower():
            try:
                pids.append(int(row[1]))
            except ValueError:
                continue
    return pids


def status() -> int:
    pids = find_pids()
    if pids:
        print(f"{PROCESS_NAME} RUNNING - pid(s): {', '.join(map(str, pids))}")
    else:
        print(f"{PROCESS_NAME} not running")
    return 0


def kill() -> int:
    pids = find_pids()
    if not pids:
        print(f"{PROCESS_NAME} not running - nothing to kill")
        return 0
    result = subprocess.run(
        ["taskkill", "/f", "/im", PROCESS_NAME],
        capture_output=True,
        text=True,
        check=False,
    )
    if find_pids():
        print(f"FAILED to kill {PROCESS_NAME}: {result.stdout.strip()} {result.stderr.strip()}")
        return 1
    print(f"killed {PROCESS_NAME} (was pid(s): {', '.join(map(str, pids))})")
    return 0


def main(argv: list[str]) -> int:
    cmd = argv[1].lower() if len(argv) > 1 else "status"
    if cmd == "status":
        return status()
    if cmd in {"kill", "stop"}:
        return kill()
    print(__doc__)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
