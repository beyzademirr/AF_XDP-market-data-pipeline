#!/bin/bash
# Setup veth pair and network namespace for AF_XDP testing

set -e

echo "=== Setting up veth test environment ==="

# Mount bpffs if not mounted
if ! mountpoint -q /sys/fs/bpf; then
    echo "Mounting bpffs..."
    sudo mount -t bpf bpf /sys/fs/bpf
fi

# Clean up existing setup if present
if ip link show veth0 &>/dev/null; then
    echo "Removing existing veth0..."
    sudo ip link del veth0
fi

if ip netns list | grep -q mdns; then
    echo "Removing existing namespace mdns..."
    sudo ip netns del mdns
fi

# Create veth pair
echo "Creating veth pair..."
sudo ip link add veth0 type veth peer name veth1

# Create namespace and move veth1 into it
echo "Creating namespace mdns..."
sudo ip netns add mdns
sudo ip link set veth1 netns mdns

# Configure veth0 (root namespace)
echo "Configuring veth0..."
sudo ip addr add 10.200.1.1/24 dev veth0
sudo ip link set veth0 up

# Configure veth1 (inside namespace)
echo "Configuring veth1 in namespace..."
sudo ip netns exec mdns ip addr add 10.200.1.2/24 dev veth1
sudo ip netns exec mdns ip link set veth1 up
sudo ip netns exec mdns ip link set lo up

echo ""
echo "=== Setup complete ==="
echo "veth0: 10.200.1.1 (root namespace)"
echo "veth1: 10.200.1.2 (mdns namespace)"
echo ""
echo "Run app:    sudo ./market_data veth0 xdp_filter.o"
echo "Send pkts:  sudo ip netns exec mdns python3 market_maker.py"
