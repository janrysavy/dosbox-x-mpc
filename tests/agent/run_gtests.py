#!/usr/bin/env python3
"""Run the DOSBox-X GoogleTest suite without opening or pausing a console."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys


SOURCE_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_EXE = SOURCE_ROOT / "bin" / "x64" / "Agent Debug SDL2" / "dosbox-x.exe"
DEFAULT_LOG = SOURCE_ROOT / "tests" / "agent" / "gtest-results.log"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    parser.add_argument("--log", type=Path, default=DEFAULT_LOG)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()

    exe = args.exe.resolve()
    log = args.log.resolve()
    config = SOURCE_ROOT / "dosbox-x.reference.conf"
    if not exe.is_file():
        parser.error(f"DOSBox-X executable does not exist: {exe}")
    if not config.is_file():
        parser.error(f"DOSBox-X config does not exist: {config}")
    try:
        log.relative_to(SOURCE_ROOT)
    except ValueError:
        parser.error(f"log must stay inside the DOSBox-X source tree: {log}")

    log.parent.mkdir(parents=True, exist_ok=True)
    if log.exists():
        log.unlink()

    env = os.environ.copy()
    env["ProgramData"] = r"C:\ProgramData"
    env["ALLUSERSPROFILE"] = r"C:\ProgramData"
    command = [
        str(exe),
        "-tests",
        # This must follow -tests because -tests enables the Win32 console.
        "-noconsole",
        "-conf",
        str(config),
        "-nopromptfolder",
        "-set",
        "sdl waitonerror=false",
    ]
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    try:
        completed = subprocess.run(
            command,
            cwd=SOURCE_ROOT,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=args.timeout,
            creationflags=creationflags,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")
        log.write_text(output + f"\nTIMEOUT after {args.timeout:g} seconds\n", encoding="utf-8")
        print(log.read_text(encoding="utf-8"), end="")
        print(f"GoogleTest timed out; full output: {log}", file=sys.stderr)
        return 124

    log.write_text(completed.stdout, encoding="utf-8")
    print(completed.stdout, end="")
    print(f"GoogleTest exit code: {completed.returncode}")
    print(f"Full output: {log}")
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
