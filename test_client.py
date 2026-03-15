import socket

HOST = '127.0.0.1'  # Load balancer address
PORT = 8181         # Load balancer port

# Create a TCP socket
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
    s.connect((HOST, PORT))
    print(f"Connected to load balancer at {HOST}:{PORT}")

    # Send a simple message
    message = "Hello Load Balancer!"
    s.sendall(message.encode())
    print(f"Sent: {message}")

    # Receive response from backend through load balancer
    data = s.recv(4096)
    print(f"Received: {data.decode()}")
