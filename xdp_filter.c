// SPDX-License-Identifier: GPL-2.0
// XDP filter program for AF_XDP market data ingestion
// Redirects UDP port 1234 traffic to user-space, passes everything else

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

// Target UDP port for market data (host byte order)
#define MARKET_DATA_PORT 1234

// XSK map for AF_XDP socket redirection
// Key: queue index, Value: xsk file descriptor
struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(key_size, sizeof(int));
    __uint(value_size, sizeof(int));
    __uint(max_entries, 64);
} xsks_map SEC(".maps");

// Statistics map for debugging/monitoring
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(key_size, sizeof(__u32));
    __uint(value_size, sizeof(__u64));
    __uint(max_entries, 4);
} stats_map SEC(".maps");

// Stats indices
#define STATS_RX_PACKETS    0
#define STATS_RX_BYTES      1
#define STATS_REDIRECTED    2
#define STATS_PASSED        3

// Helper to update stats
static __always_inline void update_stats(__u32 key, __u64 value) {
    __u64 *counter = bpf_map_lookup_elem(&stats_map, &key);
    if (counter) {
        __sync_fetch_and_add(counter, value);
    }
}

SEC("xdp")
int xdp_market_data_filter(struct xdp_md *ctx) {
    void *data_end = (void *)(long)ctx->data_end;
    void *data = (void *)(long)ctx->data;

    // Parse Ethernet header
    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end) {
        return XDP_PASS;
    }

    // Only process IPv4 packets
    if (eth->h_proto != bpf_htons(ETH_P_IP)) {
        return XDP_PASS;
    }

    // Parse IP header
    struct iphdr *ip = (void *)(eth + 1);
    if ((void *)(ip + 1) > data_end) {
        return XDP_PASS;
    }

    // Validate IP header length (minimum 20 bytes, no options for simplicity)
    if (ip->ihl < 5) {
        return XDP_PASS;
    }

    // Only process UDP packets
    if (ip->protocol != IPPROTO_UDP) {
        return XDP_PASS;
    }

    // Calculate IP header length and find UDP header
    __u32 ip_header_len = ip->ihl * 4;
    struct udphdr *udp = (void *)((char *)ip + ip_header_len);
    if ((void *)(udp + 1) > data_end) {
        return XDP_PASS;
    }

    // Update packet stats
    update_stats(STATS_RX_PACKETS, 1);
    update_stats(STATS_RX_BYTES, data_end - data);

    // Check if this is our target port (market data on UDP 1234)
    if (bpf_ntohs(udp->dest) == MARKET_DATA_PORT) {
        // Verify we have enough data for the ITCH message (25 bytes)
        void *payload = (void *)(udp + 1);
        if (payload + 25 > data_end) {
            // Packet too small, pass to kernel
            update_stats(STATS_PASSED, 1);
            return XDP_PASS;
        }

        // Redirect to AF_XDP socket
        // Use rx_queue_index to select the correct socket
        int index = ctx->rx_queue_index;
        
        update_stats(STATS_REDIRECTED, 1);
        
        // bpf_redirect_map returns XDP_REDIRECT on success
        // Falls back to XDP_PASS if the socket is not configured
        return bpf_redirect_map(&xsks_map, index, XDP_PASS);
    }

    // Not our target port - pass to regular network stack
    update_stats(STATS_PASSED, 1);
    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
