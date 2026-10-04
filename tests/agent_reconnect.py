import os
import socket
import struct
import sys
import time

path, count = sys.argv[1], int(sys.argv[2])


def string(text):
    return struct.pack('<H', len(text)) + text.encode('utf-16le')


def recv_exact(connection, length):
    data = b''
    while len(data) < length:
        chunk = connection.recv(length - len(data))
        if not chunk:
            raise ConnectionError('emulator closed the connection')
        data += chunk
    return data


for attempt in range(100):
    if os.path.exists(path):
        break
    time.sleep(0.1)
failures = 0
for attempt in range(count):
    connection = socket.socket(socket.AF_UNIX)
    connection.settimeout(60)
    connection.connect(path)
    body = struct.pack('<HHII', 2, attempt + 1, 0, 1) + string('\\reconnect%d.txt' % attempt) + b'x' * 100
    try:
        connection.sendall(struct.pack('<I', len(body)) + body)
        length = struct.unpack('<I', recv_exact(connection, 4))[0]
        reply = recv_exact(connection, length)
        if struct.unpack_from('<HHI', reply) != (0x8002, attempt + 1, 0):
            failures += 1
            print('attempt', attempt, 'bad reply', reply.hex())
    except (ConnectionError, socket.timeout) as error:
        failures += 1
        print('attempt', attempt, error)
    connection.close()
print('failures', failures, 'of', count)
sys.exit(1 if failures else 0)
