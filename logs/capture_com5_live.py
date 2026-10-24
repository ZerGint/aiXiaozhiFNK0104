import serial, time, sys
port='COM5'; baud=115200; path=sys.argv[1]
ser=serial.Serial(port, baud, timeout=1)
with open(path,'a',encoding='utf-8',errors='replace',buffering=1) as f:
    f.write('--- LIVE CAPTURE START '+time.strftime('%Y-%m-%d %H:%M:%S')+' ---\n')
    while True:
        data=ser.readline()
        if data:
            f.write(time.strftime('%Y-%m-%d %H:%M:%S ') + data.decode('utf-8',errors='replace'))
