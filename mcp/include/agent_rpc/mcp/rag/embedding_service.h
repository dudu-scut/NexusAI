/**
 * @file embedding_service.h
 * @brief Generic text embedding service (OpenAI-compatible API)
 */

#pragma once

#include "agent_rpc/common/env_loader.h"

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>

namespace agent_rpc {
namespace mcp {
namespace rag {

/**
 * @brief Embedding service configuration
 */
struct EmbeddingConfig {
    std::string api_key;                          ///< API key (Bearer Token authentication)
    std::string model = agent_rpc::common::envOrDefault("EMBEDDING_MODEL", agent_rpc::common::envOrDefault("LLM_MODEL", "deepseek-v4-pro"));
    int dimension = 1024;                         ///< Embedding dimension
    int max_retries = 3;                          ///< Maximum retry count
    int timeout_ms = 10000;                       ///< Timeout (ms)
    int initial_retry_delay_ms = 1000;            ///< Initial retry delay (ms)
    std::string api_url = agent_rpc::common::envOrDefault("EMBEDDING_API_URL", "https://api.deepseek.com/v1/embeddings");

    /**
     * @brief Load API key from environment variables
     * @return true if the key was loaded successfully
     */
    bool loadApiKeyFromEnv();

    /**
     * @brief Validate the configuration
     * @return true if the configuration is valid
     */
    bool validate() const;
};

/**
 * @brief Retry statistics
 */
struct RetryStats {
    int total_attempts = 0;
    int successful_attempts = 0;
    int failed_attempts = 0;
    std::vector<int> retry_delays_ms;  ///< Delay for each retry
};

/**
 * @brief Embedding service class
 *
 * Calls the OpenAI-compatible Embedding API to generate text embeddings.
 * Supports:
 * - Single-text embedding
 * - Batch embedding
 * - Exponential backoff retry
 * - Timeout handling
 */
class EmbeddingService {
public:
    explicit EmbeddingService(const EmbeddingConfig& config);
    ~EmbeddingService();

    // Disable copy
    EmbeddingService(const EmbeddingService&) = delete;
    EmbeddingService& operator=(const EmbeddingService&) = delete;

    /**
     * @brief Generate the embedding of a single text
     * @param text Input text
     * @return Embedding vector (dimension determined by configuration)
     * @throws std::runtime_error if the API call fails
     */
    std::vector<float> embed(const std::string& text);

    /**
     * @brief Generate embeddings in batch
     * @param texts List of input texts
     * @return List of embeddings
     * @throws std::runtime_error if the API call fails
     */
    std::vector<std::vector<float>> embedBatch(const std::vector<std::string>& texts);

    /**
     * @brief Compute the cosine similarity of two vectors
     * @param a Vector a
     * @param b Vector b
     * @return Similarity in [-1, 1]
     */
    static float cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b);

    /**
     * @brief Get the configuration
     */
    const EmbeddingConfig& getConfig() const { return config_; }

    /**
     * @brief Get the retry statistics of the last call
     */
    const RetryStats& getLastRetryStats() const { return last_retry_stats_; }

    /**
     * @brief Set the retry callback (for testing)
     */
    using RetryCallback = std::function<void(int attempt, int delay_ms)>;
    void setRetryCallback(RetryCallback callback) { retry_callback_ = callback; }

private:
    /**
     * @brief Send an HTTP POST request
     */
    std::string sendPostRequest(const std::string& data);

    /**
     * @brief API call with retry
     */
    std::string callApiWithRetry(const std::string& request_body);

    /**
     * @brief Compute the exponential backoff delay
     */
    int calculateBackoffDelay(int attempt) const;

    /**
     * @brief Parse the API response (OpenAI format)
     */
    std::vector<std::vector<float>> parseEmbeddingResponse(const std::string& response);

    EmbeddingConfig config_;
    RetryStats last_retry_stats_;
    RetryCallback retry_callback_;
};

} // namespace rag
} // namespace mcp
} // namespace agent_rpc
