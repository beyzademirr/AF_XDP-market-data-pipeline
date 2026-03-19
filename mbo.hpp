#pragma once

#include <cstdint>
#include <cstring>
#include <limits>

// ============================================================================
// Market-By-Order (MBO) Core Structures
// All pre-allocated, zero dynamic allocation in hot path
// ============================================================================

// Null index sentinel for intrusive linked lists
static constexpr uint32_t NULL_IDX = 0xFFFFFFFF;

// ============================================================================
// Order struct - element of intrusive doubly linked list
// ============================================================================
struct Order {
    uint64_t order_id;    // Unique order identifier
    uint32_t qty;         // Remaining quantity
    uint32_t prev_idx;    // Index of previous order in price level queue
    uint32_t next_idx;    // Index of next order in price level queue
    uint32_t price;       // Price level (for reverse lookup)
    bool     is_bid;      // Side indicator
    bool     active;      // Is this slot in use?
    uint16_t _pad;        // Alignment
};

static_assert(sizeof(Order) == 32, "Order should be 32 bytes for cache alignment");

// ============================================================================
// OrderPool - Pre-allocated flat array of Order structs
// Acts as a custom heap with O(1) alloc/free via freelist
// ============================================================================
class OrderPool {
public:
    static constexpr uint32_t MAX_ORDERS = 1000000;

private:
    Order orders[MAX_ORDERS];
    uint32_t free_head;      // Head of free list
    uint32_t active_count;

public:
    void init() {
        // Build free list - each order points to next free slot
        for (uint32_t i = 0; i < MAX_ORDERS - 1; i++) {
            orders[i].next_idx = i + 1;
            orders[i].active = false;
        }
        orders[MAX_ORDERS - 1].next_idx = NULL_IDX;
        orders[MAX_ORDERS - 1].active = false;
        
        free_head = 0;
        active_count = 0;
    }

    // Allocate an order slot - O(1)
    inline uint32_t alloc() {
        if (free_head == NULL_IDX) [[unlikely]] {
            return NULL_IDX;  // Pool exhausted
        }
        
        uint32_t idx = free_head;
        free_head = orders[idx].next_idx;
        orders[idx].active = true;
        orders[idx].prev_idx = NULL_IDX;
        orders[idx].next_idx = NULL_IDX;
        active_count++;
        return idx;
    }

    // Free an order slot - O(1)
    inline void free(uint32_t idx) {
        if (idx >= MAX_ORDERS || !orders[idx].active) [[unlikely]] {
            return;
        }
        
        orders[idx].active = false;
        orders[idx].next_idx = free_head;
        free_head = idx;
        active_count--;
    }

    // Direct access to order by index - O(1)
    inline Order& operator[](uint32_t idx) {
        return orders[idx];
    }

    inline const Order& operator[](uint32_t idx) const {
        return orders[idx];
    }

    uint32_t get_active_count() const { return active_count; }
};

// ============================================================================
// MBO Price Level Book
// Uses intrusive linked lists for order queues at each price level
// ============================================================================
class MBOBook {
public:
    static constexpr uint32_t MAX_PRICE_LEVELS = 100000;
    static constexpr uint32_t INVALID_PRICE = std::numeric_limits<uint32_t>::max();

private:
    // Head/tail indices for order queues at each price level
    uint32_t bid_head[MAX_PRICE_LEVELS];
    uint32_t bid_tail[MAX_PRICE_LEVELS];
    uint32_t ask_head[MAX_PRICE_LEVELS];
    uint32_t ask_tail[MAX_PRICE_LEVELS];

    // Aggregated quantity at each level (maintained for fast lookups)
    uint32_t bid_qty[MAX_PRICE_LEVELS];
    uint32_t ask_qty[MAX_PRICE_LEVELS];

    // Best bid/ask tracking
    uint32_t best_bid;
    uint32_t best_ask;

    // Reference to shared order pool
    OrderPool* pool;

    // Stats
    uint64_t total_adds;
    uint64_t total_trades;
    uint64_t total_cancels;

public:
    void init(OrderPool* order_pool) {
        pool = order_pool;
        
        for (uint32_t i = 0; i < MAX_PRICE_LEVELS; i++) {
            bid_head[i] = NULL_IDX;
            bid_tail[i] = NULL_IDX;
            ask_head[i] = NULL_IDX;
            ask_tail[i] = NULL_IDX;
            bid_qty[i] = 0;
            ask_qty[i] = 0;
        }
        
        best_bid = 0;
        best_ask = INVALID_PRICE;
        total_adds = 0;
        total_trades = 0;
        total_cancels = 0;
    }

    // ========================================================================
    // O(1) Enqueue - Add order to tail of price level queue
    // ========================================================================
    inline uint32_t enqueue_bid(uint32_t price, uint64_t order_id, uint32_t qty) {
        if (price >= MAX_PRICE_LEVELS) [[unlikely]] return NULL_IDX;
        
        uint32_t idx = pool->alloc();
        if (idx == NULL_IDX) [[unlikely]] return NULL_IDX;
        
        Order& o = (*pool)[idx];
        o.order_id = order_id;
        o.qty = qty;
        o.price = price;
        o.is_bid = true;
        o.prev_idx = bid_tail[price];
        o.next_idx = NULL_IDX;
        
        if (bid_tail[price] != NULL_IDX) {
            (*pool)[bid_tail[price]].next_idx = idx;
        } else {
            bid_head[price] = idx;
        }
        bid_tail[price] = idx;
        
        bid_qty[price] += qty;
        if (price > best_bid) {
            best_bid = price;
        }
        
        total_adds++;
        return idx;
    }

    inline uint32_t enqueue_ask(uint32_t price, uint64_t order_id, uint32_t qty) {
        if (price >= MAX_PRICE_LEVELS) [[unlikely]] return NULL_IDX;
        
        uint32_t idx = pool->alloc();
        if (idx == NULL_IDX) [[unlikely]] return NULL_IDX;
        
        Order& o = (*pool)[idx];
        o.order_id = order_id;
        o.qty = qty;
        o.price = price;
        o.is_bid = false;
        o.prev_idx = ask_tail[price];
        o.next_idx = NULL_IDX;
        
        if (ask_tail[price] != NULL_IDX) {
            (*pool)[ask_tail[price]].next_idx = idx;
        } else {
            ask_head[price] = idx;
        }
        ask_tail[price] = idx;
        
        ask_qty[price] += qty;
        if (price < best_ask) {
            best_ask = price;
        }
        
        total_adds++;
        return idx;
    }

    // ========================================================================
    // O(1) Dequeue - Remove order from queue (by index)
    // ========================================================================
    inline void dequeue(uint32_t idx) {
        if (idx == NULL_IDX) return;
        
        Order& o = (*pool)[idx];
        if (!o.active) return;
        
        uint32_t price = o.price;
        bool is_bid = o.is_bid;
        
        uint32_t* head = is_bid ? &bid_head[price] : &ask_head[price];
        uint32_t* tail = is_bid ? &bid_tail[price] : &ask_tail[price];
        uint32_t* qty_arr = is_bid ? bid_qty : ask_qty;
        
        // Update aggregate qty
        if (qty_arr[price] >= o.qty) {
            qty_arr[price] -= o.qty;
        } else {
            qty_arr[price] = 0;
        }
        
        // Unlink from doubly linked list
        if (o.prev_idx != NULL_IDX) {
            (*pool)[o.prev_idx].next_idx = o.next_idx;
        } else {
            *head = o.next_idx;
        }
        
        if (o.next_idx != NULL_IDX) {
            (*pool)[o.next_idx].prev_idx = o.prev_idx;
        } else {
            *tail = o.prev_idx;
        }
        
        pool->free(idx);
        total_cancels++;
        
        // Update BBO if needed
        if (is_bid && price == best_bid && bid_qty[price] == 0) {
            update_best_bid();
        } else if (!is_bid && price == best_ask && ask_qty[price] == 0) {
            update_best_ask();
        }
    }

    // ========================================================================
    // Trade execution - consume from front of queue (price-time priority)
    // ========================================================================
    inline uint32_t trade_bid(uint32_t price, uint32_t qty) {
        if (price >= MAX_PRICE_LEVELS) return 0;
        
        uint32_t filled = 0;
        
        while (qty > 0 && bid_head[price] != NULL_IDX) {
            uint32_t idx = bid_head[price];
            Order& o = (*pool)[idx];
            
            if (o.qty <= qty) {
                // Fully fill this order
                filled += o.qty;
                qty -= o.qty;
                dequeue(idx);
            } else {
                // Partial fill
                o.qty -= qty;
                bid_qty[price] -= qty;
                filled += qty;
                qty = 0;
            }
        }
        
        total_trades++;
        return filled;
    }

    inline uint32_t trade_ask(uint32_t price, uint32_t qty) {
        if (price >= MAX_PRICE_LEVELS) return 0;
        
        uint32_t filled = 0;
        
        while (qty > 0 && ask_head[price] != NULL_IDX) {
            uint32_t idx = ask_head[price];
            Order& o = (*pool)[idx];
            
            if (o.qty <= qty) {
                filled += o.qty;
                qty -= o.qty;
                dequeue(idx);
            } else {
                o.qty -= qty;
                ask_qty[price] -= qty;
                filled += qty;
                qty = 0;
            }
        }
        
        total_trades++;
        return filled;
    }

    // ========================================================================
    // BBO Updates
    // ========================================================================
    void update_best_bid() {
        while (best_bid > 0 && bid_qty[best_bid] == 0) {
            --best_bid;
        }
        if (bid_qty[best_bid] == 0) {
            best_bid = 0;
        }
    }

    void update_best_ask() {
        while (best_ask < MAX_PRICE_LEVELS && ask_qty[best_ask] == 0) {
            ++best_ask;
        }
        if (best_ask >= MAX_PRICE_LEVELS || ask_qty[best_ask] == 0) {
            best_ask = INVALID_PRICE;
        }
    }

    // ========================================================================
    // Accessors
    // ========================================================================
    uint32_t get_best_bid() const { return best_bid; }
    uint32_t get_best_ask() const { return best_ask; }
    uint32_t get_bid_qty(uint32_t price) const { return bid_qty[price]; }
    uint32_t get_ask_qty(uint32_t price) const { return ask_qty[price]; }
    uint64_t get_total_adds() const { return total_adds; }
    uint64_t get_total_trades() const { return total_trades; }
    
    uint32_t spread() const {
        if (best_ask == INVALID_PRICE || best_bid == 0) return 0;
        return best_ask - best_bid;
    }

    // Clear book for reuse (when evicted from cache)
    // Frees all orders back to the pool before resetting
    void clear() {
        for (uint32_t i = 0; i < MAX_PRICE_LEVELS; i++) {
            // Free bid orders at this level
            uint32_t idx = bid_head[i];
            while (idx != NULL_IDX) {
                uint32_t next = (*pool)[idx].next_idx;
                pool->free(idx);
                idx = next;
            }
            // Free ask orders at this level
            idx = ask_head[i];
            while (idx != NULL_IDX) {
                uint32_t next = (*pool)[idx].next_idx;
                pool->free(idx);
                idx = next;
            }
        }
        for (uint32_t i = 0; i < MAX_PRICE_LEVELS; i++) {
            bid_head[i] = NULL_IDX;
            bid_tail[i] = NULL_IDX;
            ask_head[i] = NULL_IDX;
            ask_tail[i] = NULL_IDX;
            bid_qty[i] = 0;
            ask_qty[i] = 0;
        }
        best_bid = 0;
        best_ask = INVALID_PRICE;
    }
};
