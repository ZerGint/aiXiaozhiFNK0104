import serial
import time
import sys

# Configure stdout encoding to utf-8 safely on Windows
if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

port = 'COM13'
baud = 115200
logfile_path = 'serial_log_new_session.txt'

print(f"Starting continuous serial capture on {port} at {baud} baud -> {logfile_path}")

try:
    ser = serial.Serial(port, baud, timeout=1)
except Exception as e:
    print(f"Failed to open serial port {port}: {e}")
    sys.exit(1)

try:
    with open(logfile_path, 'a', encoding='utf-8', errors='replace') as logfile:
        logfile.write(f"\n--- CONTINUOUS LOGGING STARTED AT {time.strftime('%Y-%m-%d %H:%M:%S')} ---\n")
        logfile.flush()
        
        while True:
            line = ser.readline()
            if line:
                decoded = line.decode('utf-8', errors='replace')
                try:
                    sys.stdout.write(decoded)
                    sys.stdout.flush()
                except Exception:
                    pass
                logfile.write(decoded)
                logfile.flush()
except KeyboardInterrupt:
    print("\nLogging stopped by user request.")
finally:
    ser.close()
    print("Serial port closed.")
