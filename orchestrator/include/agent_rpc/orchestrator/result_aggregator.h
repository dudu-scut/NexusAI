/**
 * @file result_aggregator.h
 * @brief ResultAggregator — merges multiple subtask results into a final answer
 *
 * Two strategies:
 *   - concat:        Topological-order concatenation (fast, zero-cost)
 *   - llm_synthesize: LLM-powered synthesis (slower, higher quality)
 */

#pragma once

#include "agent_rpc/common/env_loader.h"
#include "agent_rpc/orchestrator/task_planner.h"
#include "agent_rpc/orchestrator/task_executor.h"
#include <a2a/llm_client.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace agent_rpc {
namespace orchestrator {

// ── Aggregated result ──────────────────────────────────────────────────────

struct AggregatedResult {
    std::string final_answer;
    std::vector<SubTaskResult> sub_results;   // Ordered by topology
    std::string strategy;                      // "concat" | "llm_synthesize"
    int64_t total_time_ms = 0;
    // P13(d)/A2: non-fatal notices (dropped subtasks, uncertain write tasks)
    // that the caller should surface to the user (metadata / status event).
    std::vector<std::string> warnings;
};

// ── Configuration ──────────────────────────────────────────────────────────

struct AggregatorConfig {
    std::string default_strategy = "concat";   // "concat" | "llm_synthesize"
    std::string api_key;
    std::string model    = agent_rpc::common::envOrDefault("LLM_MODEL", "deepseek-v4-flash");
    std::string api_url  = agent_rpc::common::envOrDefault("LLM_API_URL", "https://api.deepseek.com/v1/chat/completions");
};

// ── ResultAggregator class ─────────────────────────────────────────────────

class ResultAggregator {
public:
    explicit ResultAggregator(const AggregatorConfig& config);

    /**
     * Aggregate subtask results into a final answer.
     * @param plan     The execution plan (for ordering and context)
     * @param results  Map of subtask_id → SubTaskResult
     * @return AggregatedResult with final_answer and per-subtask details
     */
    AggregatedResult aggregate(
        const ExecutionPlan& plan,
        const std::unordered_map<std::string, SubTaskResult>& results);

private:
    std::string aggregateConcat(
        const ExecutionPlan& plan,
        const std::unordered_map<std::string, SubTaskResult>& results) const;

    // Returns {answer, used_concat_fallback} so aggregate() can label
    // AggregatedResult.strategy truthfully when the LLM path degraded to the
    // concat output internally (empty context / empty answer / exception).
    std::pair<std::string, bool> aggregateLLMSynthesize(
        const ExecutionPlan& plan,
        const std::unordered_map<std::string, SubTaskResult>& results);

    AggregatorConfig config_;
    std::unique_ptr<LLMClient> llm_client_;
};

} // namespace orchestrator
} // namespace agent_rpc
