import serial
import time

try:
    ser = serial.Serial('COM13', 115200)
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    ser.setDTR(False)
    ser.close()
    print("Reset signal sent to COM13")
except Exception as e:
    print(f"Error sending reset: {e}")
