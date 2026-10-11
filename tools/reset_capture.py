"""Reset ESP32 via RTS and capture boot log + first 3s of button output."""
import serial, time, sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM16"

s = serial.Serial(port, 115200, timeout=1)
# Toggle RTS to reset the chip
s.setDTR(False)
s.setRTS(True)
time.sleep(0.1)
s.setRTS(False)
s.setDTR(True)
time.sleep(0.1)

# Capture for 4 seconds after reset
t0 = time.time()
while time.time() - t0 < 4:
    line = s.readline().decode("utf-8", "ignore").strip()
    if line:
        print(line)
s.close()