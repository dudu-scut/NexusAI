/**
 * @file mcp_agent_integration.h
 * @brief MCP Agent Integration - simplifies integrating AI agents with MCP
 */

#pragma once

#include "agent_rpc/common/env_loader.h"

#include "mcp_client.h"
#include "agent_rpc/common/logger.h"

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>

// Forward declarations
namespace agent_rpc {
namespace mcp {
namespace rag {
    class ToolRetriever;
    struct RetrieverConfig;
    struct RetrievedTool;
}
}
}

namespace agent_rpc {
namespace mcp {

/**
 * @brief RAG-MCP configuration
 */
struct RAGConfig {
    bool enabled = false;                  ///< Whether RAG-MCP is enabled
    std::string api_key;                   ///< API key (read from LLM_API_KEY env var if empty)
    std::string model = agent_rpc::common::envOrDefault("EMBEDDING_MODEL", agent_rpc::common::envOrDefault("LLM_MODEL", "deepseek-v4-pro"));
    int top_k = agent_rpc::common::envOrInt("RAG_TOP_K", 5);
    float similarity_threshold = agent_rpc::common::envOrFloat("RAG_THRESHOLD", 0.3f);
    std::string index_path;                ///< Index file path
    bool enable_cache = true;              ///< Whether caching is enabled
    size_t cache_max_size = 1000;          ///< Maximum cache entries
    int cache_ttl_seconds = 3600;          ///< Cache TTL in seconds
};

/**
 * @brief MCP agent integration configuration
 */
struct MCPAgentConfig {
    std::string mcp_server_path;           ///< MCP server executable path
    std::vector<std::string> mcp_args;     ///< MCP server startup arguments
    bool enable_mcp = false;               ///< Whether MCP is enabled
    int connection_timeout_ms = 5000;      ///< Connection timeout (ms)
    int tool_call_timeout_ms = 30000;      ///< Tool call timeout (ms)
    int max_retry_count = 3;               ///< Maximum retry count
    int retry_delay_ms = 1000;             ///< Retry delay (ms)
    
    // RAG-MCP configuration
    RAGConfig rag_config;                  ///< RAG-MCP configuration
};

/**
 * @brief Tool call result
 */
struct ToolCallResult {
    bool success = false;                  ///< Whether the call succeeded
    std::string result;                    ///< Result on success
    std::string error;                     ///< Error message on failure
    int64_t duration_ms = 0;               ///< Call duration (ms)
};

/**
 * @brief Tool information
 */
struct ToolInfo {
    std::string name;                      ///< Tool name
    std::string description;               ///< Tool description
    std::string input_schema;              ///< Input parameters JSON Schema
};

/**
 * @brief MCP agent integration helper class
 *
 * Wraps MCPClient and MCPToolManager to provide a simplified AI agent
 * integration interface. Supports:
 * - Automatic connect/disconnect of the MCP server
 * - Tool discovery and invocation
 * - Error handling and graceful degradation
 * - Asynchronous tool invocation
 */
class MCPAgentIntegration {
public:
    MCPAgentIntegration();
    ~MCPAgentIntegration();
    
    // Disable copy
    MCPAgentIntegration(const MCPAgentIntegration&) = delete;
    MCPAgentIntegration& operator=(const MCPAgentIntegration&) = delete;
    
    /**
     * @brief Initialize MCP integration
     * @param config MCP configuration
     * @return true if initialization succeeded or MCP is disabled
     *
     * If config.enable_mcp is false, returns true without connecting to the
     * MCP server. If connecting fails, logs an error but still returns true
     * (degraded mode).
     */
    bool initialize(const MCPAgentConfig& config);
    
    /**
     * @brief Shut down MCP integration
     *
     * Disconnects from the MCP server and releases resources.
     */
    void shutdown();
    
    /**
     * @brief Check whether MCP is initialized
     */
    bool isInitialized() const { return initialized_; }
    
    /**
     * @brief Check whether MCP is available (connected and tool manager ready)
     */
    bool isAvailable() const;
    
    /**
     * @brief Get the list of available tools
     * @return List of tool information
     */
    std::vector<ToolInfo> getAvailableTools() const;
    
    /**
     * @brief Get the list of available tool names
     * @return List of tool names
     */
    std::vector<std::string> getToolNames() const;
    
    /**
     * @brief Check whether a tool is available
     * @param tool_name Tool name
     * @return true if the tool is available
     */
    bool hasToolAvailable(const std::string& tool_name) const;
    
    /**
     * @brief Get a tool description
     * @param tool_name Tool name
     * @return Tool description, or an empty string if the tool does not exist
     */
    std::string getToolDescription(const std::string& tool_name) const;
    
    /**
     * @brief Get a tool input schema
     * @param tool_name Tool name
     * @return JSON Schema string, or an empty string if the tool does not exist
     */
    std::string getToolInputSchema(const std::string& tool_name) const;
    
    /**
     * @brief Invoke an MCP tool synchronously
     * @param tool_name Tool name
     * @param arguments JSON-formatted arguments
     * @return Tool call result
     *
     * Returns a failed result without throwing if MCP is unavailable.
     */
    ToolCallResult callTool(const std::string& tool_name, 
                            const std::string& arguments);
    
    /**
     * @brief Invoke an MCP tool asynchronously
     * @param tool_name Tool name
     * @param arguments JSON-formatted arguments
     * @param callback Completion callback
     *
     * The callback is invoked after the tool call completes, possibly on a
     * different thread.
     */
    void callToolAsync(const std::string& tool_name,
                       const std::string& arguments,
                       std::function<void(const ToolCallResult&)> callback);
    
    /**
     * @brief Simplified tool invocation (returns the result string)
     * @param tool_name Tool name
     * @param arguments JSON-formatted arguments
     * @return Result on success, or an error message prefixed with [ERROR]
     */
    std::string callToolSimple(const std::string& tool_name,
                               const std::string& arguments);
    
    /**
     * @brief Get the current configuration
     */
    const MCPAgentConfig& getConfig() const { return config_; }
    
    /**
     * @brief Get the MCP server path
     */
    const std::string& getMCPServerPath() const { return config_.mcp_server_path; }
    
    /**
     * @brief Get the connection status description
     */
    std::string getStatusDescription() const;
    
    /**
     * @brief Refresh the tool list
     * @return true if the refresh succeeded
     */
    bool refreshTools();
    
    /**
     * @brief Check whether RAG-MCP is enabled
     */
    bool isRAGEnabled() const;
    
    /**
     * @brief Retrieve relevant tools for a query
     * @param query User query
     * @return Relevant tool list (sorted by relevance)
     *
     * Returns all tools if RAG is not enabled.
     */
    std::vector<ToolInfo> getRelevantTools(const std::string& query) const;
    
    /**
     * @brief Retrieve relevant tools for a query (custom top_k)
     */
    std::vector<ToolInfo> getRelevantTools(const std::string& query, int top_k) const;
    
    /**
     * @brief Get tools in LLM function-calling format
     * @param tools Tool list
     * @return JSON-formatted function definitions
     */
    static std::string toFunctionCallingFormat(const std::vector<ToolInfo>& tools);
    
    /**
     * @brief Get relevant tools in LLM function-calling format
     * @param query User query
     * @return JSON-formatted function definitions
     */
    std::string getRelevantToolsAsJson(const std::string& query) const;

private:
    // Internal methods
    bool connectToMCPServer();
    void disconnectFromMCPServer();
    void updateToolCache();
    bool initializeRAG();
    void shutdownRAG();
    
    // Member variables
    MCPAgentConfig config_;
    std::shared_ptr<MCPClient> mcp_client_;
    std::shared_ptr<MCPToolManager> tool_manager_;
    
    // Tool cache
    std::vector<ToolInfo> tool_cache_;
    mutable std::mutex tool_cache_mutex_;
    std::shared_ptr<std::atomic<bool>> alive_flag_;  // Prevents use-after-free in async calls
    
    // RAG-MCP
    std::unique_ptr<rag::ToolRetriever> tool_retriever_;
    
    // State
    std::atomic<bool> initialized_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> rag_initialized_{false};
};

/**
 * @brief Parse MCP configuration from command-line arguments
 * @param argc Argument count
 * @param argv Argument array
 * @return MCP configuration
 *
 * Supported arguments:
 * --mcp-server <path>    MCP server executable path
 * --mcp-args <args>      MCP server startup arguments (comma-separated)
 * --enable-mcp           Enable MCP
 * --mcp-timeout <ms>     Tool call timeout (ms)
 */
MCPAgentConfig parseMCPConfigFromArgs(int argc, char* argv[]);

/**
 * @brief Parse MCP configuration from environment variables
 * @return MCP configuration
 *
 * Supported environment variables:
 * MCP_SERVER_PATH        MCP server executable path
 * MCP_SERVER_ARGS        MCP server startup arguments (comma-separated)
 * MCP_ENABLED             Whether MCP is enabled (true/false)
 * MCP_TIMEOUT_MS         Tool call timeout (ms)
 */
MCPAgentConfig parseMCPConfigFromEnv();

} // namespace mcp
} // namespace agent_rpc
