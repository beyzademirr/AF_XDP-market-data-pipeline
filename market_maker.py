import socket
import struct
import time

# Target the listening end of the veth cable (veth0)
UDP_IP = "10.200.1.1"
UDP_PORT = 1234

# Source IP should be the other end (veth1)
SRC_IP = "10.200.1.2"

print("=== Starting Market Maker Firehose ===")
print(f"Targeting IP: {UDP_IP} on Port: {UDP_PORT}")

# Create and bind UDP socket to veth1 IP
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((SRC_IP, 0))

# Fire 100 Toy ITCH packets
for i in range(1, 101):
    msg_type = b"A" if i % 2 == 0 else b"T"
    ts_ns = time.time_ns()
    price = 10000 + i
    qty = 100 + i

    # Toy ITCH: char type, uint64 ts_ns, uint32 price, uint32 qty (big-endian)
    payload = struct.pack("!cQII", msg_type, ts_ns, price, qty)

    # Send to veth0
    sock.sendto(payload, (UDP_IP, UDP_PORT))

    print(f"Sent: {msg_type.decode()} | ts_ns: {ts_ns} | price: {price} | qty: {qty}")
    time.sleep(0.1)

print("Firehose empty. Transmission complete.")