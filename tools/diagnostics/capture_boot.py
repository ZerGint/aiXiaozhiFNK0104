import serial, time

ser = serial.Serial('COM13', 115200, timeout=1)

# Reset ESP32-S3 via RTS/DTR toggle  
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.1)
ser.setRTS(False)
ser.setDTR(False)
time.sleep(0.05)

# Flush any stale data
ser.reset_input_buffer()

# Capture boot log for 20 seconds
start = time.time()
chunks = []
while time.time() - start < 20:
    data = ser.read(4096)
    if data:
        chunks.append(data)

ser.close()

full = b''.join(chunks).decode('utf-8', errors='replace')
with open('boot_log_full.txt', 'w', encoding='utf-8') as f:
    f.write(full)
print(f'Captured {len(full)} bytes')
