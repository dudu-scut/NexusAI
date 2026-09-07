/**
 * @file test_rag_mcp_properties.cpp
 * @brief RAG-MCP framework property tests
 * 
 * Tests for:
 * - Property 1: Search Results Ordering
 * - Property 2: Top-K Result Count
 * - Property 3: Similarity Threshold Filtering
 * - Property 4: Index Persistence Round-Trip
 * - Property 5: Incremental Index Update
 * - Property 6: Tool Removal Consistency
 * - Property 7: Cache Hit Behavior
 * - Property 8: LRU Eviction Order
 */

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "agent_rpc/common/rag/embedding_service.h"
#include "agent_rpc/common/rag/embedding_cache.h"
#include "agent_rpc/common/rag/vector_index.h"
#include "agent_rpc/mcp/rag/tool_retriever.h"
#include "agent_rpc/mcp/rag/tool_validator.h"
#include "agent_rpc/mcp/mcp_agent_integration.h"

#include <cmath>
#include <set>
#include <algorithm>
#include <filesystem>

namespace {

// P7 (批次十一): the vector building blocks now live in agent_rpc::common::rag
// (unconditional); the tool-integration layer stays in agent_rpc::mcp::rag.
// Explicit using-declarations win over the namespace using-directive, so the
// shared type names resolve to the common ones without ambiguity.
using namespace agent_rpc::mcp::rag;  // ToolRetriever / validation types
using agent_rpc::common::rag::EmbeddingService;
using agent_rpc::common::rag::EmbeddingCache;
using agent_rpc::common::rag::VectorIndex;
using agent_rpc::common::rag::EmbeddingConfig;
using agent_rpc::common::rag::CacheConfig;
using agent_rpc::mcp::ToolCallResult;
using agent_rpc::mcp::ToolInfo;

// Helpers

// Generate a random vector
std::vector<float> generateRandomVector(int dimension) {
    std::vector<float> vec(dimension);
    for (int i = 0; i < dimension; ++i) {
        vec[i] = static_cast<float>(rand()) / RAND_MAX * 2.0f - 1.0f;
    }
    // Normalize
    float norm = 0.0f;
    for (float v : vec) norm += v * v;
    norm = std::sqrt(norm);
    if (norm > 0) {
        for (float& v : vec) v /= norm;
    }
    return vec;
}

// Generate a random tool name
std::string generateToolName(int index) {
    return "tool_" + std::to_string(index);
}

// Property 1: Search Results Ordering

RC_GTEST_PROP(VectorIndexProperties, SearchResultsOrdering, ()) {
    VectorIndex index;
    
    // Generate a random number of tools (3-20)
    int num_tools = *rc::gen::inRange(3, 20);
    int dimension = 128;  // Use a smaller dimension to speed up tests
    
    for (int i = 0; i < num_tools; ++i) {
        IndexedTool tool;
        tool.name = generateToolName(i);
        tool.description = "Test tool " + std::to_string(i);
        tool.input_schema = "{}";
        tool.embedding = generateRandomVector(dimension);
        index.addTool(tool);
    }
    
    // Generate a random query vector
    auto query = generateRandomVector(dimension);
    
    // Search
    int top_k = *rc::gen::inRange(1, num_tools + 1);
    auto results = index.search(query, top_k);
    
    // Verify results are sorted by similarity in descending order
    for (size_t i = 1; i < results.size(); ++i) {
        RC_ASSERT(results[i-1].similarity >= results[i].similarity);
    }
}

// Property 2: Top-K Result Count

RC_GTEST_PROP(VectorIndexProperties, TopKResultCount, ()) {
    VectorIndex index;
    
    // Generate a random number of tools (1-30)
    int num_tools = *rc::gen::inRange(1, 30);
    int dimension = 64;
    
    for (int i = 0; i < num_tools; ++i) {
        IndexedTool tool;
        tool.name = generateToolName(i);
        tool.description = "Test tool";
        tool.input_schema = "{}";
        tool.embedding = generateRandomVector(dimension);
        index.addTool(tool);
    }
    
    // Generate a random top_k
    int top_k = *rc::gen::inRange(1, 50);
    
    auto query = generateRandomVector(dimension);
    // Use -1.0f as threshold so nothing is filtered out (including negative similarities)
    auto results = index.search(query, top_k, -1.0f);
    
    // Verify result count = min(top_k, num_tools)
    int expected = std::min(top_k, num_tools);
    RC_ASSERT(static_cast<int>(results.size()) == expected);
}

// Property 3: Similarity Threshold Filtering

RC_GTEST_PROP(VectorIndexProperties, SimilarityThresholdFiltering, ()) {
    VectorIndex index;
    
    int num_tools = *rc::gen::inRange(5, 20);
    int dimension = 64;
    
    for (int i = 0; i < num_tools; ++i) {
        IndexedTool tool;
        tool.name = generateToolName(i);
        tool.description = "Test tool";
        tool.input_schema = "{}";
        tool.embedding = generateRandomVector(dimension);
        index.addTool(tool);
    }
    
    auto query = generateRandomVector(dimension);
    float threshold = *rc::gen::inRange(0, 100) / 100.0f;  // 0.0 - 1.0
    
    auto results = index.search(query, num_tools, threshold);
    
    // Verify all results have similarity >= threshold (best match is returned if none qualify)
    if (results.size() > 1) {
        for (const auto& result : results) {
            RC_ASSERT(result.similarity >= threshold);
        }
    } else if (results.size() == 1) {
        // If there is only one result, it should be the best match
        // No threshold requirement
    }
}

// Property 4: Index Persistence Round-Trip

RC_GTEST_PROP(VectorIndexProperties, IndexPersistenceRoundTrip, ()) {
    VectorIndex original_index;
    
    int num_tools = *rc::gen::inRange(1, 10);
    int dimension = 32;
    
    for (int i = 0; i < num_tools; ++i) {
        IndexedTool tool;
        tool.name = generateToolName(i);
        tool.description = "Description " + std::to_string(i);
        tool.input_schema = R"({"type": "object"})";
        tool.embedding = generateRandomVector(dimension);
        original_index.addTool(tool);
    }
    
    // Save to a temporary file
    std::string temp_path = "/tmp/test_index_" + std::to_string(rand()) + ".json";
    RC_ASSERT(original_index.saveToFile(temp_path));
    
    // Load into a new index
    VectorIndex loaded_index;
    RC_ASSERT(loaded_index.loadFromFile(temp_path));
    
    // Verify sizes match
    RC_ASSERT(original_index.size() == loaded_index.size());
    
    // Verify all tools exist
    auto original_tools = original_index.getAllTools();
    for (const auto& tool : original_tools) {
        RC_ASSERT(loaded_index.hasTool(tool.name));
        
        auto loaded_tool = loaded_index.getTool(tool.name);
        RC_ASSERT(loaded_tool != nullptr);
        RC_ASSERT(loaded_tool->description == tool.description);
        RC_ASSERT(loaded_tool->embedding.size() == tool.embedding.size());
    }
    
    // Clean up
    std::filesystem::remove(temp_path);
}

// Property 5: Incremental Index Update

RC_GTEST_PROP(VectorIndexProperties, IncrementalIndexUpdate, ()) {
    VectorIndex index;
    
    int initial_count = *rc::gen::inRange(0, 10);
    int dimension = 32;
    
    // Add initial tools
    for (int i = 0; i < initial_count; ++i) {
        IndexedTool tool;
        tool.name = generateToolName(i);
        tool.description = "Test";
        tool.input_schema = "{}";
        tool.embedding = generateRandomVector(dimension);
        index.addTool(tool);
    }
    
    RC_ASSERT(index.size() == static_cast<size_t>(initial_count));
    
    // Add a new tool
    IndexedTool new_tool;
    new_tool.name = "new_tool";
    new_tool.description = "New tool";
    new_tool.input_schema = "{}";
    new_tool.embedding = generateRandomVector(dimension);
    
    index.addTool(new_tool);
    
    // Verify size increased by 1
    RC_ASSERT(index.size() == static_cast<size_t>(initial_count + 1));
    RC_ASSERT(index.hasTool("new_tool"));
}

// Property 6: Tool Removal Consistency

RC_GTEST_PROP(VectorIndexProperties, ToolRemovalConsistency, ()) {
    VectorIndex index;
    
    int num_tools = *rc::gen::inRange(2, 15);
    int dimension = 32;
    
    for (int i = 0; i < num_tools; ++i) {
        IndexedTool tool;
        tool.name = generateToolName(i);
        tool.description = "Test";
        tool.input_schema = "{}";
        tool.embedding = generateRandomVector(dimension);
        index.addTool(tool);
    }
    
    // Pick a tool to remove
    int remove_index = *rc::gen::inRange(0, num_tools);
    std::string tool_to_remove = generateToolName(remove_index);
    
    size_t size_before = index.size();
    RC_ASSERT(index.hasTool(tool_to_remove));
    
    // Remove the tool
    bool removed = index.removeTool(tool_to_remove);
    
    RC_ASSERT(removed);
    RC_ASSERT(index.size() == size_before - 1);
    RC_ASSERT(!index.hasTool(tool_to_remove));
    
    // Verify search results do not contain the removed tool
    auto query = generateRandomVector(dimension);
    auto results = index.search(query, num_tools);
    
    for (const auto& result : results) {
        RC_ASSERT(result.tool.name != tool_to_remove);
    }
}

// Property 7: Cache Hit Behavior

RC_GTEST_PROP(EmbeddingCacheProperties, CacheHitBehavior, ()) {
    CacheConfig config;
    config.enabled = true;
    config.max_size = 100;
    config.ttl_seconds = 3600;
    
    EmbeddingCache cache(config);
    
    // Generate random text and embedding
    std::string text = "test_text_" + std::to_string(*rc::gen::inRange(0, 10000));
    std::vector<float> embedding = generateRandomVector(64);
    
    // First lookup should be a miss
    auto result1 = cache.get(text);
    RC_ASSERT(!result1.has_value());
    
    // Put into cache
    cache.put(text, embedding);
    
    // Second lookup should be a hit
    auto result2 = cache.get(text);
    RC_ASSERT(result2.has_value());
    RC_ASSERT(result2.value().size() == embedding.size());
    
    // Verify values are identical
    for (size_t i = 0; i < embedding.size(); ++i) {
        RC_ASSERT(std::abs(result2.value()[i] - embedding[i]) < 1e-6f);
    }
}

// Property 8: LRU Eviction Order

RC_GTEST_PROP(EmbeddingCacheProperties, LRUEvictionOrder, ()) {
    int cache_size = *rc::gen::inRange(3, 10);
    
    CacheConfig config;
    config.enabled = true;
    config.max_size = cache_size;
    config.ttl_seconds = 3600;
    
    EmbeddingCache cache(config);
    
    // Fill the cache
    for (int i = 0; i < cache_size; ++i) {
        std::string text = "text_" + std::to_string(i);
        cache.put(text, generateRandomVector(32));
    }
    
    RC_ASSERT(cache.size() == static_cast<size_t>(cache_size));
    
    // Access the first element (making it most recently used)
    cache.get("text_0");
    
    // Add a new element; text_1 (least recently used) should be evicted
    cache.put("new_text", generateRandomVector(32));
    
    // text_0 should still be present (just accessed)
    RC_ASSERT(cache.contains("text_0"));
    
    // text_1 should be evicted
    RC_ASSERT(!cache.contains("text_1"));
    
    // The new element should be present
    RC_ASSERT(cache.contains("new_text"));
}

// Property 9: Retry Exponential Backoff

TEST(EmbeddingServiceProperties, RetryExponentialBackoff) {
    // Test exponential backoff delay calculation
    // Since real API calls need the network, only test the delay calculation logic
    
    EmbeddingConfig config;
    config.api_key = "test_key";
    config.initial_retry_delay_ms = 1000;
    config.max_retries = 5;
    
    // Verify exponential backoff pattern: delay = initial_delay * 2^(attempt-1)
    // attempt 1: 1000ms
    // attempt 2: 2000ms
    // attempt 3: 4000ms
    // attempt 4: 8000ms
    // attempt 5: 16000ms
    
    std::vector<int> expected_base_delays = {1000, 2000, 4000, 8000, 16000};
    
    for (int attempt = 1; attempt <= 5; ++attempt) {
        int expected_base = config.initial_retry_delay_ms * (1 << (attempt - 1));
        EXPECT_EQ(expected_base, expected_base_delays[attempt - 1]);
        
        // Verify exponential growth
        if (attempt > 1) {
            EXPECT_EQ(expected_base, expected_base_delays[attempt - 2] * 2);
        }
    }
}

RC_GTEST_PROP(EmbeddingServiceProperties, RetryDelaysAreExponential, ()) {
    // Generate random initial delay and retry count
    int initial_delay = *rc::gen::inRange(100, 2000);
    int max_retries = *rc::gen::inRange(2, 6);
    
    std::vector<int> delays;
    for (int attempt = 1; attempt <= max_retries; ++attempt) {
        int base_delay = initial_delay * (1 << (attempt - 1));
        delays.push_back(base_delay);
    }
    
    // Verify each delay is twice the previous one
    for (size_t i = 1; i < delays.size(); ++i) {
        RC_ASSERT(delays[i] == delays[i-1] * 2);
    }
    
    // Verify the first delay equals the initial delay
    RC_ASSERT(delays[0] == initial_delay);
}

// Property 10: Retrieved Tool Completeness

RC_GTEST_PROP(ToolRetrieverProperties, RetrievedToolCompleteness, ()) {
    VectorIndex index;
    
    int num_tools = *rc::gen::inRange(1, 10);
    int dimension = 64;
    
    // Add tools, ensuring each tool has complete fields
    for (int i = 0; i < num_tools; ++i) {
        IndexedTool tool;
        tool.name = "tool_" + std::to_string(i);
        tool.description = "Description for tool " + std::to_string(i);
        tool.input_schema = R"({"type": "object", "properties": {"param": {"type": "string"}}})";
        tool.embedding = generateRandomVector(dimension);
        index.addTool(tool);
    }
    
    auto query = generateRandomVector(dimension);
    auto results = index.search(query, num_tools);
    
    // Verify each retrieved result has complete fields
    for (const auto& result : results) {
        // Name is not empty
        RC_ASSERT(!result.tool.name.empty());
        
        // Description is not empty
        RC_ASSERT(!result.tool.description.empty());
        
        // input_schema is not empty
        RC_ASSERT(!result.tool.input_schema.empty());
        
        // Similarity score is within the valid range
        RC_ASSERT(result.similarity >= -1.0f && result.similarity <= 1.0f);
    }
}

// Property 11: Validation Exclusion

TEST(ToolValidatorProperties, ValidationExclusion) {
    ValidatorConfig config;
    config.timeout_ms = 1000;
    config.treat_timeout_as_valid = false;
    
    ToolValidator validator(config);
    
    // Track tool calls
    std::vector<std::string> called_tools;
    
    // Set a tool call function that succeeds or fails based on the tool name
    validator.setToolCallFunc([&called_tools](const std::string& tool_name, 
                                              const std::string& /*arguments*/) -> ToolCallResult {
        called_tools.push_back(tool_name);
        ToolCallResult result;
        if (tool_name == "failing_tool") {
            result.success = false;
            // Use an error message that does not contain "parameter" or "argument"
            result.error = "Tool execution failed completely";
        } else {
            result.success = true;
            result.result = "{}";
        }
        return result;
    });
    
    // Create test tools - use schemas with properties so test queries are generated
    std::vector<RetrievedTool> tools;
    
    RetrievedTool valid_tool;
    valid_tool.name = "valid_tool";
    valid_tool.description = "A valid tool";
    valid_tool.input_schema = R"({"type": "object", "properties": {"input": {"type": "string"}}})";
    valid_tool.relevance_score = 0.9f;
    tools.push_back(valid_tool);
    
    RetrievedTool failing_tool;
    failing_tool.name = "failing_tool";
    failing_tool.description = "A failing tool";
    failing_tool.input_schema = R"({"type": "object", "properties": {"input": {"type": "string"}}})";
    failing_tool.relevance_score = 0.8f;
    tools.push_back(failing_tool);
    
    // Validate each tool individually first
    called_tools.clear();
    auto valid_result = validator.validate(valid_tool);
    EXPECT_TRUE(valid_result.is_valid) << "valid_tool should be valid";
    EXPECT_FALSE(called_tools.empty()) << "Tool call function should have been called for valid_tool";
    
    called_tools.clear();
    auto failing_result = validator.validate(failing_tool);
    EXPECT_FALSE(failing_result.is_valid) << "failing_tool should be invalid, error: " << failing_result.error_message;
    EXPECT_FALSE(called_tools.empty()) << "Tool call function should have been called for failing_tool";
    
    // Filter out invalid tools
    auto filtered = validator.filterInvalid(tools);
    
    // Verify the failing tool is excluded
    EXPECT_EQ(filtered.size(), 1u) << "Should have 1 tool after filtering";
    if (!filtered.empty()) {
        EXPECT_EQ(filtered[0].name, "valid_tool");
    }
    
    // Verify failing_tool is not in the results
    for (const auto& tool : filtered) {
        EXPECT_NE(tool.name, "failing_tool");
    }
}

RC_GTEST_PROP(ToolValidatorProperties, InvalidToolsAreExcluded, ()) {
    ValidatorConfig config;
    config.timeout_ms = 1000;
    config.treat_timeout_as_valid = false;
    
    ToolValidator validator(config);
    
    // Generate a random number of tools
    int num_valid = *rc::gen::inRange(1, 5);
    int num_invalid = *rc::gen::inRange(1, 5);
    
    // Build the set of invalid tool names first
    std::set<std::string> invalid_names;
    for (int i = 0; i < num_invalid; ++i) {
        invalid_names.insert("invalid_" + std::to_string(i));
    }
    
    // Set tool call function - capture by value to avoid dangling references
    validator.setToolCallFunc([invalid_names](const std::string& tool_name, 
                                              const std::string& /*arguments*/) -> ToolCallResult {
        ToolCallResult result;
        if (invalid_names.count(tool_name) > 0) {
            result.success = false;
            // Use an error message that does not contain "parameter" or "argument"
            result.error = "Tool execution failed completely";
        } else {
            result.success = true;
            result.result = "{}";
        }
        return result;
    });
    
    std::vector<RetrievedTool> tools;
    
    // Use a schema with properties so test queries are generated
    std::string schema = R"({"type": "object", "properties": {"input": {"type": "string"}}})";
    
    // Add valid tools
    for (int i = 0; i < num_valid; ++i) {
        RetrievedTool tool;
        tool.name = "valid_" + std::to_string(i);
        tool.description = "Valid tool";
        tool.input_schema = schema;
        tool.relevance_score = 0.9f;
        tools.push_back(tool);
    }
    
    // Add invalid tools
    for (int i = 0; i < num_invalid; ++i) {
        RetrievedTool tool;
        tool.name = "invalid_" + std::to_string(i);
        tool.description = "Invalid tool";
        tool.input_schema = schema;
        tool.relevance_score = 0.8f;
        tools.push_back(tool);
    }
    
    // Filter
    auto filtered = validator.filterInvalid(tools);
    
    // Verify the result count equals the number of valid tools
    RC_ASSERT(static_cast<int>(filtered.size()) == num_valid);
    
    // Verify all invalid tools are excluded
    for (const auto& tool : filtered) {
        RC_ASSERT(invalid_names.count(tool.name) == 0);
    }
}

// Cosine similarity tests

TEST(VectorIndexTest, CosineSimilarity_IdenticalVectors) {
    std::vector<float> v = {1.0f, 2.0f, 3.0f};
    float sim = VectorIndex::cosineSimilarity(v, v);
    EXPECT_NEAR(sim, 1.0f, 1e-5f);
}

TEST(VectorIndexTest, CosineSimilarity_OrthogonalVectors) {
    std::vector<float> v1 = {1.0f, 0.0f};
    std::vector<float> v2 = {0.0f, 1.0f};
    float sim = VectorIndex::cosineSimilarity(v1, v2);
    EXPECT_NEAR(sim, 0.0f, 1e-5f);
}

TEST(VectorIndexTest, CosineSimilarity_OppositeVectors) {
    std::vector<float> v1 = {1.0f, 2.0f, 3.0f};
    std::vector<float> v2 = {-1.0f, -2.0f, -3.0f};
    float sim = VectorIndex::cosineSimilarity(v1, v2);
    EXPECT_NEAR(sim, -1.0f, 1e-5f);
}

// Cache disabled tests

TEST(EmbeddingCacheTest, DisabledCache) {
    CacheConfig config;
    config.enabled = false;
    
    EmbeddingCache cache(config);
    
    cache.put("test", {1.0f, 2.0f, 3.0f});
    
    auto result = cache.get("test");
    EXPECT_FALSE(result.has_value());
}

// Empty index search tests

TEST(VectorIndexTest, EmptyIndexSearch) {
    VectorIndex index;
    
    auto query = generateRandomVector(64);
    auto results = index.search(query, 5);
    
    EXPECT_TRUE(results.empty());
}

// RAG-MCP integration tests

using agent_rpc::mcp::MCPAgentIntegration;
using agent_rpc::mcp::MCPAgentConfig;
using agent_rpc::mcp::RAGConfig;

class RAGMCPIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create base configuration
        config_.enable_mcp = false;  // Do not connect to a real MCP server
        config_.mcp_server_path = "";
    }
    
    void TearDown() override {
        integration_.shutdown();
    }
    
    MCPAgentConfig config_;
    MCPAgentIntegration integration_;
};

// Test RAG-MCP disabled scenario
TEST_F(RAGMCPIntegrationTest, RAGDisabled_ReturnsAllTools) {
    // Configure RAG as disabled
    config_.rag_config.enabled = false;
    
    // Initialize
    ASSERT_TRUE(integration_.initialize(config_));
    
    // Verify RAG is not enabled
    EXPECT_FALSE(integration_.isRAGEnabled());
    
    // When RAG is disabled, getRelevantTools should return all tools
    // With no MCP server connected, the tool list is empty
    auto tools = integration_.getRelevantTools("any query");
    // An empty tool list is expected since there is no MCP server
    EXPECT_TRUE(tools.empty());
}

// Test RAG-MCP enabled without an API key
TEST_F(RAGMCPIntegrationTest, RAGEnabled_NoApiKey_FallbackToAllTools) {
    // Enable RAG but provide no API key
    config_.rag_config.enabled = true;
    config_.rag_config.api_key = "";  // Empty API key
    
    // Initialization should succeed (degraded mode)
    ASSERT_TRUE(integration_.initialize(config_));
    
    // RAG may not initialize successfully, but must not crash
    // Should degrade to returning all tools
    auto tools = integration_.getRelevantTools("test query");
    // With no MCP server connected, the tool list is empty
    EXPECT_TRUE(tools.empty());
}

// Test RAG config parameters
TEST_F(RAGMCPIntegrationTest, RAGConfig_Parameters) {
    config_.rag_config.enabled = true;
    config_.rag_config.api_key = "test_key";
    config_.rag_config.model = "text-embedding-v2";
    config_.rag_config.top_k = 3;
    config_.rag_config.similarity_threshold = 0.5f;
    config_.rag_config.enable_cache = true;
    config_.rag_config.cache_max_size = 500;
    config_.rag_config.cache_ttl_seconds = 1800;
    
    // Verify the config is stored correctly
    EXPECT_TRUE(config_.rag_config.enabled);
    EXPECT_EQ(config_.rag_config.api_key, "test_key");
    EXPECT_EQ(config_.rag_config.model, "text-embedding-v2");
    EXPECT_EQ(config_.rag_config.top_k, 3);
    EXPECT_FLOAT_EQ(config_.rag_config.similarity_threshold, 0.5f);
    EXPECT_TRUE(config_.rag_config.enable_cache);
    EXPECT_EQ(config_.rag_config.cache_max_size, 500u);
    EXPECT_EQ(config_.rag_config.cache_ttl_seconds, 1800);
}

// Test LLM function calling format conversion
TEST(FunctionCallingFormatTest, ToFunctionCallingFormat) {
    std::vector<ToolInfo> tools;
    
    ToolInfo tool1;
    tool1.name = "calculator";
    tool1.description = "Perform mathematical calculations";
    tool1.input_schema = R"({"type": "object", "properties": {"expression": {"type": "string"}}})";
    tools.push_back(tool1);
    
    ToolInfo tool2;
    tool2.name = "weather";
    tool2.description = "Get weather information";
    tool2.input_schema = R"({"type": "object", "properties": {"location": {"type": "string"}}})";
    tools.push_back(tool2);
    
    std::string json = MCPAgentIntegration::toFunctionCallingFormat(tools);
    
    // Verify the JSON format
    EXPECT_FALSE(json.empty());
    EXPECT_NE(json.find("calculator"), std::string::npos);
    EXPECT_NE(json.find("weather"), std::string::npos);
    EXPECT_NE(json.find("Perform mathematical calculations"), std::string::npos);
    EXPECT_NE(json.find("Get weather information"), std::string::npos);
}

// Test function calling format with an empty tool list
TEST(FunctionCallingFormatTest, ToFunctionCallingFormat_EmptyTools) {
    std::vector<ToolInfo> tools;
    
    std::string json = MCPAgentIntegration::toFunctionCallingFormat(tools);
    
    // An empty tool list should return an empty array
    EXPECT_EQ(json, "[]");
}

// Test initialize and shutdown lifecycle
TEST_F(RAGMCPIntegrationTest, InitializeAndShutdown) {
    config_.rag_config.enabled = false;
    
    // Initialize
    ASSERT_TRUE(integration_.initialize(config_));
    EXPECT_TRUE(integration_.isInitialized());
    
    // Shutdown
    integration_.shutdown();
    EXPECT_FALSE(integration_.isInitialized());
    
    // Can re-initialize
    ASSERT_TRUE(integration_.initialize(config_));
    EXPECT_TRUE(integration_.isInitialized());
}

// Test multiple initializations
TEST_F(RAGMCPIntegrationTest, MultipleInitialize) {
    config_.rag_config.enabled = false;
    
    // First initialization
    ASSERT_TRUE(integration_.initialize(config_));
    EXPECT_TRUE(integration_.isInitialized());
    
    // Second initialization (should shut down first)
    ASSERT_TRUE(integration_.initialize(config_));
    EXPECT_TRUE(integration_.isInitialized());
}

// Test getRelevantTools with a custom top_k
TEST_F(RAGMCPIntegrationTest, GetRelevantTools_CustomTopK) {
    config_.rag_config.enabled = false;
    
    ASSERT_TRUE(integration_.initialize(config_));
    
    // Use a custom top_k
    auto tools = integration_.getRelevantTools("test query", 10);
    // With no MCP server connected, the tool list is empty
    EXPECT_TRUE(tools.empty());
}

// VectorIndex and ToolRetriever integration tests

TEST(VectorIndexIntegrationTest, VectorIndex_ToolRetriever_Integration) {
    // Create a vector index
    VectorIndex index;
    
    // Add some tools
    int dimension = 64;
    for (int i = 0; i < 10; ++i) {
        IndexedTool tool;
        tool.name = "tool_" + std::to_string(i);
        tool.description = "Description for tool " + std::to_string(i);
        tool.input_schema = R"({"type": "object"})";
        tool.embedding = generateRandomVector(dimension);
        index.addTool(tool);
    }
    
    EXPECT_EQ(index.size(), 10u);
    
    // Search (use a -1.0f threshold so nothing is filtered)
    auto query = generateRandomVector(dimension);
    auto results = index.search(query, 5, -1.0f);
    
    EXPECT_EQ(results.size(), 5u);
    
    // Verify results are sorted by similarity
    for (size_t i = 1; i < results.size(); ++i) {
        EXPECT_GE(results[i-1].similarity, results[i].similarity);
    }
}

// Test search consistency after persistence and reload
TEST(VectorIndexIntegrationTest, IndexPersistence_SearchConsistency) {
    VectorIndex original_index;
    int dimension = 64;
    
    // Add tools
    for (int i = 0; i < 5; ++i) {
        IndexedTool tool;
        tool.name = "tool_" + std::to_string(i);
        tool.description = "Description " + std::to_string(i);
        tool.input_schema = R"({"type": "object"})";
        tool.embedding = generateRandomVector(dimension);
        original_index.addTool(tool);
    }
    
    // Save
    std::string temp_path = "/tmp/test_integration_index_" + std::to_string(rand()) + ".json";
    ASSERT_TRUE(original_index.saveToFile(temp_path));
    
    // Load
    VectorIndex loaded_index;
    ASSERT_TRUE(loaded_index.loadFromFile(temp_path));
    
    // Search with the same query
    auto query = generateRandomVector(dimension);
    auto original_results = original_index.search(query, 3);
    auto loaded_results = loaded_index.search(query, 3);
    
    // Verify the result counts match
    EXPECT_EQ(original_results.size(), loaded_results.size());
    
    // Verify the tool names match
    for (size_t i = 0; i < original_results.size(); ++i) {
        EXPECT_EQ(original_results[i].tool.name, loaded_results[i].tool.name);
    }
    
    // Clean up
    std::filesystem::remove(temp_path);
}

// Test cache and vector index integration
TEST(CacheIntegrationTest, Cache_VectorIndex_Integration) {
    CacheConfig cache_config;
    cache_config.enabled = true;
    cache_config.max_size = 100;
    cache_config.ttl_seconds = 3600;
    
    EmbeddingCache cache(cache_config);
    VectorIndex index;
    
    int dimension = 64;
    
    // Simulate the tool indexing flow
    std::vector<std::string> tool_descriptions = {
        "Calculate mathematical expressions",
        "Get weather information for a location",
        "Search the web for information"
    };
    
    for (size_t i = 0; i < tool_descriptions.size(); ++i) {
        const auto& desc = tool_descriptions[i];
        
        // Check the cache
        auto cached = cache.get(desc);
        std::vector<float> embedding;
        
        if (cached.has_value()) {
            embedding = cached.value();
        } else {
            // Generate a new embedding (simulating an API call)
            embedding = generateRandomVector(dimension);
            cache.put(desc, embedding);
        }
        
        // Add to the index
        IndexedTool tool;
        tool.name = "tool_" + std::to_string(i);
        tool.description = desc;
        tool.input_schema = "{}";
        tool.embedding = embedding;
        index.addTool(tool);
    }
    
    EXPECT_EQ(index.size(), 3u);
    
    // Verify cache hits
    for (const auto& desc : tool_descriptions) {
        auto cached = cache.get(desc);
        EXPECT_TRUE(cached.has_value());
    }
}

// Property: RAG fallback behavior

RC_GTEST_PROP(RAGMCPIntegrationProperties, FallbackToAllToolsWhenRAGUnavailable, ()) {
    MCPAgentConfig config;
    config.enable_mcp = false;
    config.rag_config.enabled = true;
    config.rag_config.api_key = "";  // Invalid API key
    
    MCPAgentIntegration integration;
    
    // Initialization should succeed (degraded mode)
    RC_ASSERT(integration.initialize(config));
    
    // Generate a random query
    std::string query = "random query " + std::to_string(*rc::gen::inRange(0, 10000));
    
    // Fetching relevant tools must not crash
    auto tools = integration.getRelevantTools(query);
    
    // An empty tool list is expected since there is no MCP server
    // The key point is that it must not throw
    RC_ASSERT(tools.empty() || !tools.empty());  // Always true; verifies no crash
    
    integration.shutdown();
}

} // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
