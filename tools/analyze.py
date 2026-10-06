#!/usr/bin/env python3
"""Analyse a capture: per-move travel, sensor agreement, and the optical question.

    python3 tools/analyze.py run1.csv            # numbers only (stdlib)
    python3 tools/analyze.py run1.csv --plot     # also write run1.png (matplotlib)

The headline output is Shutter and Frame_Avg AGAINST SPEED. That is the test:
if the chip lengthens its exposure as the target moves faster, it is short of
light, and a longer exposure blurs more per frame -- which is how an optical
problem turns into lost counts specifically at speed. Flat readings across the
speed range point away from optics and toward standoff, scale or mechanics.

Only the plot needs a third-party package; the numbers are stdlib.
"""
import argparse, csv, statistics as st, sys

IDLE_GAP_MS = 300      # no displacement for this long ends a move
MIN_MOVE_CNT = 20      # ignore twitches


def load(path):
    """Rows, scan rows, and the '#' lines carrying ruler checks and test reports."""
    rows, notes, scan = [], [], []
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if line.startswith("#"):
                notes.append(line)
                continue
            if line.startswith("P,"):
                try:
                    scan.append([float(x) for x in line.split(",")[1:]])
                except ValueError:
                    pass
                continue
            if line.startswith("ms,") or "," not in line:
                continue
            parts = line.split(",")
            try:
                rows.append([float(x) for x in parts])
            except ValueError:
                continue
    return rows, notes, scan


def segment(rows, xcol):
    """Split into moves on displacement gaps. Returns (start, end) row indices."""
    moves, start, last_change = [], None, None
    for i in range(1, len(rows)):
        moved = rows[i][xcol] != rows[i - 1][xcol]
        if moved:
            if start is None:
                start = i - 1
            last_change = i
        elif start is not None and last_change is not None:
            if rows[i][0] - rows[last_change][0] > IDLE_GAP_MS:
                if abs(rows[last_change][xcol] - rows[start][xcol]) >= MIN_MOVE_CNT:
                    moves.append((start, last_change))
                start = last_change = None
    if start is not None and last_change is not None:
        if abs(rows[last_change][xcol] - rows[start][xcol]) >= MIN_MOVE_CNT:
            moves.append((start, last_change))
    return moves


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--counts-per-mm", type=float, default=50.2)
    ap.add_argument("--plot", action="store_true")
    args = ap.parse_args()

    rows, notes, scan = load(args.csv)
    if not rows and not scan:
        sys.exit("no data found -- was the board printing CSV?")

    # ms,s1_dx,s1_dy,s1_x,s1_y,s1_x_mm,s1_shutter,s1_frame_avg, s2... (8 cols/sensor)
    S1X, S1SH, S1FA = 3, 6, 7
    S2X, S2SH, S2FA = 10, 13, 14
    two = bool(rows) and len(rows[0]) > S2FA
    cpm = args.counts_per_mm

    print(f"=== {args.csv} ===")
    if rows:
        print(f"rows={len(rows)}  duration={(rows[-1][0]-rows[0][0])/1000:.1f}s  "
              f"sensors={'2' if two else '1'}  counts/mm={cpm}")
    else:
        print(f"scan-only log  samples={len(scan)}  counts/mm={cpm}")

    moves = segment(rows, S1X) if rows else []
    if moves:
        print(f"\n--- moves (n={len(moves)}) ---")
        print(f"{'#':>3} {'dist_s1':>9} {'dist_s2':>9} {'s2/s1':>7} "
              f"{'speed':>9} {'shutter':>8} {'frame_avg':>10}")
        speeds, shutters, favgs = [], [], []
        for n, (a, b) in enumerate(moves, 1):
            sec = (rows[b][0] - rows[a][0]) / 1000.0
            d1 = (rows[b][S1X] - rows[a][S1X]) / cpm
            d2 = (rows[b][S2X] - rows[a][S2X]) / cpm if two else float("nan")
            spd = abs(d1) / sec if sec > 0 else 0.0
            seg = rows[a:b + 1]
            sh = st.mean(r[S1SH] for r in seg)
            fa = st.mean(r[S1FA] for r in seg)
            speeds.append(spd); shutters.append(sh); favgs.append(fa)
            d2s = f"{d2:>8.3f}m" if two else f"{'-':>9}"
            rts = f"{d2/d1:>7.4f}" if (two and d1) else f"{'-':>7}"
            print(f"{n:>3} {d1:>8.3f}m {d2s} {rts} "
                  f"{spd:>7.2f}mm/s {sh:>8.1f} {fa:>10.1f}")

        if two:
            rs = [abs((rows[b][S2X]-rows[a][S2X]) / (rows[b][S1X]-rows[a][S1X]))
                  for a, b in moves if rows[b][S1X] != rows[a][S1X]]
            if rs:
                print(f"\nsensor agreement s2/s1: mean={st.mean(rs):.4f}"
                      + (f" sd={st.pstdev(rs):.4f}" if len(rs) > 1 else ""))
                print("  near 1.0 => both sensors see the same thing, so a single")
                print("  faulty part is not the explanation.")

        # the hypothesis test
        if len(speeds) >= 2 and max(speeds) > min(speeds) * 1.5:
            lo = min(range(len(speeds)), key=lambda i: speeds[i])
            hi = max(range(len(speeds)), key=lambda i: speeds[i])
            dsh = (shutters[hi]-shutters[lo])/shutters[lo]*100 if shutters[lo] else 0
            dfa = (favgs[hi]-favgs[lo])/favgs[lo]*100 if favgs[lo] else 0
            print(f"\n--- optical vs speed ---")
            print(f"slowest {speeds[lo]:.2f}mm/s: shutter={shutters[lo]:.1f} frame_avg={favgs[lo]:.1f}")
            print(f"fastest {speeds[hi]:.2f}mm/s: shutter={shutters[hi]:.1f} frame_avg={favgs[hi]:.1f}")
            print(f"change: shutter {dsh:+.1f}%   frame_avg {dfa:+.1f}%")
            if dsh > 25:
                print("  => exposure lengthens with speed: the chip is short of light.")
                print("     Consistent with aperture/illumination causing loss at speed.")
            elif abs(dsh) < 10 and 25 < favgs[hi] < 230:
                print("  => stable and mid-range: illumination is NOT the limit here.")
                print("     Look to standoff variation, scale calibration, or mechanics.")
            elif favgs[hi] > 230:
                print("  => near saturation: ambient light. Shield before enlarging"); print("     the aperture -- a bigger hole makes this worse.")
            else:
                print("  => mixed; report the table above rather than a single verdict.")
        else:
            print("\n--- optical vs speed ---")
            print("  need moves at two clearly different speeds (>1.5x apart).")
            print("  Run a slow pass and a fast pass, or use the sketch's 't' test.")
    elif rows:
        print("\nno moves detected -- did the target actually move?")

    checks = [n for n in notes if "RULER CHECK" in n or "ratio=" in n or "implied counts/mm" in n]
    if checks:
        print("\n--- ruler checks from the log ---")
        for n in checks:
            print(" ", n.lstrip("# "))

    if scan:
        # P,s1_x,s1_y,s1_sh,s1_fa[,s2_x,s2_y,s2_sh,s2_fa]
        pos = [r[0] / cpm for r in scan]
        sh  = [r[2] for r in scan]
        fa  = [r[3] for r in scan]
        span = max(pos) - min(pos)
        print(f"\n--- surface scan ({len(scan)} samples over {span:.2f} mm) ---")
        if span > 0:
            print(f"resolution: {span/len(scan)*1000:.0f} um/sample")
        print(f"frame_avg  min={min(fa):.0f} mean={st.mean(fa):.1f} max={max(fa):.0f}"
              f"  spread={max(fa)-min(fa):.0f}")
        print(f"shutter    min={min(sh):.0f} mean={st.mean(sh):.1f} max={max(sh):.0f}"
              f"  spread={max(sh)-min(sh):.0f}")
        rng = (max(fa) - min(fa)) / st.mean(fa) * 100 if st.mean(fa) else 0
        if rng < 10:
            print("  => brightness is uniform along the rod. The surface is not the")
            print("     variable; look at geometry (aperture, standoff) instead.")
        else:
            worst = min(range(len(fa)), key=lambda i: fa[i])
            print(f"  => brightness varies {rng:.0f}% along the stroke, darkest near")
            print(f"     {pos[worst]:.1f} mm. A localised dip points at the SURFACE")
            print("     (contamination, finish, a defect) rather than the optics.")

    if args.plot:
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
        except ImportError:
            sys.exit("--plot needs matplotlib:  pip install matplotlib")
        if rows:
            t = [(r[0] - rows[0][0]) / 1000.0 for r in rows]
            fig, ax = plt.subplots(3, 1, figsize=(10, 9), sharex=True)
            ax[0].plot(t, [r[S1X]/cpm for r in rows], label="sensor 1")
            if two:
                ax[0].plot(t, [r[S2X]/cpm for r in rows], label="sensor 2")
            ax[0].set_ylabel("travel (mm)"); ax[0].legend(); ax[0].grid(alpha=.3)
            ax[1].plot(t, [r[S1SH] for r in rows], label="s1 shutter")
            if two: ax[1].plot(t, [r[S2SH] for r in rows], label="s2 shutter")
            ax[1].set_ylabel("shutter (exposure)"); ax[1].legend(); ax[1].grid(alpha=.3)
            ax[2].plot(t, [r[S1FA] for r in rows], label="s1 frame_avg")
            if two: ax[2].plot(t, [r[S2FA] for r in rows], label="s2 frame_avg")
            ax[2].set_ylabel("frame_avg (brightness)"); ax[2].set_xlabel("time (s)")
            ax[2].legend(); ax[2].grid(alpha=.3)
        if scan:
            fig2, bx = plt.subplots(2, 1, figsize=(10, 6), sharex=True)
            pos = [r[0] / cpm for r in scan]
            bx[0].plot(pos, [r[3] for r in scan], lw=.8, label="s1 frame_avg")
            if len(scan[0]) >= 8:
                bx[0].plot(pos, [r[7] for r in scan], lw=.8, label="s2 frame_avg")
            bx[0].set_ylabel("frame_avg"); bx[0].legend(); bx[0].grid(alpha=.3)
            bx[1].plot(pos, [r[2] for r in scan], lw=.8, label="s1 shutter")
            if len(scan[0]) >= 8:
                bx[1].plot(pos, [r[6] for r in scan], lw=.8, label="s2 shutter")
            bx[1].set_ylabel("shutter"); bx[1].set_xlabel("position along rod (mm)")
            bx[1].legend(); bx[1].grid(alpha=.3)
            fig2.suptitle("Surface scan: what the sensor sees vs position", y=.98)
            fig2.tight_layout()
            so = args.csv.rsplit(".", 1)[0] + "_scan.png"
            fig2.savefig(so, dpi=130); print(f"scan plot -> {so}")

        if rows:
            out = args.csv.rsplit(".", 1)[0] + ".png"
            fig.suptitle("PAT9125: travel, exposure and brightness", y=.995)
            fig.tight_layout(); fig.savefig(out, dpi=130)
            print(f"plot -> {out}")


if __name__ == "__main__":
    main()
