/**
 * @file tool_retriever.h
 * @brief Tool retriever integrating EmbeddingService and VectorIndex
 */

#pragma once

#include "agent_rpc/common/rag/embedding_service.h"
#include "agent_rpc/common/rag/embedding_cache.h"
#include "agent_rpc/common/rag/vector_index.h"
#include "../mcp_agent_integration.h"

#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace agent_rpc {
namespace mcp {
namespace rag {

// P7 (批次十一): the vector building blocks moved to agent_rpc::common::rag;
// this MCP-side retriever bridges the namespaces so its call sites stay
// unchanged.
using agent_rpc::common::rag::EmbeddingService;
using agent_rpc::common::rag::EmbeddingCache;
using agent_rpc::common::rag::VectorIndex;
using agent_rpc::common::rag::EmbeddingConfig;
using agent_rpc::common::rag::CacheConfig;
using agent_rpc::common::rag::CacheStats;
using agent_rpc::common::rag::IndexedTool;

/**
 * @brief Retriever configuration
 */
struct RetrieverConfig {
    EmbeddingConfig embedding_config;     ///< Embedding service configuration
    CacheConfig cache_config;             ///< Cache configuration
    int top_k = 5;                        ///< Number of tools to return
    float similarity_threshold = 0.3f;    ///< Similarity threshold
    bool enable_validation = false;       ///< Whether validation is enabled
    int validation_timeout_ms = 5000;     ///< Validation timeout (ms)
    std::string index_path;               ///< Index file path
    bool auto_save_index = true;          ///< Whether to auto-save the index
};

/**
 * @brief Retrieved tool
 */
struct RetrievedTool {
    std::string name;                     ///< Tool name
    std::string description;              ///< Tool description
    std::string input_schema;             ///< Input parameter JSON Schema
    float relevance_score;                ///< Relevance score
};

/**
 * @brief Tool retriever
 * 
 * Integrates EmbeddingService, EmbeddingCache, and VectorIndex to provide
 * complete tool retrieval.
 * 
 * Workflow:
 * 1. Receive the user query
 * 2. Check whether the cache has the query embedding
 * 3. If not, call EmbeddingService to generate the embedding
 * 4. Search the VectorIndex for the most similar tools
 * 5. Optionally validate tool compatibility
 * 6. Return the retrieval results
 */
class ToolRetriever {
public:
    explicit ToolRetriever(const RetrieverConfig& config);
    ~ToolRetriever();
    
    // Disable copy
    ToolRetriever(const ToolRetriever&) = delete;
    ToolRetriever& operator=(const ToolRetriever&) = delete;
    
    // Lifecycle management
    
    /**
     * @brief Initialize the retriever
     * @return true if initialization succeeded
     * 
     * Tries to load the index from index_path; creates an empty index if the
     * file does not exist.
     */
    bool initialize();
    
    /**
     * @brief Shut down the retriever
     * 
     * Saves the index to a file if auto_save_index is true.
     */
    void shutdown();
    
    /**
     * @brief Check whether initialized
     */
    bool isInitialized() const { return initialized_; }
    
    // Tool indexing
    
    /**
     * @brief Index MCP tools
     * @param tools Tool list
     * 
     * Generates an embedding for each tool and adds it to the index.
     */
    void indexTools(const std::vector<ToolInfo>& tools);
    
    /**
     * @brief Add a single tool to the index
     */
    void addTool(const ToolInfo& tool);
    
    /**
     * @brief Remove a tool from the index
     */
    bool removeTool(const std::string& tool_name);
    
    /**
     * @brief Refresh the index (regenerate all embeddings)
     */
    void refreshIndex();
    
    /**
     * @brief Save the index to a file
     */
    bool saveIndex();
    
    /**
     * @brief Load the index from a file
     */
    bool loadIndex();
    
    // Tool retrieval
    
    /**
     * @brief Retrieve relevant tools
     * @param query User query
     * @return Retrieved tools sorted by relevance in descending order
     */
    std::vector<RetrievedTool> retrieve(const std::string& query);
    
    /**
     * @brief Retrieve relevant tools (custom top_k)
     */
    std::vector<RetrievedTool> retrieve(const std::string& query, int top_k);
    
    /**
     * @brief Get all tools (without retrieval)
     */
    std::vector<RetrievedTool> getAllTools() const;
    
    // Format conversion
    
    /**
     * @brief Convert retrieval results to LLM function-calling format
     * @param tools Retrieved tools
     * @return JSON-formatted function definitions
     */
    static std::string toFunctionCallingFormat(const std::vector<RetrievedTool>& tools);
    
    // State and configuration
    
    /**
     * @brief Get the index size
     */
    size_t getIndexSize() const;
    
    /**
     * @brief Get cache statistics
     */
    CacheStats getCacheStats() const;
    
    /**
     * @brief Get the configuration
     */
    const RetrieverConfig& getConfig() const { return config_; }

private:
    /**
     * @brief Get the embedding of text (with cache)
     */
    std::vector<float> getEmbedding(const std::string& text);
    
    /**
     * @brief Build the textual representation of a tool (for embedding)
     */
    static std::string buildToolText(const ToolInfo& tool);
    
    RetrieverConfig config_;
    std::unique_ptr<EmbeddingService> embedding_service_;
    std::unique_ptr<EmbeddingCache> cache_;
    std::unique_ptr<VectorIndex> index_;
    bool initialized_ = false;
};

/**
 * @brief RAG-MCP integration configuration
 */
struct RAGMCPConfig {
    bool enabled = false;                 ///< Whether RAG-MCP is enabled
    RetrieverConfig retriever_config;     ///< Retriever configuration
};

} // namespace rag
} // namespace mcp
} // namespace agent_rpc
