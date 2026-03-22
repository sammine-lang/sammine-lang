#!/usr/bin/env python3
"""Check per-target optimization and debug flags in compile_commands.json."""

import json
import sys
from pathlib import Path


def main():
    build_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("build")
    db = json.loads((build_dir / "compile_commands.json").read_text())

    categories = {
        "cpptrace": lambda f: "_deps/cpptrace" in f
        and "libdwarf" not in f
        and "zstd" not in f,
        "fmt": lambda f: "_deps/fmt" in f,
        "Catch2": lambda f: "_deps/catch2" in f,
        "zstd": lambda f: "zstd" in f,
        "SammineCheck": lambda f: "SammineCheck" in f,
        "MAIN": lambda f: "_deps" not in f and "SammineCheck" not in f,
    }

    print(f"{'target':<15} {'opt':<14} {'debug':<14} {'NDEBUG'}")
    print("-" * 55)

    for cat, match in categories.items():
        entries = [e for e in db if match(e["file"])]
        if not entries:
            continue
        cmd = entries[0]["command"]
        has_O2 = "-O2" in cmd
        has_g0 = "-g0" in cmd
        has_g = " -g " in cmd or cmd.endswith("-g")
        has_NDEBUG = "NDEBUG" in cmd
        opt = "-O2" if has_O2 else "debug-default"
        dbg = "none(-g0)" if has_g0 else "-g" if has_g else "none"
        print(f"{cat:<15} {opt:<14} {dbg:<14} {has_NDEBUG}")


if __name__ == "__main__":
    main()
