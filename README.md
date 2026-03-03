# AF_XDP Market Data Pipeline

Kernel-bypass UDP ingestion for market data on Linux (works on WSL2).

## What it does

- XDP program filters UDP port 1234 at the NIC driver level
- AF_XDP socket receives packets directly into user-space memory
- Zero-copy path from kernel to a flat-array order book
- No malloc/new in the hot loop

## Files

- `xdp_filter.c` - eBPF program, parses eth/ip/udp, redirects port 1234
- `main.cpp` - AF_XDP socket setup + packet processing loop
- `types.hpp` - ITCH message struct + price-indexed order book
- `market_maker.py` - test packet generator

## Build

```bash
# BPF program
clang -O2 -g -target bpf -c xdp_filter.c -o xdp_filter.o

# User-space app
g++ -O3 -o market_data main.cpp -lxdp -lbpf -lpthread
```

Needs libxdp and libbpf installed.

## Run

WSL2 requires a custom kernel with `CONFIG_XDP_SOCKETS=y`.

```bash
# mount bpffs (once per boot)
sudo mount -t bpf bpf /sys/fs/bpf

# create test interface
sudo ip link add veth0 type veth peer name veth1
sudo ip netns add mdns
sudo ip link set veth1 netns mdns
sudo ip addr add 10.200.1.1/24 dev veth0
sudo ip netns exec mdns ip addr add 10.200.1.2/24 dev veth1
sudo ip link set veth0 up
sudo ip netns exec mdns ip link set veth1 up

# run
sudo ./market_data veth0 xdp_filter.o

# send test packets (another terminal)
sudo ip netns exec mdns python3 market_maker.py
```

## Message format

17 bytes, big-endian:

| Field | Type | Size |
|-------|------|------|
| type | char | 1 | 
| ts_ns | uint64 | 8 |
| price | uint32 | 4 |
| qty | uint32 | 4 |

`type` is `A` (add) or `T` (trade).

## Notes

- Uses SKB mode + copy (zero-copy not supported on virtual NICs)
- Order book uses price as array index, O(1) updates
- Poll timeout is 100ms, remove for busy-poll lowest latency
