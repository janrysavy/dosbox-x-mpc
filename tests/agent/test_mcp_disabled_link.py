#!/usr/bin/env python3
"""Compile, link and run the production MCP fallback in three configurations."""

import argparse
import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--source", type=Path,
                        default=ROOT / "src/debug/debug_mcp.cpp")
    args = parser.parse_args()
    # Every case intentionally selects the disabled-transport fallback.
    for name, debug, network in (("debug-on-network-off", 1, 0),
                                 ("debug-off-network-off", 0, 0),
                                 ("debug-off-network-on", 0, 1)):
        output = ROOT / "_build/mcp-disabled-link" / name
        output.mkdir(parents=True, exist_ok=True)
        temporary = output / "tmp"
        temporary.mkdir(exist_ok=True)
        environment = dict(os.environ, TMP=str(temporary), TEMP=str(temporary),
                           TMPDIR=str(temporary))
        (output / "config.h").write_text(
            f"#define C_DEBUG {debug}\n#define C_SDL_NET {network}\n"
            "#define C_SDL2_NET 0\n", encoding="ascii")
        executable = output / "mcp-disabled-link.exe"
        executable.unlink(missing_ok=True)
        sources = [str(args.source.resolve()),
                   str(ROOT / "tests/agent/mcp_disabled_link.cpp")]
        if Path(args.compiler).stem.lower() == "cl":
            command = [args.compiler, "/nologo", "/std:c++17", "/EHsc",
                       "/I" + str(output), "/I" + str(ROOT / "src/debug"),
                       *sources, "/Fe:" + str(executable),
                       "/Fo" + str(output) + os.sep]
        else:
            command = [args.compiler, "-std=c++17", "-I", str(output),
                       "-I", str(ROOT / "src/debug"), *sources,
                       "-o", str(executable)]
        print(f"BUILD {name}: {command!r}", flush=True)
        subprocess.run(command, check=True, cwd=output, env=environment)
        subprocess.run([str(executable)], check=True, cwd=output, env=environment)
        print(f"PASS {name}: production fallback linked and ran", flush=True)


if __name__ == "__main__":
    main()
