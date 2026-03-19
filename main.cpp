// AF_XDP Market Data Ingestion Pipeline
// Zero-allocation, nanosecond-optimized for WSL2 (SKB mode)
// Multi-Symbol MBO Architecture with O(1) LFU Cache

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <unistd.h>
#include <poll.h>
#include <net/if.h>
#include <linux/if_link.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <arpa/inet.h>

// XDP and BPF headers
#include <xdp/xsk.h>
#include <xdp/libxdp.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include "types.hpp"
#include "trie.hpp"
#include "lfu_cache.hpp"

// Configuration constants
static constexpr const char* INTERFACE_NAME = "veth0";
static constexpr const char* BPF_PROGRAM_PATH = "xdp_filter.o";
static constexpr int MARKET_DATA_PORT = 1234;

// UMEM configuration
static constexpr uint32_t NUM_FRAMES = 4096;
static constexpr uint32_t FRAME_SIZE = XSK_UMEM__DEFAULT_FRAME_SIZE;  // 4096 bytes
static constexpr uint32_t UMEM_SIZE = NUM_FRAMES * FRAME_SIZE;

// Ring sizes (must be power of 2)
static constexpr uint32_t RING_SIZE = 2048;
static constexpr uint32_t FILL_RING_SIZE = RING_SIZE * 2;
static constexpr uint32_t COMP_RING_SIZE = RING_SIZE;

// Batch size for ring operations
static constexpr uint32_t BATCH_SIZE = 64;

// Global state for cleanup
static volatile bool g_running = true;
static struct xsk_socket *g_xsk = nullptr;
static struct xsk_umem *g_umem = nullptr;
static void *g_umem_buffer = nullptr;
static struct xdp_program *g_xdp_prog = nullptr;
static int g_ifindex = 0;

// Signal handler for clean shutdown
void signal_handler(int sig) {
    (void)sig;
    g_running = false;
}

// Calculate offset into UMEM for a given frame
static inline void* frame_address(void* umem_buffer, uint64_t addr) {
    return xsk_umem__get_data(umem_buffer, xsk_umem__add_offset_to_addr(addr));
}

// Get pointer to UDP payload (ITCH message) from frame
static inline ITCH_Message* get_itch_message(void* frame_ptr, uint32_t frame_len) {
    // Parse Ethernet -> IPv4 -> UDP to find payload
    if (frame_len < sizeof(struct ethhdr)) {
        return nullptr;
    }

    auto *eth = reinterpret_cast<struct ethhdr*>(frame_ptr);
    if (ntohs(eth->h_proto) != ETH_P_IP) {
        return nullptr;
    }

    uint32_t offset = sizeof(struct ethhdr);
    if (frame_len < offset + sizeof(struct iphdr)) {
        return nullptr;
    }

    auto *ip = reinterpret_cast<struct iphdr*>(static_cast<char*>(frame_ptr) + offset);
    if (ip->version != 4 || ip->ihl < 5) {
        return nullptr;
    }

    uint32_t ip_hlen = ip->ihl * 4;
    offset += ip_hlen;
    if (frame_len < offset + sizeof(struct udphdr)) {
        return nullptr;
    }

    auto *udp = reinterpret_cast<struct udphdr*>(static_cast<char*>(frame_ptr) + offset);
    offset += sizeof(struct udphdr);

    if (frame_len < offset + sizeof(ITCH_Message)) {
        return nullptr;
    }

    return reinterpret_cast<ITCH_Message*>(static_cast<char*>(frame_ptr) + offset);
}

static void dump_packet_once(const void* pkt, uint32_t len) {
    static bool dumped = false;
    if (dumped) {
        return;
    }
    dumped = true;

    const unsigned char* p = static_cast<const unsigned char*>(pkt);
    uint32_t dump_len = len < 64 ? len : 64;

    printf("Parse failed: frame_len=%u, first %u bytes:\n", len, dump_len);
    for (uint32_t i = 0; i < dump_len; i++) {
        printf("%02x%s", p[i], (i + 1) % 16 == 0 ? "\n" : " ");
    }
    if (dump_len % 16 != 0) {
        printf("\n");
    }
}

// Initialize UMEM (shared memory region for packet data)
static int init_umem(struct xsk_umem **umem, void **buffer,
                     struct xsk_ring_prod *fill_ring,
                     struct xsk_ring_cons *comp_ring) {
    // Allocate aligned memory for UMEM
    int ret = posix_memalign(buffer, getpagesize(), UMEM_SIZE);
    if (ret != 0) {
        fprintf(stderr, "ERROR: posix_memalign failed: %s\n", strerror(ret));
        return -ret;
    }

    // Zero the buffer
    memset(*buffer, 0, UMEM_SIZE);

    // Configure UMEM
    struct xsk_umem_config umem_cfg = {
        .fill_size = FILL_RING_SIZE,
        .comp_size = COMP_RING_SIZE,
        .frame_size = FRAME_SIZE,
        .frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM,
        .flags = 0
    };

    ret = xsk_umem__create(umem, *buffer, UMEM_SIZE, fill_ring, comp_ring, &umem_cfg);
    if (ret != 0) {
        fprintf(stderr, "ERROR: xsk_umem__create failed: %s\n", strerror(-ret));
        free(*buffer);
        *buffer = nullptr;
        return ret;
    }

    printf("UMEM initialized: %u frames x %u bytes = %u bytes total\n",
           NUM_FRAMES, FRAME_SIZE, UMEM_SIZE);

    return 0;
}

// Initialize AF_XDP socket
static int init_xsk_socket(struct xsk_socket **xsk, struct xsk_umem *umem, 
                           const char *ifname, int queue_id,
                           struct xsk_ring_cons *rx_ring) {
    
    struct xsk_socket_config xsk_cfg = {
        .rx_size = RING_SIZE,
        .tx_size = 0,  // We only receive, no TX
        .libxdp_flags = XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD,  // We load our own XDP program
        .xdp_flags = XDP_FLAGS_SKB_MODE,  // Required for WSL2/virtual NICs
        .bind_flags = XDP_COPY           // Required for WSL2/virtual NICs
    };

    int ret = xsk_socket__create(xsk, ifname, queue_id, umem,
                                  rx_ring, nullptr, &xsk_cfg);
    if (ret != 0) {
        fprintf(stderr, "ERROR: xsk_socket__create failed: %s\n", strerror(-ret));
        return ret;
    }

    printf("AF_XDP socket created on %s queue %d (SKB mode, copy)\n", ifname, queue_id);
    return 0;
}

// Populate the fill ring with frame addresses
static void populate_fill_ring(struct xsk_ring_prod *fill_ring, uint32_t num_frames) {
    uint32_t idx;
    uint32_t ret = xsk_ring_prod__reserve(fill_ring, num_frames, &idx);
    if (ret != num_frames) {
        fprintf(stderr, "WARNING: Could only reserve %u of %u fill ring entries\n",
                ret, num_frames);
    }

    for (uint32_t i = 0; i < ret; i++) {
        *xsk_ring_prod__fill_addr(fill_ring, idx + i) = i * FRAME_SIZE;
    }

    xsk_ring_prod__submit(fill_ring, ret);
    printf("Populated fill ring with %u frame addresses\n", ret);
}

// Load and attach XDP program
static int load_xdp_program(const char *prog_path, const char *ifname, int ifindex) {
    int ret;

    // Load the XDP program
    g_xdp_prog = xdp_program__open_file(prog_path, "xdp", nullptr);
    if (libxdp_get_error(g_xdp_prog)) {
        fprintf(stderr, "ERROR: Failed to load XDP program from %s: %s\n",
                prog_path, strerror(errno));
        return -1;
    }

    // Attach to interface in SKB mode
    ret = xdp_program__attach(g_xdp_prog, ifindex, XDP_MODE_SKB, 0);
    if (ret < 0) {
        fprintf(stderr, "ERROR: Failed to attach XDP program to %s: %s\n",
                ifname, strerror(-ret));
        xdp_program__close(g_xdp_prog);
        g_xdp_prog = nullptr;
        return ret;
    }

    printf("XDP program loaded and attached to %s (ifindex %d)\n", ifname, ifindex);
    return 0;
}

// Update the XSKMAP with our socket FD
static int update_xsk_map(struct xdp_program *prog, struct xsk_socket *xsk, int queue_id) {
    // Find the xsks_map in the loaded program
    struct bpf_object *bpf_obj = xdp_program__bpf_obj(prog);
    struct bpf_map *map = bpf_object__find_map_by_name(bpf_obj, "xsks_map");
    if (!map) {
        fprintf(stderr, "ERROR: Could not find xsks_map in BPF program\n");
        return -1;
    }

    int map_fd = bpf_map__fd(map);
    int xsk_fd = xsk_socket__fd(xsk);

    int ret = bpf_map_update_elem(map_fd, &queue_id, &xsk_fd, BPF_ANY);
    if (ret != 0) {
        fprintf(stderr, "ERROR: Failed to update xsks_map: %s\n", strerror(errno));
        return -1;
    }

    printf("Updated xsks_map[%d] = %d\n", queue_id, xsk_fd);
    return 0;
}

// Cleanup resources
static void cleanup() {
    printf("\nCleaning up...\n");

    if (g_xdp_prog && g_ifindex > 0) {
        xdp_program__detach(g_xdp_prog, g_ifindex, XDP_MODE_SKB, 0);
        xdp_program__close(g_xdp_prog);
        printf("XDP program detached and closed\n");
    }

    if (g_xsk) {
        xsk_socket__delete(g_xsk);
        printf("XSK socket deleted\n");
    }

    if (g_umem) {
        xsk_umem__delete(g_umem);
        printf("UMEM deleted\n");
    }

    if (g_umem_buffer) {
        free(g_umem_buffer);
        printf("UMEM buffer freed\n");
    }
}

int main(int argc, char *argv[]) {
    const char *ifname = INTERFACE_NAME;
    const char *prog_path = BPF_PROGRAM_PATH;
    int queue_id = 0;

    // Allow interface override from command line
    if (argc > 1) {
        ifname = argv[1];
    }
    if (argc > 2) {
        prog_path = argv[2];
    }

    printf("=== AF_XDP Market Data Ingestion Pipeline ===\n");
    printf("Interface: %s\n", ifname);
    printf("BPF Program: %s\n", prog_path);
    printf("Target Port: UDP %d\n", MARKET_DATA_PORT);
    printf("\n");

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Get interface index
    g_ifindex = if_nametoindex(ifname);
    if (g_ifindex == 0) {
        fprintf(stderr, "ERROR: Interface %s not found\n", ifname);
        return 1;
    }
    printf("Interface %s has index %d\n", ifname, g_ifindex);

    // Initialize AF_XDP socket rings
    struct xsk_ring_cons rx_ring;
    struct xsk_ring_prod fill_ring;
    struct xsk_ring_cons comp_ring;

    // Load and attach XDP program FIRST (before socket creation)
    int ret = load_xdp_program(prog_path, ifname, g_ifindex);
    if (ret != 0) {
        return 1;
    }

    // Initialize UMEM (fill and comp rings are returned by umem creation)
    ret = init_umem(&g_umem, &g_umem_buffer, &fill_ring, &comp_ring);
    if (ret != 0) {
        cleanup();
        return 1;
    }

    // Initialize AF_XDP socket (rx ring is returned by socket creation)
    // Using INHIBIT_PROG_LOAD since we already loaded our XDP program
    ret = init_xsk_socket(&g_xsk, g_umem, ifname, queue_id, &rx_ring);
    if (ret != 0) {
        cleanup();
        return 1;
    }

    // Update XSKMAP with our socket
    ret = update_xsk_map(g_xdp_prog, g_xsk, queue_id);
    if (ret != 0) {
        cleanup();
        return 1;
    }

    // Populate fill ring with initial frame addresses
    populate_fill_ring(&fill_ring, NUM_FRAMES / 2);

    // ========================================
    // MBO Architecture Initialization
    // All static - zero heap allocation
    // ========================================
    static OrderPool order_pool;        // 1M pre-allocated orders
    static PrefixTrie symbol_trie;      // O(1) symbol→book_id routing
    static LFUCache lfu_cache;          // O(1) LFU with 5 cached MBOBookWithBIT

    order_pool.init();
    symbol_trie.init();
    lfu_cache.init(&order_pool);

    printf("MBO subsystem initialized:\n");
    printf("  - OrderPool: %u slots\n", OrderPool::MAX_ORDERS);
    printf("  - SymbolTrie: O(1) routing\n");
    printf("  - LFUCache: %u active books\n", MAX_CACHE_SIZE);

    printf("\n=== Starting Hot Loop ===\n");
    printf("Press Ctrl+C to stop\n\n");

    // Statistics
    uint64_t total_received = 0;
    uint64_t total_packets = 0;
    uint64_t total_adds = 0;
    uint64_t total_trades = 0;
    uint64_t parse_failures = 0;
    uint64_t last_print_packets = 0;
    uint64_t unique_symbols = 0;

    // Poll setup for blocking wait (optional, can busy-poll instead)
    struct pollfd fds[1];
    fds[0].fd = xsk_socket__fd(g_xsk);
    fds[0].events = POLLIN;

    // ========================================
    // HOT LOOP - No allocations beyond this point
    // ========================================
    while (g_running) {
        uint32_t idx_rx = 0;
        uint32_t rcvd;
        uint64_t addrs[BATCH_SIZE];

        // Peek at available packets in RX ring
        rcvd = xsk_ring_cons__peek(&rx_ring, BATCH_SIZE, &idx_rx);

        if (rcvd == 0) {
            // No packets available - optionally poll/sleep
            // For lowest latency, busy-poll (remove this block)
            poll(fds, 1, 100);  // 100ms timeout
            continue;
        }

        total_received += rcvd;

        // Process each received packet
        for (uint32_t i = 0; i < rcvd; i++) {
            // Get the descriptor
            const struct xdp_desc *desc = xsk_ring_cons__rx_desc(&rx_ring, idx_rx + i);
            addrs[i] = xsk_umem__extract_addr(desc->addr);
            
            // Get pointer to packet data in UMEM
            void *pkt = frame_address(g_umem_buffer, desc->addr);
            
            // Extract ITCH message from packet
            ITCH_Message *msg = get_itch_message(pkt, desc->len);
            if (!msg) {
                ++parse_failures;
                dump_packet_once(pkt, desc->len);
                continue;
            }
            
            // Convert from network byte order to host byte order
            // Note: We copy to avoid modifying shared memory
            ITCH_Message local_msg;
            memcpy(&local_msg, msg, sizeof(ITCH_Message));
            itch_to_host(&local_msg);
            
            // ========================================
            // MBO ROUTING - O(1) symbol lookup → O(1) cache access
            // ========================================
            
            // 1. Look up symbol → book_id (O(1) trie lookup)
            bool created = false;
            uint16_t book_id = symbol_trie.get_or_create(local_msg.symbol, &created);
            if (created) {
                ++unique_symbols;
            }
            
            // 2. Access MBO book via LFU cache (O(1) access/evict)
            MBOBookWithBIT* book = lfu_cache.access(book_id, local_msg.symbol);
            if (!book) {
                ++parse_failures;
                continue;
            }
            
            // 3. Route message to appropriate book operation
            if (local_msg.type == 'A') {
                // Add order - determine side from price heuristic
                // In real ITCH, there's a side field; here we use price vs mid
                uint32_t mid = (book->get_best_bid() + book->get_best_ask()) / 2;
                if (mid == 0) mid = 50000;  // Default mid if no BBO yet
                
                // Generate order_id from timestamp + price
                uint64_t order_id = (static_cast<uint64_t>(local_msg.ts_ns) << 32) | local_msg.price;
                
                if (local_msg.price <= mid) {
                    book->enqueue_bid(local_msg.price, order_id, local_msg.qty);
                } else {
                    book->enqueue_ask(local_msg.price, order_id, local_msg.qty);
                }
                ++total_adds;
            } else if (local_msg.type == 'T') {
                // Trade execution - consume from front of queue
                uint32_t mid = (book->get_best_bid() + book->get_best_ask()) / 2;
                if (mid == 0) mid = 50000;
                
                if (local_msg.price <= mid) {
                    book->trade_bid(local_msg.price, local_msg.qty);
                } else {
                    book->trade_ask(local_msg.price, local_msg.qty);
                }
                ++total_trades;
            }
            
            ++total_packets;
        }

        // Release processed packets back to kernel
        xsk_ring_cons__release(&rx_ring, rcvd);

        // Replenish fill ring with saved addresses
        uint32_t idx_fill = 0;
        uint32_t free_slots = xsk_ring_prod__reserve(&fill_ring, rcvd, &idx_fill);
        for (uint32_t i = 0; i < free_slots; i++) {
            *xsk_ring_prod__fill_addr(&fill_ring, idx_fill + i) = addrs[i];
        }
        xsk_ring_prod__submit(&fill_ring, free_slots);

        // Print stats periodically
        if (total_packets - last_print_packets >= 10000) {
            printf("Pkts: %lu | Symbols: %lu | Cache: %.1f%% hit | Orders: %u active\n",
                   total_packets, unique_symbols, 
                   lfu_cache.hit_rate() * 100.0,
                   order_pool.get_active_count());
            last_print_packets = total_packets;
        }
    }

    // Final statistics
    printf("\n=== Final Statistics ===\n");
    printf("Total Received: %lu\n", total_received);
    printf("Total Parsed: %lu\n", total_packets);
    printf("Parse Failures: %lu\n", parse_failures);
    printf("Total Adds: %lu\n", total_adds);
    printf("Total Trades: %lu\n", total_trades);
    printf("\n--- MBO Subsystem ---\n");
    printf("Unique Symbols: %lu\n", unique_symbols);
    printf("Active Orders: %u\n", order_pool.get_active_count());
    printf("LFU Cache Hits: %lu\n", lfu_cache.get_hits());
    printf("LFU Cache Misses: %lu\n", lfu_cache.get_misses());
    printf("LFU Cache Evictions: %lu\n", lfu_cache.get_evictions());
    printf("LFU Hit Rate: %.2f%%\n", lfu_cache.hit_rate() * 100.0);

    cleanup();
    return 0;
}
