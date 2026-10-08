import socket
import sys
import time

HANDSHAKE = b"CLIENT"


def main():
    port, seconds = int(sys.argv[1]), float(sys.argv[2])
    deadline = time.time() + seconds
    while True:
        try:
            connection = socket.create_connection(("127.0.0.1", port), timeout=1)
            break
        except OSError:
            if time.time() > deadline:
                raise SystemExit("nothing listening on port %d" % port)
            time.sleep(0.2)
    data = b""
    while time.time() < deadline and HANDSHAKE not in data:
        try:
            chunk = connection.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            break
        data += chunk
    if HANDSHAKE not in data:
        raise SystemExit("no %s from the device, got %r" % (HANDSHAKE.decode(), data[:60]))


main()
