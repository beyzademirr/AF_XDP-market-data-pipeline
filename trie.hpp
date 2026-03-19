#pragma once

#include <cstdint>
#include <cstring>

// Static Prefix Trie for O(1) symbol routing
// No dynamic allocation - all nodes pre-allocated in flat array
// Maps 8-byte symbol strings to uint16_t book_id (0-999)

class PrefixTrie {
public:
    static constexpr uint32_t MAX_NODES = 2048;      // Pre-allocated trie nodes
    static constexpr uint32_t ALPHABET_SIZE = 128;   // ASCII characters
    static constexpr uint16_t INVALID_BOOK_ID = 0xFFFF;
    static constexpr uint32_t SYMBOL_LEN = 8;

private:
    // Trie node - stored in flat array
    struct TrieNode {
        uint32_t children[ALPHABET_SIZE];  // Index into nodes[] array, 0 = no child
        uint16_t book_id;                   // INVALID_BOOK_ID if not a terminal
        uint16_t _pad;                      // Alignment padding
    };

    TrieNode nodes[MAX_NODES];
    uint32_t node_count;
    uint16_t next_book_id;

public:
    void init() {
        memset(nodes, 0, sizeof(nodes));
        node_count = 1;  // Node 0 is root
        next_book_id = 0;

        // Mark all as non-terminal
        for (uint32_t i = 0; i < MAX_NODES; i++) {
            nodes[i].book_id = INVALID_BOOK_ID;
        }
    }

    // Insert a symbol and assign it the next available book_id
    // Returns the assigned book_id, or INVALID_BOOK_ID if trie is full
    uint16_t insert(const char* symbol) {
        uint32_t node_idx = 0;  // Start at root

        for (uint32_t i = 0; i < SYMBOL_LEN; i++) {
            uint8_t c = static_cast<uint8_t>(symbol[i]);
            if (c >= ALPHABET_SIZE) {
                c = 0;  // Treat invalid chars as null
            }

            if (nodes[node_idx].children[c] == 0) {
                // Allocate new node
                if (node_count >= MAX_NODES) {
                    return INVALID_BOOK_ID;  // Trie full
                }
                nodes[node_idx].children[c] = node_count;
                node_count++;
            }

            node_idx = nodes[node_idx].children[c];
        }

        // Assign book_id if not already assigned
        if (nodes[node_idx].book_id == INVALID_BOOK_ID) {
            if (next_book_id >= 1000) {
                return INVALID_BOOK_ID;  // Max books reached
            }
            nodes[node_idx].book_id = next_book_id++;
        }

        return nodes[node_idx].book_id;
    }

    // Lookup symbol and return book_id
    // Returns INVALID_BOOK_ID if symbol not found
    // This is the hot-path function - must be O(1) with respect to trie depth (fixed 8 chars)
    inline uint16_t lookup(const char* symbol) const {
        uint32_t node_idx = 0;  // Start at root

        for (uint32_t i = 0; i < SYMBOL_LEN; i++) {
            uint8_t c = static_cast<uint8_t>(symbol[i]);
            if (c >= ALPHABET_SIZE) {
                c = 0;
            }

            uint32_t child = nodes[node_idx].children[c];
            if (child == 0) {
                return INVALID_BOOK_ID;  // Symbol not registered
            }

            node_idx = child;
        }

        return nodes[node_idx].book_id;
    }

    // Get or create: lookup first, insert if not found
    // Hot path for message processing
    // created: output flag indicating if a new symbol was registered
    inline uint16_t get_or_create(const char* symbol, bool* created = nullptr) {
        uint16_t id = lookup(symbol);
        if (id == INVALID_BOOK_ID) {
            id = insert(symbol);
            if (created) *created = true;
        } else {
            if (created) *created = false;
        }
        return id;
    }

    uint16_t get_book_count() const {
        return next_book_id;
    }
};
