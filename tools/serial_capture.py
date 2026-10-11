"""Capture serial output for a few seconds — verify button logs only."""
import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM16"
secs = int(sys.argv[2]) if len(sys.argv) > 2 else 5

s = serial.Serial(port, 115200, timeout=1)
t0 = time.time()
while time.time() - t0 < secs:
    line = s.readline().decode("utf-8", "ignore").strip()
    if line:
        print(line)
s.close()