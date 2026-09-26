import serial
import time
import sys

port = 'COM13'
baudrate = 115200
output_file = 'log_redirect_test.txt'

print(f"Connecting to {port} at {baudrate} baud...")
try:
    ser = serial.Serial(port, baudrate, timeout=1)
except Exception as e:
    print(f"Failed to open port {port}: {e}")
    sys.exit(1)

print(f"Recording log into {output_file}...")

with open(output_file, 'w', encoding='utf-8', buffering=1) as f:
    f.write(f"--- LOG SESSION STARTED AT {time.ctime()} ---\n")
    while True:
        try:
            line = ser.readline()
            if line:
                decoded = line.decode('utf-8', errors='replace')
                sys.stdout.write(decoded)
                sys.stdout.flush()
                f.write(decoded)
                f.flush()
        except KeyboardInterrupt:
            break
        except Exception as e:
            time.sleep(0.1)

ser.close()
