import socket
import struct
import time
import random

# Target the listening end of the veth cable (veth0)
UDP_IP = "10.200.1.1"
UDP_PORT = 1234

# Source IP should be the other end (veth1)
SRC_IP = "10.200.1.2"

# 10 ticker symbols, each padded to exactly 8 bytes
SYMBOLS = [
    b"BTC/USDT",
    b"ETH/USDT",
    b"SOL/USDT",
    b"XRP/USDT",
    b"ADA/USDT",
    b"DOT/USDT",
    b"LINK/USD",
    b"AVAX/USD",
    b"MATIC/US",
    b"ATOM/USD",
]

print("=== Starting Market Maker Firehose ===")
print(f"Targeting IP: {UDP_IP} on Port: {UDP_PORT}")
print(f"Symbols: {len(SYMBOLS)}")

# Create and bind UDP socket to veth1 IP
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind((SRC_IP, 0))

# Fire 100 multi-symbol ITCH packets (25 bytes each)
for i in range(1, 101):
    msg_type = b"A" if i % 2 == 0 else b"T"
    symbol = random.choice(SYMBOLS)
    ts_ns = time.time_ns()
    price = 10000 + i
    qty = 100 + i

    # ITCH: char type (1) + char symbol[8] (8) + uint64 ts_ns (8) + uint32 price (4) + uint32 qty (4) = 25 bytes
    payload = struct.pack("!c8sQII", msg_type, symbol, ts_ns, price, qty)

    # Send to veth0
    sock.sendto(payload, (UDP_IP, UDP_PORT))

    print(f"Sent: {msg_type.decode()} | {symbol.decode()} | price: {price} | qty: {qty}")
    time.sleep(0.05)

print("Firehose empty. Transmission complete.")