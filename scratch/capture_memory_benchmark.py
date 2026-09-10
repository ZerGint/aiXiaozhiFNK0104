import serial
import time
import sys

def main():
    print("=== CAPTURING HARDWARE MEMORY BENCHMARK (90 seconds) ===")
    try:
        s = serial.Serial('COM13', 115200, timeout=0.2)
        # Issue hard reset via RTS/DTR
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.1)
        s.setDTR(True)
        s.setRTS(False)
        time.sleep(0.1)
        s.setDTR(False)

        start = time.time()
        log_records = []
        while time.time() - start < 90:
            line = s.readline()
            if line:
                text = line.decode('utf-8', errors='ignore').rstrip()
                if 'UI_MEM_' in text or 'RAM: [BOOT]' in text:
                    timestamp = time.time() - start
                    log_records.append((timestamp, text))
                    print(f"[{timestamp:6.2f}s] {text}")
                    sys.stdout.flush()
        s.close()
        print(f"\n=== CAPTURE COMPLETE. Total records: {len(log_records)} ===")
    except Exception as e:
        print(f"Error reading COM13: {e}")

if __name__ == '__main__':
    main()
