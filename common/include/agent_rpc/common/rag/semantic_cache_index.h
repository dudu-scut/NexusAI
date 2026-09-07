#pragma once
#include "agent_rpc/common/rag/vector_index.h"
#include "agent_rpc/common/rag/embedding_service.h"
#include <string>
#include <optional>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <utility>

namespace agent_rpc {
namespace common {
namespace rag {

/**
 * @brief A cached agent response, keyed by query embedding similarity.
 */
struct CachedResponse {
    std::string agent_response;
    std::string agent_id;
    int64_t timestamp;
    int hit_count = 0;
};

/**
 * @brief Semantic cache for agent responses.
 *
 * Stores query vectors alongside cached responses.  Lookup performs
 * brute-force cosine-similarity search across stored vectors and returns
 * the cached response when similarity exceeds SIMILARITY_THRESHOLD (0.92).
 */
class SemanticCacheIndex {
public:
    explicit SemanticCacheIndex(EmbeddingService* embedding);

    /// Look up a cached response by query vector.  Returns nullopt on miss.
    std::optional<CachedResponse> lookup(const std::vector<float>& query_vector);

    /// Store a query–response pair.  Only stores when is_cacheable is true.
    void store(const std::vector<float>& query_vector,
               const std::string& agent_id,
               const std::string& response,
               bool is_cacheable);

    /// Invalidate all cache entries belonging to a specific agent.
    void invalidateAgent(const std::string& agent_id);

    /// Remove all expired entries (older than TTL_SECONDS).
    void cleanup();

private:
    static std::string vectorToKey(const std::vector<float>& v);

    EmbeddingService* embedding_;

    /// cache_key -> cached response
    std::unordered_map<std::string, CachedResponse> cache_;

    /// cache_key -> query vector (for similarity search)
    std::vector<std::pair<std::string, std::vector<float>>> query_vectors_;

    /// agent_id -> list of cache_keys belonging to that agent
    std::unordered_map<std::string, std::vector<std::string>> agent_to_keys_;

    mutable std::mutex mutex_;

    static constexpr double SIMILARITY_THRESHOLD = 0.92;
    static constexpr int64_t TTL_SECONDS = 86400; // 24h default
};

} // namespace rag
} // namespace common
} // namespace agent_rpc
