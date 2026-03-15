# backend_server.py
import socket

PORT = 6789# change to 9002 or 9003 for other servers

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.bind(('0.0.0.0', PORT))
s.listen()

print(f"Backend server listening on port {PORT}")

while True:
    conn, addr = s.accept()
    print(f"Connection from {addr}")
    conn.sendall(f"Hello from backend {PORT}\n".encode())
    try:
        while True:
            data = conn.recv(1024)
            if not data:
                break
            conn.sendall(f"Backend {PORT} received: ".encode() + data)
    except:
        pass
    conn.close()
