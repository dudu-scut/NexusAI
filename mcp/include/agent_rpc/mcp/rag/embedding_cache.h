/**
 * @file embedding_cache.h
 * @brief LRU cache to avoid duplicate API calls
 */

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <list>
#include <mutex>
#include <chrono>

namespace agent_rpc {
namespace mcp {
namespace rag {

/**
 * @brief Cache configuration
 */
struct CacheConfig {
    size_t max_size = 1000;           ///< Maximum cache entries
    int ttl_seconds = 3600;           ///< Cache TTL (seconds)
    bool enabled = true;              ///< Whether the cache is enabled
};

/**
 * @brief Cache statistics
 */
struct CacheStats {
    size_t hits = 0;                  ///< Cache hit count
    size_t misses = 0;                ///< Cache miss count
    size_t evictions = 0;             ///< Eviction count
    size_t size = 0;                  ///< Current cache size
    
    float hitRate() const {
        size_t total = hits + misses;
        return total > 0 ? static_cast<float>(hits) / total : 0.0f;
    }
};

/**
 * @brief LRU embedding cache
 * 
 * Manages the cache with the LRU (Least Recently Used) strategy.
 * Supports:
 * - Configurable maximum cache size
 * - TTL expiration
 * - Thread safety
 */
class EmbeddingCache {
public:
    explicit EmbeddingCache(const CacheConfig& config);
    ~EmbeddingCache() = default;
    
    // Disable copy
    EmbeddingCache(const EmbeddingCache&) = delete;
    EmbeddingCache& operator=(const EmbeddingCache&) = delete;
    
    /**
     * @brief Get a cached embedding
     * @param text Text key
     * @return The embedding if present and not expired, otherwise nullopt
     */
    std::optional<std::vector<float>> get(const std::string& text);
    
    /**
     * @brief Store in the cache
     * @param text Text key
     * @param embedding Embedding value
     */
    void put(const std::string& text, const std::vector<float>& embedding);
    
    /**
     * @brief Check whether a key exists (without updating LRU order)
     */
    bool contains(const std::string& text) const;
    
    /**
     * @brief Remove a key
     * @return true if the key existed and was removed
     */
    bool remove(const std::string& text);
    
    /**
     * @brief Clear the cache
     */
    void clear();
    
    /**
     * @brief Get the current cache size
     */
    size_t size() const;
    
    /**
     * @brief Get cache statistics
     */
    CacheStats getStats() const;
    
    /**
     * @brief Reset statistics
     */
    void resetStats();
    
    /**
     * @brief Get the configuration
     */
    const CacheConfig& getConfig() const { return config_; }
    
    /**
     * @brief Get the most recently evicted key (for testing)
     */
    std::string getLastEvictedKey() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_evicted_key_;
    }

private:
    struct CacheEntry {
        std::vector<float> embedding;
        std::chrono::steady_clock::time_point created_at;
    };
    
    using CacheList = std::list<std::pair<std::string, CacheEntry>>;
    using CacheMap = std::unordered_map<std::string, CacheList::iterator>;
    
    /**
     * @brief Check whether an entry is expired
     */
    bool isExpired(const CacheEntry& entry) const;
    
    /**
     * @brief Evict the least recently used entry
     */
    void evictLRU();
    
    /**
     * @brief Move an entry to the list front (most recently used)
     */
    void moveToFront(CacheMap::iterator it);
    
    CacheConfig config_;
    CacheList cache_list_;            ///< Doubly linked list; front is most recently used
    CacheMap cache_map_;              ///< Hash map for fast lookup
    mutable std::mutex mutex_;
    
    // Statistics
    mutable CacheStats stats_;
    std::string last_evicted_key_;
};

} // namespace rag
} // namespace mcp
} // namespace agent_rpc
