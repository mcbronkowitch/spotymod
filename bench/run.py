#!/usr/bin/env python3
"""Build the bench firmware, load it into the Seed's SRAM through the debug
probe, and capture its semihosting output. One command, one cable."""

import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
OPENOCD = r"C:\Program Files\DaisyToolchain\bin\openocd.exe"
SCRIPTS = r"C:\Program Files\DaisyToolchain\openocd\scripts"
ELF = os.path.join(HERE, "build", "bench.elf")


def build():
    subprocess.run(["make", "-j8"], cwd=HERE, check=True)


def run_once(interface, timeout):
    """Load and run the image, reading openocd's output until BENCH_END.

    openocd is both the loader and the semihosting server, so it stays alive
    for the whole run and its stdout IS the capture. Returns the captured
    lines, or None on timeout -- a hang writes nothing.
    """
    cmd = [
        OPENOCD,
        "-s", SCRIPTS,
        "-f", "interface/%s" % interface,
        "-f", "target/stm32h7x.cfg",
        "-c", "set IMAGE {%s}" % ELF.replace("\\", "/"),
        "-f", os.path.join(HERE, "openocd", "spotykach-sram.cfg"),
    ]
    # openocd logs to stderr and semihosting output can land on either --
    # merge them so no line is missed.
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, bufsize=1)
    deadline = time.time() + timeout
    lines, done = [], False
    try:
        # iter(readline) NOT `for raw in proc.stdout`: the latter uses Python's
        # read-ahead buffer, which on Windows blocks until the pipe fills and
        # deadlocks this loop. readline() returns per line. Verified on hardware.
        for raw in iter(proc.stdout.readline, ""):
            line = raw.rstrip("\r\n")
            print(line)
            lines.append(line)
            if line.startswith("BENCH_END"):
                done = True
                break
            if time.time() > deadline:
                break
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    return lines if done else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--interface", default="stlink-dap.cfg",
                    help="openocd interface cfg; this desk's probe is an ST-Link V3")
    ap.add_argument("--transport", default="semihost", choices=["semihost"],
                    help="capture transport (USB-CDC fallback: see bench/README.md)")
    ap.add_argument("--timeout", type=float, default=600.0,
                    help="seconds to wait for BENCH_END")
    ap.add_argument("--build-only", action="store_true")
    ap.add_argument("--no-build", action="store_true")
    args = ap.parse_args()

    if not args.no_build:
        build()
    if args.build_only:
        return 0

    lines = run_once(args.interface, args.timeout)
    if lines is None:
        print("ERROR: BENCH_END never arrived (timeout or openocd exited)",
              file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
