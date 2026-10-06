#!/usr/bin/env python3
"""Record the sketch's output to a file while you drive it interactively.

Everything the board prints is logged verbatim -- the CSV rows and the '#' lines
(ruler checks, optical-test reports), so the log is self-describing. Anything you
type is forwarded, so 'z', 'r 40.15' and 't' work normally while recording.

    python3 tools/capture.py                 # auto-detect the port
    python3 tools/capture.py --port /dev/ttyACM0 --out run1.csv

Only dependency is pyserial:  pip install pyserial
"""
import argparse, sys, threading, time
from datetime import datetime

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("needs pyserial:  pip install pyserial")


def find_port():
    """Prefer a Seeed XIAO (VID 0x2886); otherwise the sole USB serial device."""
    ports = list(list_ports.comports())
    for p in ports:
        if (p.vid, p.pid) == (0x2886, 0x802F):
            return p.device
    cdc = [p for p in ports if p.vid is not None]
    if len(cdc) == 1:
        return cdc[0].device
    if not cdc:
        sys.exit("no USB serial device found -- is the board plugged in?")
    sys.exit("several serial devices; pick one with --port:\n  " +
             "\n  ".join(f"{p.device}  {p.description}" for p in cdc))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out")
    args = ap.parse_args()

    port = args.port or find_port()
    out = args.out or f"pat9125_{datetime.now():%Y%m%d_%H%M%S}.csv"
    ser = serial.Serial(port, args.baud, timeout=0.2)
    time.sleep(0.3)

    print(f"# port {port} -> {out}")
    print("# type commands normally (z, r <mm>, t, ?). Ctrl-C to stop.\n")

    stop = threading.Event()

    def pump_stdin():
        # Forward what you type. Line-buffered, which is what the sketch expects.
        try:
            for line in sys.stdin:
                if stop.is_set():
                    return
                ser.write(line.rstrip("\n").encode() + b"\n")
                ser.flush()
        except Exception:
            pass

    threading.Thread(target=pump_stdin, daemon=True).start()

    rows = 0
    with open(out, "w", encoding="utf-8") as f:
        f.write(f"# captured {datetime.now().isoformat(timespec='seconds')} from {port}\n")
        try:
            while True:
                line = ser.readline().decode("utf-8", "replace").rstrip()
                if not line:
                    continue
                f.write(line + "\n")
                f.flush()                 # survive a Ctrl-C mid-run
                print(line)
                if not line.startswith("#") and "," in line:
                    rows += 1
        except KeyboardInterrupt:
            pass
        finally:
            stop.set()
            ser.close()
    print(f"\n# {rows} data rows -> {out}")
    print(f"# analyse with:  python3 tools/analyze.py {out}")


if __name__ == "__main__":
    main()
