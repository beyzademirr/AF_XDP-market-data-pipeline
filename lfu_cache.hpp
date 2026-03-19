#pragma once

#include <cstdint>
#include <cstring>
#include "fenwick.hpp"

// ============================================================================
// O(1) LFU (Least Frequently Used) Cache
// Uses frequency buckets with doubly-linked lists for constant-time operations
// Pre-allocated for zero dynamic memory in hot path
// ============================================================================

static constexpr uint32_t LFU_NULL_IDX = 0xFFFFFFFF;
static constexpr uint32_t MAX_CACHE_SIZE = 5;        // 5 active books in cache
static constexpr uint32_t MAX_SYMBOLS = 256;         // Total symbols we can track

// ============================================================================
// LFU Node - Entry in the cache
// ============================================================================
struct LFUNode {
    uint16_t book_id;           // Which book this node represents
    uint8_t  symbol[8];         // Symbol string (for reverse lookup)
    uint32_t frequency;         // Access frequency
    
    // Intrusive list for frequency bucket
    uint32_t freq_prev;         // Previous node in same frequency bucket
    uint32_t freq_next;         // Next node in same frequency bucket
    
    bool in_cache;              // Is this slot actively in cache?
    bool valid;                 // Has this slot ever been used?
    uint16_t _pad;
};

// ============================================================================
// Frequency Bucket - Doubly linked list of all nodes with same frequency
// ============================================================================
struct FreqBucket {
    uint32_t head;
    uint32_t tail;
    uint32_t count;
};

// ============================================================================
// LFU Cache for MBO Books
// Maintains up to MAX_CACHE_SIZE active books with O(1) access/evict
// ============================================================================
class LFUCache {
public:
    static constexpr uint32_t MAX_FREQUENCY = 1024;  // Max tracked frequency

private:
    // Pre-allocated nodes (one per possible symbol)
    LFUNode nodes[MAX_SYMBOLS];
    
    // Pre-allocated MBO books with BIT
    MBOBookWithBIT books[MAX_CACHE_SIZE];
    
    // Mapping: book_id -> cache slot (or LFU_NULL_IDX if not cached)
    uint32_t book_to_slot[MAX_SYMBOLS];
    
    // Reverse mapping: cache slot -> book_id
    uint32_t slot_to_book[MAX_CACHE_SIZE];
    
    // Frequency buckets
    FreqBucket freq_buckets[MAX_FREQUENCY];
    
    // Minimum frequency with non-empty bucket (for O(1) eviction)
    uint32_t min_frequency;
    
    // Cache state
    uint32_t cache_size;
    
    // Shared order pool
    OrderPool* pool;

    // Stats
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;

public:
    void init(OrderPool* order_pool) {
        pool = order_pool;
        
        for (uint32_t i = 0; i < MAX_SYMBOLS; i++) {
            nodes[i].in_cache = false;
            nodes[i].valid = false;
            nodes[i].frequency = 0;
            nodes[i].freq_prev = LFU_NULL_IDX;
            nodes[i].freq_next = LFU_NULL_IDX;
            book_to_slot[i] = LFU_NULL_IDX;
        }
        
        for (uint32_t i = 0; i < MAX_CACHE_SIZE; i++) {
            books[i].init(pool);
            slot_to_book[i] = LFU_NULL_IDX;
        }
        
        for (uint32_t i = 0; i < MAX_FREQUENCY; i++) {
            freq_buckets[i].head = LFU_NULL_IDX;
            freq_buckets[i].tail = LFU_NULL_IDX;
            freq_buckets[i].count = 0;
        }
        
        min_frequency = 0;
        cache_size = 0;
        hits = 0;
        misses = 0;
        evictions = 0;
    }

    // ========================================================================
    // O(1) Access - Returns book pointer, handles cache miss/eviction
    // ========================================================================
    MBOBookWithBIT* access(uint16_t book_id, const char* symbol) {
        if (book_id >= MAX_SYMBOLS) [[unlikely]] return nullptr;
        
        LFUNode& node = nodes[book_id];
        
        if (node.in_cache) {
            // Cache hit - O(1)
            hits++;
            update_frequency(book_id);
            return &books[book_to_slot[book_id]];
        }
        
        // Cache miss
        misses++;
        
        // Need to add to cache
        uint32_t slot;
        
        if (cache_size < MAX_CACHE_SIZE) {
            // Cache not full - use next available slot
            slot = cache_size;
            cache_size++;
        } else {
            // Cache full - evict LFU entry - O(1)
            slot = evict_lfu();
        }
        
        // Initialize the node
        node.book_id = book_id;
        std::memcpy(node.symbol, symbol, 8);
        node.in_cache = true;
        node.valid = true;
        node.frequency = 1;
        
        // Add to frequency bucket 1
        add_to_bucket(book_id, 1);
        min_frequency = 1;
        
        // Set up mappings
        book_to_slot[book_id] = slot;
        slot_to_book[slot] = book_id;
        
        // Clear and return the book
        books[slot].clear();
        books[slot].init(pool);
        
        return &books[slot];
    }

    // ========================================================================
    // O(1) Frequency Update
    // ========================================================================
    inline void update_frequency(uint16_t book_id) {
        LFUNode& node = nodes[book_id];
        uint32_t old_freq = node.frequency;
        uint32_t new_freq = (old_freq < MAX_FREQUENCY - 1) ? old_freq + 1 : old_freq;
        
        // Remove from old frequency bucket
        remove_from_bucket(book_id, old_freq);
        
        // Update min_frequency if needed
        if (freq_buckets[old_freq].count == 0 && old_freq == min_frequency) {
            min_frequency = new_freq;
        }
        
        // Add to new frequency bucket
        node.frequency = new_freq;
        add_to_bucket(book_id, new_freq);
    }

    // ========================================================================
    // O(1) Eviction - Returns slot that was freed
    // ========================================================================
    inline uint32_t evict_lfu() {
        // min_frequency invariant is maintained by access() and update_frequency(),
        // so no scanning needed - direct O(1) lookup

        // Get tail of min frequency bucket (LRU within same frequency)
        uint32_t victim_id = freq_buckets[min_frequency].tail;
        if (victim_id == LFU_NULL_IDX) [[unlikely]] {
            return 0;
        }
        
        LFUNode& victim = nodes[victim_id];
        uint32_t slot = book_to_slot[victim_id];
        
        // Remove from frequency bucket
        remove_from_bucket(victim_id, min_frequency);
        
        // Clear cache state
        victim.in_cache = false;
        book_to_slot[victim_id] = LFU_NULL_IDX;
        
        evictions++;
        return slot;
    }

    // ========================================================================
    // Frequency Bucket Operations - O(1)
    // ========================================================================
    inline void add_to_bucket(uint16_t book_id, uint32_t freq) {
        FreqBucket& bucket = freq_buckets[freq];
        LFUNode& node = nodes[book_id];
        
        node.freq_prev = LFU_NULL_IDX;
        node.freq_next = bucket.head;
        
        if (bucket.head != LFU_NULL_IDX) {
            nodes[bucket.head].freq_prev = book_id;
        } else {
            bucket.tail = book_id;
        }
        
        bucket.head = book_id;
        bucket.count++;
    }

    inline void remove_from_bucket(uint16_t book_id, uint32_t freq) {
        FreqBucket& bucket = freq_buckets[freq];
        LFUNode& node = nodes[book_id];
        
        if (node.freq_prev != LFU_NULL_IDX) {
            nodes[node.freq_prev].freq_next = node.freq_next;
        } else {
            bucket.head = node.freq_next;
        }
        
        if (node.freq_next != LFU_NULL_IDX) {
            nodes[node.freq_next].freq_prev = node.freq_prev;
        } else {
            bucket.tail = node.freq_prev;
        }
        
        bucket.count--;
        node.freq_prev = LFU_NULL_IDX;
        node.freq_next = LFU_NULL_IDX;
    }

    // ========================================================================
    // Direct access (bypasses frequency update - use for read-only ops)
    // ========================================================================
    MBOBookWithBIT* peek(uint16_t book_id) {
        if (book_id >= MAX_SYMBOLS || !nodes[book_id].in_cache) {
            return nullptr;
        }
        return &books[book_to_slot[book_id]];
    }

    // ========================================================================
    // Stats
    // ========================================================================
    uint64_t get_hits() const { return hits; }
    uint64_t get_misses() const { return misses; }
    uint64_t get_evictions() const { return evictions; }
    uint32_t get_cache_size() const { return cache_size; }
    
    double hit_rate() const {
        uint64_t total = hits + misses;
        return (total > 0) ? (static_cast<double>(hits) / total) : 0.0;
    }

    // Check if a book is currently in cache
    bool is_cached(uint16_t book_id) const {
        return book_id < MAX_SYMBOLS && nodes[book_id].in_cache;
    }

    // Get frequency of a book
    uint32_t get_frequency(uint16_t book_id) const {
        if (book_id >= MAX_SYMBOLS || !nodes[book_id].valid) return 0;
        return nodes[book_id].frequency;
    }
};
