import serial
import time
import sys

port = 'COM13'
baudrate = 115200
duration = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 300 # 5 minutes default
output_file = 'log_stage12_wifi_ps_none.txt'

print(f"Connecting to {port} at {baudrate} baud...")
try:
    ser = serial.Serial(port, baudrate, timeout=1)
except Exception as e:
    print(f"Failed to open port {port}: {e}")
    sys.exit(1)

reset = '--reset' in sys.argv
if reset:
    print("Resetting ESP32-S3...")
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    ser.setDTR(False)
    time.sleep(0.05)

ser.reset_input_buffer()

print(f"Recording log for {duration} seconds into {output_file}...")
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
