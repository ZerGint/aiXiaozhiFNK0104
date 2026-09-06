import serial
import time
import sys

port = 'COM13'
baudrate = 115200
duration = 300  # 5 minutes
output_file = 'log_5min_stage7.txt'

print(f"Connecting to {port} at {baudrate} baud...")
try:
    ser = serial.Serial(port, baudrate, timeout=1)
except Exception as e:
    print(f"Failed to open port {port}: {e}")
    sys.exit(1)

# Do not reset by default so if user started radio, it keeps running, but if needed can reset.
# We will do a mild DTR/RTS flush
ser.reset_input_buffer()

print(f"Recording log for {duration} seconds (5 minutes) into {output_file}...")
start_time = time.time()
bytes_written = 0

with open(output_file, 'w', encoding='utf-8', buffering=1) as f:
    while time.time() - start_time < duration:
        data = ser.read(4096)
        if data:
            text = data.decode('utf-8', errors='replace')
            f.write(text)
            f.flush()
            bytes_written += len(text)
        time.sleep(0.01)

ser.close()
print(f"Finished! Recorded {bytes_written} characters into {output_file}.")
