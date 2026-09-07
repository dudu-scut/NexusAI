/**
 * @file vector_index.h
 * @brief In-memory vector index with fast similarity search
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cstdint>

namespace agent_rpc {
namespace common {
namespace rag {

/**
 * @brief Tool record stored in the index
 */
struct IndexedTool {
    std::string name;                 ///< Tool name
    std::string description;          ///< Tool description
    std::string input_schema;         ///< Input parameter JSON Schema
    std::vector<float> embedding;     ///< Embedding vector
    int64_t created_at = 0;           ///< Creation timestamp
    int64_t updated_at = 0;           ///< Update timestamp
};

/**
 * @brief Search result
 */
struct SearchResult {
    IndexedTool tool;                 ///< Tool info
    float similarity;                 ///< Similarity score [0, 1]
};

/**
 * @brief Vector index class
 * 
 * In-memory vector index supporting:
 * - CRUD for tools
 * - Top-K search based on cosine similarity
 * - Persistence to JSON files
 * - Thread safety
 */
class VectorIndex {
public:
    VectorIndex() = default;
    ~VectorIndex() = default;
    
    // Disable copy
    VectorIndex(const VectorIndex&) = delete;
    VectorIndex& operator=(const VectorIndex&) = delete;
    
    // Tool management
    
    /**
     * @brief Add a tool to the index
     * @param tool Tool info (must include the embedding)
     */
    void addTool(const IndexedTool& tool);
    
    /**
     * @brief Remove a tool from the index
     * @param tool_name Tool name
     * @return true if the tool existed and was removed
     */
    bool removeTool(const std::string& tool_name);
    
    /**
     * @brief Update tool info
     * @param tool New tool info
     * @return true if the tool existed and was updated
     */
    bool updateTool(const IndexedTool& tool);
    
    /**
     * @brief Get tool info
     * @param tool_name Tool name
     * @return Tool info, or nullptr if it does not exist
     */
    const IndexedTool* getTool(const std::string& tool_name) const;
    
    /**
     * @brief Check whether a tool exists
     */
    bool hasTool(const std::string& tool_name) const;
    
    /**
     * @brief Get all tools
     */
    std::vector<IndexedTool> getAllTools() const;
    
    // Search
    
    /**
     * @brief Search the top-K most similar tools
     * @param query_embedding Query embedding
     * @param top_k Number of results to return
     * @param threshold Similarity threshold (0-1); results below it are filtered out
     * @return Search results sorted by similarity in descending order
     */
    std::vector<SearchResult> search(
        const std::vector<float>& query_embedding,
        int top_k,
        float threshold = 0.0f) const;
    
    /**
     * @brief Compute cosine similarity
     */
    static float cosineSimilarity(
        const std::vector<float>& a,
        const std::vector<float>& b);
    
    // Persistence
    
    /**
     * @brief Save the index to a JSON file
     * @param path File path
     * @return true if the save succeeded
     */
    bool saveToFile(const std::string& path) const;
    
    /**
     * @brief Load the index from a JSON file
     * @param path File path
     * @return true if the load succeeded
     */
    bool loadFromFile(const std::string& path);
    
    // State
    
    /**
     * @brief Get the index size
     */
    size_t size() const;
    
    /**
     * @brief Clear the index
     */
    void clear();
    
    /**
     * @brief Get the index version
     */
    std::string getVersion() const { return version_; }
    
    /**
     * @brief Set the index version
     */
    void setVersion(const std::string& version) { version_ = version; }

private:
    std::unordered_map<std::string, IndexedTool> tools_;
    mutable std::mutex mutex_;
    std::string version_ = "1.0";
};

} // namespace rag
} // namespace common
} // namespace agent_rpc
