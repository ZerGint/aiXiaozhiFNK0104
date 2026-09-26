import serial
import time
import sys

port = 'COM13'
baudrate = 115200
duration = int(sys.argv[1]) if len(sys.argv) > 1 else 120 # seconds

print(f"Connecting to {port} at {baudrate} baud...")
try:
    ser = serial.Serial(port, baudrate, timeout=1)
except Exception as e:
    print(f"Failed to open port {port}: {e}")
    sys.exit(1)

# Reset ESP32-S3 via RTS/DTR toggle
print("Resetting ESP32-S3...")
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.1)
ser.setRTS(False)
ser.setDTR(False)
time.sleep(0.05)

ser.reset_input_buffer()

print(f"Recording log for {duration} seconds...")
start_time = time.time()
chunks = []

while time.time() - start_time < duration:
    data = ser.read(4096)
    if data:
        chunks.append(data)
    time.sleep(0.01)

ser.close()

full_text = b''.join(chunks).decode('utf-8', errors='replace')
output_file = 'minute_log.txt'

with open(output_file, 'w', encoding='utf-8') as f:
    f.write(full_text)

print(f"Finished! Recorded {len(full_text)} characters into {output_file}.")
