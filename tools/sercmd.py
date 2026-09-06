#!/usr/bin/env python3
"""Non-resetting serial driver for hughes_bridge (ESP32-S3 USB-CDC).

The S3's USB-CDC RESETS the chip whenever DTR/RTS toggle on port open/close, so a
naive monitor reboots the device and looks exactly like a crash (see LEDGER.md gotchas;
a real crash shows reset=PANIC/TASK_WDT/BROWNOUT in `dmesg`, a tooling reboot shows
reset=USB). This opens with DTR=RTS=False (set BEFORE open, kept low before close) so
the board is left running.

Ports: the production bridge enumerates as COM8, the demo/test board as COM11 (one COM
holder at a time -- a monitor and an upload can't coexist).

Usage: python sercmd.py COM8 "<cmd>:<wait_s>" "<cmd>:<wait_s>" ...
       an empty cmd (":3") just reads for wait_s seconds.
Examples: python sercmd.py COM8 "status:2" "dmesg:3"
          python sercmd.py COM11 "heap:1" "version:1"
"""
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM8"
steps = sys.argv[2:] or ["status:2"]

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 0.1
ser.dtr = False          # applied on open() -> no reset
ser.rts = False
ser.open()
time.sleep(0.3)

def drain(sec):
    end = time.time() + sec
    while time.time() < end:
        data = ser.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()

try:
    drain(0.5)                                   # flush anything already queued
    for step in steps:
        cmd, _, wait = step.rpartition(":")
        wait = float(wait) if wait else 2.0
        if cmd:
            sys.stdout.write(f"\n>>> {cmd}\n"); sys.stdout.flush()
            ser.write((cmd + "\n").encode())
        drain(wait)
finally:
    ser.dtr = False; ser.rts = False             # keep low across close -> no reset
    ser.close()
