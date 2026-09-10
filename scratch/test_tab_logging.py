import serial
import time
import sys

def main():
    print("=== MONITORING COM13 FOR UI_MEM_ MARKERS ===")
    try:
        s = serial.Serial('COM13', 115200, timeout=0.2)
        # Soft reboot to catch boot markers if needed, or listen continuously
        # s.setDTR(False); s.setRTS(True); time.sleep(0.1); s.setDTR(True); s.setRTS(False); time.sleep(0.1); s.setDTR(False)
        start = time.time()
        count = 0
        while time.time() - start < 120:
            line = s.readline()
            if line:
                text = line.decode('utf-8', errors='ignore').rstrip()
                if 'UI_MEM_' in text:
                    count += 1
                    print(f"[{time.time()-start:6.1f}s] #{count:02d}: {text}")
                    sys.stdout.flush()
        s.close()
        print(f"=== DONE. TOTAL UI_MEM_ LOGS CAPTURED: {count} ===")
    except Exception as e:
        print(f"Error: {e}")

if __name__ == '__main__':
    main()
