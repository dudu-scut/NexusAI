/**
 * @file semantic_cache_index.cpp
 * @brief Implementation of SemanticCacheIndex
 */

#include "agent_rpc/common/rag/semantic_cache_index.h"
#include <cmath>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace agent_rpc {
namespace common {
namespace rag {

SemanticCacheIndex::SemanticCacheIndex(EmbeddingService* embedding)
    : embedding_(embedding) {}

std::string SemanticCacheIndex::vectorToKey(const std::vector<float>& v) {
    // Produce a deterministic hex string from the vector's data.
    // Not meant to be human-readable — just fast enough for map keys.
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < v.size() && i < 8; ++i) {
        // Cast the float bits to uint32_t for a stable hex representation.
        uint32_t bits;
        static_assert(sizeof(bits) == sizeof(float), "float must be 32-bit");
        std::memcpy(&bits, &v[i], sizeof(bits));
        oss << std::setw(8) << bits;
    }
    return oss.str();
}

static float cosineSimilarity(const std::vector<float>& a,
                               const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0f;
    double dot = 0.0, norm_a = 0.0, norm_b = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot   += static_cast<double>(a[i]) * b[i];
        norm_a += static_cast<double>(a[i]) * a[i];
        norm_b += static_cast<double>(b[i]) * b[i];
    }
    double denom = std::sqrt(norm_a) * std::sqrt(norm_b);
    return (denom < 1e-12) ? 0.0f : static_cast<float>(dot / denom);
}

std::optional<CachedResponse>
SemanticCacheIndex::lookup(const std::vector<float>& query_vector) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (query_vector.empty() || query_vectors_.empty()) {
        return std::nullopt;
    }

    std::string best_key;
    float best_sim = 0.0f;

    for (const auto& [key, vec] : query_vectors_) {
        float sim = cosineSimilarity(query_vector, vec);
        if (sim > best_sim) {
            best_sim = sim;
            best_key = key;
        }
    }

    if (best_sim < SIMILARITY_THRESHOLD) {
        return std::nullopt;
    }

    auto it = cache_.find(best_key);
    if (it == cache_.end()) {
        return std::nullopt; // stale entry — vector without cache data
    }

    // Bump hit count
    it->second.hit_count++;
    return it->second;
}

void SemanticCacheIndex::store(const std::vector<float>& query_vector,
                                const std::string& agent_id,
                                const std::string& response,
                                bool is_cacheable) {
    if (!is_cacheable || query_vector.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    std::string key = vectorToKey(query_vector);
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();

    // Insert / update response
    cache_[key] = {response, agent_id, now, 0};

    // Track vector for similarity search (only if not already tracked)
    bool found = false;
    for (auto& [k, _] : query_vectors_) {
        if (k == key) { found = true; break; }
    }
    if (!found) {
        query_vectors_.emplace_back(key, query_vector);
    }

    // Track per-agent key list
    auto& keys = agent_to_keys_[agent_id];
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
        keys.push_back(key);
    }
}

void SemanticCacheIndex::invalidateAgent(const std::string& agent_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = agent_to_keys_.find(agent_id);
    if (it == agent_to_keys_.end()) return;

    for (const auto& key : it->second) {
        cache_.erase(key);
        // Remove from query_vectors_ as well
        query_vectors_.erase(
            std::remove_if(query_vectors_.begin(), query_vectors_.end(),
                           [&](const auto& p) { return p.first == key; }),
            query_vectors_.end());
    }
    agent_to_keys_.erase(it);
}

void SemanticCacheIndex::cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    int64_t cutoff = now - TTL_SECONDS;

    std::vector<std::string> expired_keys;
    for (const auto& [key, entry] : cache_) {
        if (entry.timestamp < cutoff) {
            expired_keys.push_back(key);
        }
    }

    for (const auto& key : expired_keys) {
        cache_.erase(key);
        query_vectors_.erase(
            std::remove_if(query_vectors_.begin(), query_vectors_.end(),
                           [&](const auto& p) { return p.first == key; }),
            query_vectors_.end());

        // Remove key from all agent_to_keys_ lists
        for (auto& [agent, keys] : agent_to_keys_) {
            keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
        }
    }

    // Clean up empty agent entries
    for (auto it = agent_to_keys_.begin(); it != agent_to_keys_.end(); ) {
        if (it->second.empty()) {
            it = agent_to_keys_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace rag
} // namespace common
} // namespace agent_rpc
