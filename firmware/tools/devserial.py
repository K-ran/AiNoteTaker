#!/usr/bin/env python3
"""Non-interactive serial helper for the note taker (developer/factory use).

Capture the log for N seconds, optionally resetting first and sending console
commands at given times. Secrets can be read from environment variables so they
never appear in shell history:

  python tools/devserial.py --reset --seconds 40
  python tools/devserial.py --seconds 20 --cmd 0:status --cmd 5:upload
  NT_TOKEN=... python tools/devserial.py --seconds 5 --cmd '0:token $NT_TOKEN'
"""
import argparse, os, sys, time
import serial  # pyserial (bundled with the ESP-IDF Python env)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("NT_PORT", "/dev/cu.usbmodem1101"))
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--reset", action="store_true", help="pulse RTS to reboot the board first")
    ap.add_argument("--cmd", action="append", default=[], help="T:command, sent T seconds after start")
    ap.add_argument("--quiet-secrets", action="store_true", default=True)
    a = ap.parse_args()

    cmds = []
    for c in a.cmd:
        t, _, text = c.partition(":")
        cmds.append((float(t), os.path.expandvars(text)))
    cmds.sort()

    def connect():
        for _ in range(50):                      # the USB port vanishes briefly on every chip reset
            try:
                return serial.Serial(a.port, 115200, timeout=0.2)
            except serial.SerialException:
                time.sleep(0.2)
        raise SystemExit("port %s not found" % a.port)

    s = connect()
    if a.reset:
        s.dtr = False
        s.rts = True
        time.sleep(0.1)
        s.rts = False
    start = time.time()
    secrets = [v for k, v in os.environ.items() if k.startswith("NT_") and len(v) > 8]
    while time.time() - start < a.seconds:
        while cmds and time.time() - start >= cmds[0][0]:
            _, text = cmds.pop(0)
            try:
                s.write((text + "\n").encode())
            except (serial.SerialException, OSError):
                s = connect()
        try:
            data = s.read(4096)
        except (serial.SerialException, OSError):
            s.close()
            s = connect()
            continue
        if data:
            out = data.decode("utf-8", "replace")
            for v in secrets:  # never echo secrets into logs
                out = out.replace(v, "<redacted>")
            sys.stdout.write(out)
            sys.stdout.flush()
    s.close()


if __name__ == "__main__":
    main()
