import serial
import time
import sys

def main():
    print("=== CAPTURING FULL HARDWARE VALIDATION RUN (150 seconds) ===")
    try:
        s = serial.Serial('COM13', 115200, timeout=0.2)
        # Reset board via DTR/RTS
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.1)
        s.setDTR(True)
        s.setRTS(False)
        time.sleep(0.1)
        s.setDTR(False)

        start = time.time()
        record_count = 0
        while time.time() - start < 150:
            line = s.readline()
            if line:
                text = line.decode('utf-8', errors='ignore').rstrip()
                if 'UI_MEM_' in text or 'RAM:' in text or 'ToggleQuickSettings' in text:
                    record_count += 1
                    print(f"[{time.time()-start:6.1f}s] #{record_count:02d}: {text}")
                    sys.stdout.flush()
        s.close()
        print(f"\n=== CAPTURE COMPLETE. Total records: {record_count} ===")
    except Exception as e:
        print(f"Error: {e}")

if __name__ == '__main__':
    main()
