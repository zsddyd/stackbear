#!/usr/bin/env python3
"""Read an ESP32 native USB-CDC console on Termux/Android.

Why this exists
---------------
On Android there is no serial tty: `/dev/ttyACM*` and `/dev/bus/usb` are not
exposed to apps, so `minicom`, `picocom`, `pyserial` and `idf.py monitor` cannot
open the ESP32-C3 USB-Serial/JTAG console. The only way in is a raw USB device
descriptor handed out by `termux-usb`.

This tool wraps that descriptor with the same USB layer used by the local
`nrflash`/`espbridge` tooling (pyusb + libusb fd wrapping), claims the CDC data
interface, and streams the bulk-IN endpoint to stdout -- an equivalent of a
serial monitor for this environment.

It is a diagnostic helper, not part of the firmware build. Run it from the
repository root:

    python3 tools/usb_console.py                # auto-detect, ask permission
    python3 tools/usb_console.py --seconds 60   # stop after 60 s
    python3 tools/usb_console.py --device /dev/bus/usb/002/007
    python3 tools/usb_console.py --save build/console.log

Android shows a USB permission dialog the first time; tap Allow.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
import time

try:
    import espbridge as eb
    import usb.core
except ImportError as exc:  # pragma: no cover - environment guidance
    sys.stderr.write(
        f"missing dependency: {exc}\n"
        "Install the Termux USB helpers first:  pip install pyusb  (and the\n"
        "'espbridge'/'nrflash' packages this project's tooling uses)\n"
    )
    raise SystemExit(2)

READ_SIZE = 4096
READ_TIMEOUT_MS = 500


def _timestamp() -> str:
    return time.strftime("[%H:%M:%S] ")


def stream(dev, ep_in: int, seconds: float, save_path: str | None, prefix_ts: bool) -> int:
    """Read the bulk-IN endpoint until the deadline or Ctrl-C."""
    out = None
    if save_path:
        os.makedirs(os.path.dirname(save_path) or ".", exist_ok=True)
        out = open(save_path, "ab", buffering=0)

    deadline = time.time() + seconds if seconds > 0 else None
    at_line_start = True
    total = 0
    try:
        while deadline is None or time.time() < deadline:
            try:
                data = dev.read(ep_in, READ_SIZE, timeout=READ_TIMEOUT_MS)
            except usb.core.USBTimeoutError:
                continue
            except usb.core.USBError as exc:
                # Device reset/re-enumerated (e.g. during a crash reboot): report
                # and keep trying -- the console re-appears after the reboot.
                sys.stderr.write(f"\n[usb] read error: {exc}\n")
                time.sleep(1.0)
                continue
            if not data:
                continue
            chunk = bytes(data)
            total += len(chunk)
            if out is not None:
                out.write(chunk)
            text = chunk.decode("utf-8", "replace")
            if prefix_ts:
                lines = text.splitlines(keepends=True)
                for line in lines:
                    if at_line_start:
                        sys.stdout.write(_timestamp())
                    sys.stdout.write(line)
                    at_line_start = line.endswith("\n")
            else:
                sys.stdout.write(text)
            sys.stdout.flush()
            at_line_start = text.endswith("\n")
    except KeyboardInterrupt:
        sys.stdout.write("\n[usb] stopped\n")
    finally:
        if out is not None:
            out.close()
    return total


def child_main(args: argparse.Namespace) -> int:
    """Re-exec'd process: TERMUX_USB_FD (or a direct fd) is available."""
    fd_env = os.environ.get("TERMUX_USB_FD")
    if fd_env is None:
        sys.stderr.write("no TERMUX_USB_FD in environment\n")
        return 2
    dev = eb.wrap_fd(int(fd_env))
    ep_in, ep_out, iface = eb.get_cdc_endpoints(dev)
    eb.claim_device(dev, iface, fd_wrapped=True)
    eb.reset_endpoint_toggles(dev, ep_in, ep_out)
    sys.stderr.write(
        f"[usb] {eb.describe_device(dev)}  interface={iface}  ep_in=0x{ep_in:02X}\n"
    )
    total = stream(dev, ep_in, args.seconds, args.save, not args.raw)
    sys.stderr.write(f"\n[usb] {total} bytes read\n")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Read an ESP32 native USB-CDC console on Termux/Android."
    )
    parser.add_argument("--device", help="USB device path, e.g. /dev/bus/usb/002/007")
    parser.add_argument("--seconds", type=float, default=0, help="stop after N seconds (0 = forever)")
    parser.add_argument("--save", help="also append the raw stream to this file")
    parser.add_argument("--raw", action="store_true", help="no timestamp prefix")
    # Internal: the re-exec'd child receives the descriptor via TERMUX_USB_FD.
    if os.environ.get("TERMUX_USB_FD"):
        return child_main(parser.parse_args())

    args = parser.parse_args()
    backend = eb.detect_backend()
    if backend != "termux":
        sys.stderr.write(
            f"[usb] backend={backend}: this tool targets Termux's termux-usb fd path.\n"
            "      On a rooted/Nethunter host use a normal serial monitor instead.\n"
        )
        return 2

    # NB: espbridge's open_usb_device() shells out to the `termux-api` CLI, which
    # is NOT part of the minimal install here (only `termux-usb` is present).
    # Call termux-usb ourselves: -r shows the permission dialog, -E exports the
    # descriptor as TERMUX_USB_FD to the re-exec'd command (-e).
    device_path = args.device or _auto_detect()
    passthrough = ["--seconds", str(args.seconds)]
    if args.raw:
        passthrough.append("--raw")
    if args.save:
        passthrough += ["--save", args.save]
    cmd = " ".join(
        [shlex.quote(sys.executable), shlex.quote(os.path.abspath(__file__))]
        + [shlex.quote(p) for p in passthrough]
    )
    sys.stderr.write(f"[usb] opening {device_path} (tap Allow on the phone)\n")
    result = subprocess.run(
        ["termux-usb", "-r", "-E", "-e", cmd, device_path], check=False
    )
    return result.returncode


def _auto_detect() -> str:
    """Ask termux-usb for the device list and return the first path."""
    out = subprocess.run(
        ["termux-usb", "-l"], capture_output=True, text=True, check=True
    ).stdout.strip()
    try:
        devices = json.loads(out)
    except json.JSONDecodeError:
        devices = []
    if not devices:
        raise SystemExit("no USB device found - is the board plugged in via OTG?")
    return devices[0]


if __name__ == "__main__":
    raise SystemExit(main())
