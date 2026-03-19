#pragma once

#include <cstdint>
#include <cstring>
#include <limits>

// 25-byte Multi-Symbol ITCH message - packed, big-endian on wire
struct __attribute__((packed)) ITCH_Message {
    char     type;        // 'A' = Add, 'T' = Trade
    char     symbol[8];   // Ticker symbol (e.g., "BTC/USDT")
    uint64_t ts_ns;       // Timestamp in nanoseconds
    uint32_t price;       // Price in ticks
    uint32_t qty;         // Quantity
};

static_assert(sizeof(ITCH_Message) == 25, "ITCH_Message must be exactly 25 bytes");

// Byte order conversion helpers
inline uint64_t ntohll(uint64_t val) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap64(val);
#else
    return val;
#endif
}

inline uint32_t ntohl_inline(uint32_t val) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(val);
#else
    return val;
#endif
}

// Convert from network byte order to host byte order in-place
inline void itch_to_host(ITCH_Message* msg) {
    msg->ts_ns = ntohll(msg->ts_ns);
    msg->price = ntohl_inline(msg->price);
    msg->qty   = ntohl_inline(msg->qty);
}

// Zero-allocation price level book using flat arrays
// Supports O(1) add and trade operations
// Price is used as direct index into the arrays
struct PriceLevelBook {
    static constexpr uint32_t MAX_PRICE_LEVELS = 100000;
    static constexpr uint32_t INVALID_PRICE = std::numeric_limits<uint32_t>::max();

    // Flat arrays indexed by price
    uint32_t bid_qty[MAX_PRICE_LEVELS];  // Quantity at each bid price level
    uint32_t ask_qty[MAX_PRICE_LEVELS];  // Quantity at each ask price level

    // Best bid/ask tracking
    uint32_t best_bid;  // Highest bid price with quantity > 0
    uint32_t best_ask;  // Lowest ask price with quantity > 0

    // Statistics
    uint64_t total_adds;
    uint64_t total_trades;
    uint64_t last_ts_ns;

    // Initialize the book - call once at startup
    void init() {
        std::memset(bid_qty, 0, sizeof(bid_qty));
        std::memset(ask_qty, 0, sizeof(ask_qty));
        best_bid = 0;
        best_ask = INVALID_PRICE;
        total_adds = 0;
        total_trades = 0;
        last_ts_ns = 0;
    }

    // Process an Add message - O(1)
    // For simplicity, we treat adds with price < mid as bids, >= mid as asks
    // In production, the message would indicate side
    inline void process_add(uint32_t price, uint32_t qty, uint64_t ts_ns) {
        if (price >= MAX_PRICE_LEVELS) [[unlikely]] {
            return;
        }

        // Heuristic: if price <= best_bid or no best_ask, treat as bid
        // Otherwise treat as ask
        // In real systems, side would be explicit in the message
        bool is_bid = (best_ask == INVALID_PRICE) || (price <= best_bid) || 
                      (best_bid == 0 && price < MAX_PRICE_LEVELS / 2);

        if (is_bid) {
            add_bid(price, qty);
        } else {
            add_ask(price, qty);
        }

        last_ts_ns = ts_ns;
        ++total_adds;
    }

    // Add to bid side - O(1)
    inline void add_bid(uint32_t price, uint32_t qty) {
        bid_qty[price] += qty;
        if (price > best_bid) {
            best_bid = price;
        }
    }

    // Add to ask side - O(1)
    inline void add_ask(uint32_t price, uint32_t qty) {
        ask_qty[price] += qty;
        if (price < best_ask) {
            best_ask = price;
        }
    }

    // Process a Trade message - O(1) average, O(n) worst case for BBO update
    // Reduces quantity at the given price level
    inline void process_trade(uint32_t price, uint32_t qty, uint64_t ts_ns) {
        if (price >= MAX_PRICE_LEVELS) [[unlikely]] {
            return;
        }

        // Determine which side the trade occurred on
        if (price <= best_bid && bid_qty[price] > 0) {
            trade_bid(price, qty);
        } else if (price >= best_ask && ask_qty[price] > 0) {
            trade_ask(price, qty);
        }
        // If neither matches, the trade is for a price level we don't track

        last_ts_ns = ts_ns;
        ++total_trades;
    }

    // Trade on bid side - reduces quantity
    inline void trade_bid(uint32_t price, uint32_t qty) {
        if (bid_qty[price] >= qty) {
            bid_qty[price] -= qty;
        } else {
            bid_qty[price] = 0;
        }

        // Update best_bid if this level is now empty
        if (price == best_bid && bid_qty[price] == 0) {
            update_best_bid();
        }
    }

    // Trade on ask side - reduces quantity
    inline void trade_ask(uint32_t price, uint32_t qty) {
        if (ask_qty[price] >= qty) {
            ask_qty[price] -= qty;
        } else {
            ask_qty[price] = 0;
        }

        // Update best_ask if this level is now empty
        if (price == best_ask && ask_qty[price] == 0) {
            update_best_ask();
        }
    }

    // Find new best bid by scanning down from current
    // Called rarely - only when best level is exhausted
    void update_best_bid() {
        while (best_bid > 0 && bid_qty[best_bid] == 0) {
            --best_bid;
        }
        if (bid_qty[best_bid] == 0) {
            best_bid = 0;
        }
    }

    // Find new best ask by scanning up from current
    // Called rarely - only when best level is exhausted
    void update_best_ask() {
        while (best_ask < MAX_PRICE_LEVELS && ask_qty[best_ask] == 0) {
            ++best_ask;
        }
        if (best_ask >= MAX_PRICE_LEVELS || ask_qty[best_ask] == 0) {
            best_ask = INVALID_PRICE;
        }
    }

    // Process any ITCH message - dispatcher
    inline void process(const ITCH_Message* msg) {
        switch (msg->type) {
            case 'A':
                process_add(msg->price, msg->qty, msg->ts_ns);
                break;
            case 'T':
                process_trade(msg->price, msg->qty, msg->ts_ns);
                break;
            default:
                // Unknown message type - ignore
                break;
        }
    }

    // Get spread in ticks (returns 0 if no valid spread)
    inline uint32_t spread() const {
        if (best_ask == INVALID_PRICE || best_bid == 0) {
            return 0;
        }
        return best_ask - best_bid;
    }

    // Check if book has valid two-sided market
    inline bool has_two_sided_market() const {
        return best_bid > 0 && best_ask != INVALID_PRICE && best_ask > best_bid;
    }
};
