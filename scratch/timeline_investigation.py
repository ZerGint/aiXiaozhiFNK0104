import serial
import time
import sys
import re

def main():
    print("=== STARTING STEP-BY-STEP TIMELINE INVESTIGATION ON COM13 ===")
    try:
        s = serial.Serial('COM13', 115200, timeout=0.2)
        # Soft reset via RTS/DTR
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.1)
        s.setDTR(True)
        s.setRTS(False)
        time.sleep(0.1)
        s.setDTR(False)

        start = time.time()
        count = 0
        log_file = open("scratch/timeline_log.txt", "w", encoding="utf-8")
        
        while time.time() - start < 180:
            line = s.readline()
            if line:
                text = line.decode('utf-8', errors='ignore').rstrip()
                t_rel = time.time() - start
                log_file.write(f"[{t_rel:6.2f}s] {text}\n")
                log_file.flush()
                
                # Print key diagnostic lines to console
                if any(k in text for k in ['RAM:', 'UI_MEM_', 'ToggleQuickSettings', 'State:', 'Audio', 'StorageManager', 'WiFi', 'WebSocket', 'Radio', 'Player']):
                    count += 1
                    print(f"[{t_rel:6.1f}s] #{count:02d}: {text}")
                    sys.stdout.flush()
                    
        s.close()
        log_file.close()
        print(f"\n=== TIMELINE LOG COMPLETE. Saved to scratch/timeline_log.txt (Total logs: {count}) ===")
    except Exception as e:
        print(f"Error: {e}")

if __name__ == '__main__':
    main()
