import serial
import sys
import time

if sys.stdout and hasattr(sys.stdout, 'reconfigure'):
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='ignore')
    except Exception:
        pass

log_file_path = "log_favorites_presentation.txt"

print(f"Starting serial logger on COM13 -> {log_file_path}", flush=True)

try:
    ser = serial.Serial("COM13", 115200, timeout=1)
    with open(log_file_path, "a", encoding="utf-8", errors="ignore") as f:
        f.write(f"\n--- LOGGING STARTED AT {time.strftime('%Y-%m-%d %H:%M:%S')} ---\n")
        f.flush()
        while True:
            line = ser.readline()
            if line:
                decoded = line.decode("utf-8", errors="ignore")
                try:
                    sys.stdout.write(decoded)
                    sys.stdout.flush()
                except Exception:
                    pass
                f.write(decoded)
                f.flush()
except Exception as e:
    print(f"Serial logger stopped with error: {e}", flush=True)
