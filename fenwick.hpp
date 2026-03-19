#pragma once

#include <cstdint>
#include <cstring>

// ============================================================================
// Fenwick Tree (Binary Indexed Tree) for O(log N) prefix sum queries
// Used to quickly answer: "What's the total volume from price 0 to P?"
// ============================================================================

template <uint32_t N>
class FenwickTree {
private:
    int64_t tree[N + 1];  // 1-indexed for classic BIT operations

public:
    void init() {
        std::memset(tree, 0, sizeof(tree));
    }

    // Add delta to position idx - O(log N)
    inline void update(uint32_t idx, int64_t delta) {
        if (idx >= N) [[unlikely]] return;
        idx++;  // 1-indexed
        while (idx <= N) {
            tree[idx] += delta;
            idx += idx & (-idx);  // Add LSB
        }
    }

    // Query prefix sum [0, idx] - O(log N)
    inline int64_t prefix_sum(uint32_t idx) const {
        if (idx >= N) idx = N - 1;
        idx++;  // 1-indexed
        int64_t sum = 0;
        while (idx > 0) {
            sum += tree[idx];
            idx -= idx & (-idx);  // Remove LSB
        }
        return sum;
    }

    // Range sum [left, right] - O(log N)
    inline int64_t range_sum(uint32_t left, uint32_t right) const {
        if (left > right) return 0;
        if (left == 0) return prefix_sum(right);
        return prefix_sum(right) - prefix_sum(left - 1);
    }

    // Point query at single index - O(log N)
    inline int64_t point_query(uint32_t idx) const {
        return range_sum(idx, idx);
    }

    // Find largest index with nonzero value via binary lifting - O(log N)
    // Returns N if all values are zero (sentinel)
    inline uint32_t find_last_nonzero() const {
        int64_t total = prefix_sum(N - 1);
        if (total <= 0) return N;

        // Binary lifting: find smallest i where prefix_sum(i) >= total
        uint32_t pos = 0;
        int64_t remaining = total;
        for (uint32_t pw = highest_bit(); pw > 0; pw >>= 1) {
            if (pos + pw <= N && tree[pos + pw] < remaining) {
                pos += pw;
                remaining -= tree[pos];
            }
        }
        return pos;  // 0-indexed
    }

    // Find smallest index with nonzero value via binary lifting - O(log N)
    // Returns N if all values are zero (sentinel)
    inline uint32_t find_first_nonzero() const {
        int64_t total = prefix_sum(N - 1);
        if (total <= 0) return N;

        // Binary lifting: find smallest i where prefix_sum(i) >= 1
        uint32_t pos = 0;
        int64_t remaining = 1;
        for (uint32_t pw = highest_bit(); pw > 0; pw >>= 1) {
            if (pos + pw <= N && tree[pos + pw] < remaining) {
                pos += pw;
                remaining -= tree[pos];
            }
        }
        return pos;  // 0-indexed
    }

private:
    static constexpr uint32_t highest_bit() {
        uint32_t pw = 1;
        while (pw * 2 <= N) pw *= 2;
        return pw;
    }
};

// ============================================================================
// Enhanced MBO Book with Fenwick Tree for volume tracking
// Maintains both intrusive order lists AND cumulative volume BIT
// ============================================================================

#include "mbo.hpp"

class MBOBookWithBIT {
public:
    static constexpr uint32_t MAX_PRICE_LEVELS = 100000;
    static constexpr uint32_t INVALID_PRICE = 0xFFFFFFFF;

private:
    // Head/tail indices for order queues
    uint32_t bid_head[MAX_PRICE_LEVELS];
    uint32_t bid_tail[MAX_PRICE_LEVELS];
    uint32_t ask_head[MAX_PRICE_LEVELS];
    uint32_t ask_tail[MAX_PRICE_LEVELS];

    // Aggregated quantity at each level
    uint32_t bid_qty[MAX_PRICE_LEVELS];
    uint32_t ask_qty[MAX_PRICE_LEVELS];

    // Fenwick trees for O(log N) cumulative volume queries
    FenwickTree<MAX_PRICE_LEVELS> bid_bit;
    FenwickTree<MAX_PRICE_LEVELS> ask_bit;

    // BBO tracking
    uint32_t best_bid;
    uint32_t best_ask;

    OrderPool* pool;

    uint64_t total_adds;
    uint64_t total_trades;

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
        
        bid_bit.init();
        ask_bit.init();
        
        best_bid = 0;
        best_ask = INVALID_PRICE;
        total_adds = 0;
        total_trades = 0;
    }

    // ========================================================================
    // Enqueue with Fenwick Tree update
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
        bid_bit.update(price, qty);  // Update BIT
        
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
        ask_bit.update(price, qty);  // Update BIT
        
        if (price < best_ask) {
            best_ask = price;
        }
        
        total_adds++;
        return idx;
    }

    // ========================================================================
    // Dequeue with Fenwick Tree update
    // ========================================================================
    inline void dequeue(uint32_t idx) {
        if (idx == NULL_IDX) return;
        
        Order& o = (*pool)[idx];
        if (!o.active) return;
        
        uint32_t price = o.price;
        bool is_bid = o.is_bid;
        uint32_t qty = o.qty;
        
        uint32_t* head = is_bid ? &bid_head[price] : &ask_head[price];
        uint32_t* tail = is_bid ? &bid_tail[price] : &ask_tail[price];
        uint32_t* qty_arr = is_bid ? bid_qty : ask_qty;
        
        // Update aggregate qty and BIT
        if (qty_arr[price] >= qty) {
            qty_arr[price] -= qty;
        } else {
            qty_arr[price] = 0;
        }
        
        if (is_bid) {
            bid_bit.update(price, -static_cast<int64_t>(qty));
        } else {
            ask_bit.update(price, -static_cast<int64_t>(qty));
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
        
        // Update BBO if needed
        if (is_bid && price == best_bid && bid_qty[price] == 0) {
            update_best_bid();
        } else if (!is_bid && price == best_ask && ask_qty[price] == 0) {
            update_best_ask();
        }
    }

    // ========================================================================
    // Trade with BIT update
    // ========================================================================
    inline uint32_t trade_bid(uint32_t price, uint32_t qty) {
        if (price >= MAX_PRICE_LEVELS) return 0;
        
        uint32_t filled = 0;
        
        while (qty > 0 && bid_head[price] != NULL_IDX) {
            uint32_t idx = bid_head[price];
            Order& o = (*pool)[idx];
            
            if (o.qty <= qty) {
                filled += o.qty;
                qty -= o.qty;
                dequeue(idx);
            } else {
                o.qty -= qty;
                bid_qty[price] -= qty;
                bid_bit.update(price, -static_cast<int64_t>(qty));
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
                ask_bit.update(price, -static_cast<int64_t>(qty));
                filled += qty;
                qty = 0;
            }
        }
        
        total_trades++;
        return filled;
    }

    // ========================================================================
    // Fenwick Tree Queries - O(log N)
    // ========================================================================
    
    // Total bid volume from price 0 up to 'price'
    inline int64_t cumulative_bid_volume(uint32_t price) const {
        return bid_bit.prefix_sum(price);
    }

    // Total ask volume from price 0 up to 'price'
    inline int64_t cumulative_ask_volume(uint32_t price) const {
        return ask_bit.prefix_sum(price);
    }

    // Bid volume in range [low, high]
    inline int64_t bid_volume_range(uint32_t low, uint32_t high) const {
        return bid_bit.range_sum(low, high);
    }

    // Ask volume in range [low, high]
    inline int64_t ask_volume_range(uint32_t low, uint32_t high) const {
        return ask_bit.range_sum(low, high);
    }

    // Market depth: total bid volume from best_bid down N levels
    inline int64_t bid_depth(uint32_t levels) const {
        if (best_bid == 0) return 0;
        uint32_t low = (best_bid > levels) ? (best_bid - levels) : 0;
        return bid_bit.range_sum(low, best_bid);
    }

    // Market depth: total ask volume from best_ask up N levels
    inline int64_t ask_depth(uint32_t levels) const {
        if (best_ask == INVALID_PRICE) return 0;
        uint32_t high = (best_ask + levels < MAX_PRICE_LEVELS) ? (best_ask + levels) : (MAX_PRICE_LEVELS - 1);
        return ask_bit.range_sum(best_ask, high);
    }

    // ========================================================================
    // BBO Updates - O(log N) via Fenwick tree binary lifting
    // ========================================================================
    void update_best_bid() {
        uint32_t idx = bid_bit.find_last_nonzero();
        best_bid = (idx < MAX_PRICE_LEVELS) ? idx : 0;
    }

    void update_best_ask() {
        uint32_t idx = ask_bit.find_first_nonzero();
        best_ask = (idx < MAX_PRICE_LEVELS) ? idx : INVALID_PRICE;
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

    // Clear book (used when evicted from LFU cache)
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

            bid_head[i] = NULL_IDX;
            bid_tail[i] = NULL_IDX;
            ask_head[i] = NULL_IDX;
            ask_tail[i] = NULL_IDX;
            bid_qty[i] = 0;
            ask_qty[i] = 0;
        }
        bid_bit.init();
        ask_bit.init();
        best_bid = 0;
        best_ask = INVALID_PRICE;
    }
};
