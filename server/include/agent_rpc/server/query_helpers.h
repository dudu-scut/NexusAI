/**
 * @file query_helpers.h
 * @brief Helper utilities for AIQueryService
 *
 * Extracted from ai_query_service.cpp:
 *   - Task status tracking (recordMetrics, updateTaskStatus, cleanupExpiredTasks)
 *   - Agent switch detection with cross-agent summary generation
 *   - UUID generation, memory context building, error sanitization
 */

#pragma once

#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <set>
#include <memory>
#include <vector>

namespace agent_communication { class AIQueryRequest; }

namespace agent_rpc {

// Forward declarations
namespace common { class MemoryService; class RedisClient; class QueryDomainRepository; }

namespace server {

/**
 * @brief Stateless and stateful helpers extracted from AIQueryServiceImpl
 *
 * Stateless methods are static; stateful methods operate on the
 * summary-generation tracking owned by this struct.
 */
struct QueryHelpers {

    QueryHelpers() = default;
    // Destructor drains pending summary futures so shutdown never leaks
    // detached std::async tasks (the old (void)std::async pattern).
    ~QueryHelpers();

    // The in-memory task-status cache was deleted: it was write-only
    // (GetQueryStatus reads the durable PostgreSQL query_logs row; nothing
    // ever consumed the cache). The entry points below keep their signatures
    // as no-op state-transition hooks so the Query pipeline call sites stay
    // unchanged.

    void updateTaskStatus(const std::string& task_id, const std::string& state,
                          const std::string& agent_id = "",
                          const std::string& agent_name = "",
                          const std::string& error_msg = "");

    void cleanupExpiredTasks();

    static void recordMetrics(const std::string& method, int64_t duration_ms, bool success);
    static std::string generateRequestId();

    // Sanitize raw CURL errors into user-friendly messages
    static std::string sanitizeErrorMessage(const std::string& msg);

    std::mutex memory_llm_mutex;
    std::set<std::string> summary_in_progress;  // context_ids with ongoing summary generation
    // Note: memory_llm_client_ is not owned here; passed as parameter.

    // Injectable summarizer hook: (system_prompt, history) -> summary text.
    // When empty, handleAgentSwitch falls back to the LLMClient passed as a
    // parameter. Tests use this seam to observe prompts without a live LLM.
    using SummaryFn = std::function<std::string(const std::string& prompt,
                                                const std::string& history)>;
    SummaryFn summarize_fn;

    // Builds the cross-agent summary prompt. When target agent information is
    // available the prompt is specialized to the taking-over assistant's
    // duties so only relevant context survives the switch.
    static std::string buildSummaryPrompt(const std::string& target_agent_name,
                                          const std::string& target_agent_duties);

    void handleAgentSwitch(common::MemoryService* memory_service,
                           void* memory_llm_client,  // LLMClient*
                           common::QueryDomainRepository* domain_repo,
                           const std::string& user_id,
                           const std::string& context_id,
                           const std::string& current_agent_id,
                           const std::string& target_agent_name = "",
                           const std::string& target_agent_duties = "");

    static std::string buildMemoryContext(const agent_communication::AIQueryRequest* request);

private:
    // Collect already-finished summary futures without blocking (wait_for(0)).
    void reapFinishedSummaries();

    std::mutex futures_mutex_;
    std::vector<std::future<void>> pending_summaries_;
};

} // namespace server
} // namespace agent_rpc
