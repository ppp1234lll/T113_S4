import socket
import sys

s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('0.0.0.0', 16098))
s.listen(2)
print('listening 16098', flush=True)
while True:
    c, a = s.accept()
    print('accept %s' % (a,), flush=True)
    while True:
        d = c.recv(4096)
        if not d:
            break
        sys.stdout.write(d.decode('utf-8', 'replace'))
        sys.stdout.flush()
    c.close()
    print('--- conn closed ---', flush=True)
