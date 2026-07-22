#!/usr/bin/env python3
"""Build the bench firmware, load it into the Seed's SRAM through the debug
probe, and capture its semihosting output. One command, one cable."""

import argparse
import csv
import datetime
import io
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


BUDGET_CYCLES = 960000


def parse(lines):
    """Pull the marker-delimited payload out of a capture. Returns
    (header, rows, anchors) or None if the run never completed."""
    header, rows, anchors = None, [], []
    for line in lines:
        if line.startswith("BENCH_BEGIN,"):
            f = line.split(",")
            header = {"githash": f[1], "clock": f[2], "block": f[3], "cache": f[4]}
        elif line.startswith("BENCH,"):
            f = line.split(",")
            rows.append({
                "family": f[1], "name": f[2], "avg_cyc": f[3], "max_cyc": f[4],
                "pct_avg": f[5], "pct_max": f[6], "checksum": f[7],
            })
        elif line.startswith("ANCHOR,"):
            f = line.split(",")
            anchors.append({"name": f[1], "avg_pct": f[2], "max_pct": f[3]})
    if header is None or not rows:
        return None
    return header, rows, anchors


def by_name(rows):
    return {r["name"]: r for r in rows}


def current_head():
    """git rev-parse --short HEAD for REPO, same abbreviation the firmware
    embeds (bench/Makefile's git_hash.h uses the identical command)."""
    return subprocess.run(
        ["git", "-C", REPO, "rev-parse", "--short", "HEAD"],
        capture_output=True, text=True, check=True).stdout.strip()


def check_hash(header):
    """Guard against a mislabelled result file. bench/Makefile's
    git_hash.h rule gives the embedded hash a real dependency edge onto
    the object that reports it, but that only prevents ONE way this can
    go stale (an un-rebuilt object). This is the loud, independent
    host-side check that catches staleness from any source -- a flashed
    image that predates a `git checkout`, a build directory copied from
    elsewhere, etc. A mislabelled result file is worse than no result
    file, so this is an error, not a warning: return False and the caller
    must abort without writing anything.
    """
    embedded = header["githash"]
    head = current_head()
    if embedded != head:
        print("ERROR: firmware reports git hash %r but the working tree's "
              "HEAD is %r -- the flashed image does not match the code "
              "you think you're measuring. Writing nothing." % (embedded, head),
              file=sys.stderr)
        return False
    return True


def ratio(rows, num, den):
    """Cycle ratio between two rows, as a raw float (or an error string)."""
    d = by_name(rows)
    if num not in d or den not in d:
        return "n/a (row missing)"
    try:
        a, b = float(d[num]["avg_cyc"]), float(d[den]["avg_cyc"])
    except ValueError:
        return "n/a (TIMEOUT)"
    if b <= 0:
        return "n/a (zero denominator)"
    return a / b


def sig2(x):
    """Format a ratio to 2 significant figures for verdict PROSE only --
    e.g. 5.28 -> "5.3", 44.33 -> "44", 0.29 -> "0.29". Measured intra-run
    jitter (~1700 cycles on a 1.5M-cycle workload) and a cross-build layout
    shift (~7% on a 29K-cycle workload) mean anything past this is noise
    dressed up as precision. The data tables keep full precision; this
    helper is not used there."""
    if not isinstance(x, float):
        return x  # error string from ratio(), pass through unrounded
    if x == 0:
        return "0x"
    import math
    exp = math.floor(math.log10(abs(x)))
    decimals = max(0, 1 - exp)
    r = round(x, decimals)
    return ("%dx" % round(r)) if decimals <= 0 else ("%.*fx" % (decimals, r))


def pct1(s):
    """Format a firmware percentage string to whole-number prose precision.
    The firmware prints hundredths (e.g. "165.08"); verdict prose rounds to
    a whole number so it doesn't imply resolution the hardware jitter can't
    support."""
    try:
        return "%.0f" % float(s)
    except ValueError:
        return s


def verdict(rows, anchors):
    """The paragraph the spec's acceptance criterion 2 asks for: the three
    questions this bench exists to answer, answered in prose."""
    d = by_name(rows)
    a = {x["name"]: x for x in anchors}
    out = io.StringIO()

    worst = d.get("instrument_worst")
    worst_anchor = a.get("instrument_worst")
    out.write("## Verdict\n\n")

    if worst and worst["avg_cyc"] != "TIMEOUT":
        offline = pct1(worst["pct_max"])
        anchored_raw = worst_anchor["max_pct"] if worst_anchor else None
        anchored = pct1(anchored_raw) if anchored_raw is not None else "not anchored"
        try:
            fits = float(anchored_raw) < 100.0 if anchored_raw is not None else None
        except ValueError:
            fits = None
        if fits is True:
            conclusion = ("**Conclusion: the 2x4 architecture fits.** The "
                          "anchored figure is under 100 % of the block budget.")
        elif fits is False:
            conclusion = ("**Conclusion: the 2x4 architecture does not fit.** "
                          "The anchored figure is over 100 % of the block "
                          "budget, so the design has to shed voices or FX.")
        else:
            conclusion = ("**Conclusion: undetermined.** No anchored figure "
                          "was measured to compare against the 100 % line.")
        out.write(
            "**2x4 budget — go/no-go.** The full instrument at its worst case "
            "(8 voices, COLOR 4-note on both parts, all FX on, high diffusion, "
            "echo at max) costs **%s %% of the block budget offline**, and "
            "**%s %% measured inside a real audio callback**. The anchored "
            "figure is the one that decides. %s\n\n"
            % (offline, anchored, conclusion))
    else:
        out.write("**2x4 budget — NO RESULT.** `instrument_worst` did not "
                  "produce a number. The go/no-go question is unanswered.\n\n")

    # Ratios are against synth_1_voice -- ONE REAL spotymod voice (two MorphOsc
    # in unison + sub + SVF + envelope). NOT against morph_osc_bare, which is a
    # single oscillator kernel and ~7.3x cheaper; anchoring on that inflates
    # every ratio by that factor and misranks the table.
    out.write("**Cost per candidate, relative to one real spotymod voice.**\n\n")
    for r in rows:
        if r["family"] == "voice" and r["name"] != "morph_osc_bare":
            out.write("- `%s` — %s one real voice (%s a bare oscillator kernel)\n"
                      % (r["name"],
                         sig2(ratio(rows, r["name"], "synth_1_voice")),
                         sig2(ratio(rows, r["name"], "morph_osc_bare"))))
    out.write("\n")

    out.write(
        "**SRAM vs SDRAM.** The grain-read proxy (8 scattered interpolated "
        "stereo reads per sample, identical window in both regions) costs "
        "**%s** in SDRAM against SRAM. That is a bare access pattern, written "
        "before the sampler existed to stand in for it; the `sampler_win_*` "
        "pair below is the same contrast with the real engine around it. The "
        "Oliverb pair reads **%s**, and the shortened echo-style streaming "
        "walk **%s**.\n\n"
        % (sig2(ratio(rows, "grain_read_sdram", "grain_read_sram")),
           sig2(ratio(rows, "oliverb_sdram", "oliverb_solo_sram")),
           sig2(ratio(rows, "echo_walk_sdram", "echo_walk_sram"))))

    sw = d.get("inst_sampler_worst")
    if sw and sw["avg_cyc"] != "TIMEOUT":
        try:
            over = float(sw["pct_max"]) >= 100.0
        except ValueError:
            over = None
        if over is True:
            call = ("**Conclusion: the texture deck fits on average and "
                    "overruns on peaks.** Its worst block is over the budget "
                    "even though its mean block is under it, so what needs "
                    "capping is the spawn burst, not the steady cloud.")
        elif over is False:
            call = ("**Conclusion: a two-part texture deck fits**, peaks "
                    "included.")
        else:
            call = "**Conclusion: undetermined.**"
        out.write(
            "**The texture deck.** Both parts on the sampler at its worst "
            "case — DENS 8, SIZE 0.05 (128-sample grains spawning every 16), "
            "MOTION 1 scattering reads over the whole 42 s record buffer, "
            "SCAN running, and instrument_worst's FX chain unchanged around "
            "it — costs **%s %% of the block budget on the mean block and "
            "%s %% on the worst**, against **%s %% / %s %%** for the same box "
            "with both parts on the synth. The mean is the cheaper of the "
            "two; the peak is not. %s\n\n"
            % (pct1(sw["pct_avg"]), pct1(sw["pct_max"]),
               pct1(worst["pct_avg"]) if worst else "n/a",
               pct1(worst["pct_max"]) if worst else "n/a",
               call))
        out.write(
            "Solo, the same cloud is **%s %%** mean / **%s %%** peak, a "
            "musical setting (DENS 4, half-second grains, MOTION 0.5, "
            "playhead parked) is **%s %%** mean, and running an overdub "
            "underneath the worst-case cloud costs **%s** its mean. "
            "SCAN driven at the VCV host's rate — every 16 samples, six "
            "times the engine's own control tick, the `std::pow` "
            "`SamplerEngine::set_scan`'s comment flags — reads **%s** against "
            "the same cloud without it. With the whole engine around it the "
            "record buffer's region costs **%s** (`sampler_win_sdram` over "
            "`sampler_win_sram`, identical settings and identical 8192-frame "
            "content), well under the bare proxy's figure above: the "
            "scheduler, window, filter and normalization are all region-blind "
            "and dilute it.\n\n"
            % (pct1(d["sampler_flow_worst"]["pct_avg"]),
               pct1(d["sampler_flow_worst"]["pct_max"]),
               pct1(d["sampler_flow_typ"]["pct_avg"]),
               sig2(ratio(rows, "sampler_overdub_worst", "sampler_flow_worst")),
               sig2(ratio(rows, "sampler_scan_ctrl", "sampler_flow_worst")),
               sig2(ratio(rows, "sampler_win_sdram", "sampler_win_sram"))))

    out.write(
        "*Figures in this section are quoted to whole percentage points and "
        "two significant figures for ratios — honest to what this bench can "
        "actually resolve (intra-run jitter of roughly 1700 cycles on a "
        "1.5M-cycle workload, and a cross-build layout shift that moved a "
        "29K-cycle workload by about 7%). The tables below retain full "
        "measured precision.*\n\n")
    return out.getvalue()


def write_results(out_dir, header, rows, anchors, drift):
    os.makedirs(out_dir, exist_ok=True)
    stamp = datetime.date.today().isoformat()
    base = os.path.join(out_dir, "%s-%s" % (stamp, header["githash"]))

    with open(base + ".csv", "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["family", "name", "avg_cyc", "max_cyc",
                    "pct_avg", "pct_max", "checksum"])
        for r in rows:
            w.writerow([r["family"], r["name"], r["avg_cyc"], r["max_cyc"],
                        r["pct_avg"], r["pct_max"], r["checksum"]])

    with open(base + ".md", "w", encoding="utf-8") as fh:
        fh.write("# Bench run %s — `%s`\n\n" % (stamp, header["githash"]))
        fh.write("Measured on a Daisy Seed (STM32H750). %s Hz core clock, "
                 "block size %s, %s, `-ffast-math -funroll-loops`. "
                 "Block budget %d cycles.\n\n"
                 % (header["clock"], header["block"], header["cache"],
                    BUDGET_CYCLES))
        if drift:
            fh.write("> **WARNING — checksum drift between runs.** %s\n>\n"
                     "> Determinism is a measured property of this engine, not "
                     "an assumption. These numbers are suspect until the drift "
                     "is explained.\n\n" % drift)
        fh.write(verdict(rows, anchors))
        fh.write("## Offline table\n\n")
        fh.write("| family | workload | avg cyc | max cyc | avg % | max % | checksum |\n")
        fh.write("|---|---|---:|---:|---:|---:|---|\n")
        for r in rows:
            fh.write("| %s | `%s` | %s | %s | %s | %s | `%s` |\n"
                     % (r["family"], r["name"], r["avg_cyc"], r["max_cyc"],
                        r["pct_avg"], r["pct_max"], r["checksum"]))
        if anchors:
            fh.write("\n## Anchor mode (real audio callback, CpuLoadMeter)\n\n")
            fh.write("| workload | avg % | max % |\n|---|---:|---:|\n")
            for x in anchors:
                fh.write("| `%s` | %s | %s |\n"
                         % (x["name"], x["avg_pct"], x["max_pct"]))
    return base


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
    ap.add_argument("--repeat", type=int, default=2,
                    help="runs to compare for the determinism check")
    ap.add_argument("--out-dir", default=os.path.join(REPO, "docs", "bench"))
    args = ap.parse_args()

    if not args.no_build:
        build()
    if args.build_only:
        return 0

    captures = []
    for i in range(max(1, args.repeat)):
        print("# run %d/%d" % (i + 1, args.repeat), file=sys.stderr)
        lines = run_once(args.interface, args.timeout)
        if lines is None:
            print("ERROR: BENCH_END never arrived (timeout or openocd exited)",
                  file=sys.stderr)
            return 2
        parsed = parse(lines)
        if parsed is None:
            print("ERROR: capture completed but held no usable rows",
                  file=sys.stderr)
            return 2
        header, _rows, _anchors = parsed
        if not check_hash(header):
            return 2
        captures.append(parsed)

    header, rows, anchors = captures[0]

    # A repeat run must produce identical checksums. If it does not, the
    # engine is not deterministic under these conditions and the numbers say
    # less than they appear to -- so it goes in the file, loudly.
    drift = ""
    for j, (_, other, _) in enumerate(captures[1:], start=2):
        a, b = by_name(rows), by_name(other)
        bad = [n for n in a
               if n in b and a[n]["checksum"] != b[n]["checksum"]]
        if bad:
            drift += ("Run 1 vs run %d differ on: %s. " % (j, ", ".join(bad)))
    if drift:
        print("WARNING: %s" % drift, file=sys.stderr)

    base = write_results(args.out_dir, header, rows, anchors, drift)
    print("# wrote %s.md and %s.csv" % (base, base), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
