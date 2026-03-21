import socket
import sys

port = 47998
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    s.bind(('0.0.0.0', port))
    print(f"Successfully bound to {port}")
except Exception as e:
    print(f"Failed to bind to {port}: {e}")
finally:
    s.close()
