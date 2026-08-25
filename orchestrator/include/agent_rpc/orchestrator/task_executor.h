/**
 * @file task_executor.h
 * @brief TaskExecutor — DAG execution engine for multi-agent orchestration
 *
 * Executes an ExecutionPlan by topologically sorting subtasks into layers,
 * running same-layer tasks in parallel via std::async, and propagating
 * predecessor results into dependent subtask prompts.
 */

#pragma once

#include "agent_rpc/orchestrator/task_planner.h"
#include "agent_rpc/orchestrator/agent_router.h"
#include "agent_rpc/common/trace_context.h"
#include <a2a/llm_client.hpp>
#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace agent_rpc {
namespace orchestrator {

// ── Result types ───────────────────────────────────────────────────────────

struct SubTaskResult {
    std::string subtask_id;
    std::string description;
    std::string result;                // Agent response text
    bool success = false;
    int64_t duration_ms = 0;
    std::string error_message;
    // Spans captured in the subtask worker thread (P5: merged back into the
    // parent TraceContext after all futures of a layer are collected).
    // Empty for timed-out/abandoned tasks and when trace propagation is off.
    std::vector<agent_rpc::common::Span> child_spans;
};

// ── Configuration ──────────────────────────────────────────────────────────

struct ExecutorConfig {
    int subtask_timeout_seconds  = 30;   // Per-subtask timeout
    int global_timeout_seconds   = 120;  // Overall execution timeout
};

// ── Progress callback ──────────────────────────────────────────────────────

enum class SubTaskEventType { START, COMPLETE, FAILED };

struct SubTaskEvent {
    SubTaskEventType type;
    std::string subtask_id;
    std::string detail;                // Result summary or error message
};

using ProgressCallback = std::function<void(const SubTaskEvent&)>;

// ── Agent call function type ───────────────────────────────────────────────
// Signature: (agent_url, prompt) → response_text
// The Orchestrator resolves agent_url from preferred_agent_id before calling.

using AgentCallFn = std::function<std::string(const std::string& agent_url,
                                               const std::string& prompt)>;

// ── TaskExecutor class ─────────────────────────────────────────────────────

class TaskExecutor {
public:
    TaskExecutor(AgentRouter& router, const ExecutorConfig& config);

    /**
     * Execute the full DAG plan and collect results.
     * @param plan              The execution plan from TaskPlanner
     * @param call_agent        Function that sends a prompt to an agent by skill
     * @param on_progress       Optional callback for real-time subtask events
     * @return Map of subtask_id → SubTaskResult
     */
    std::unordered_map<std::string, SubTaskResult> execute(
        const ExecutionPlan& plan,
        const AgentCallFn& call_agent,
        const ProgressCallback& on_progress = nullptr);

private:
    // Topological sort into layers (same-layer = parallel, cross-layer = serial)
    std::vector<std::vector<std::string>> topologicalLayers(
        const std::vector<SubTask>& tasks) const;

    // Build prompt with predecessor results injected as context
    std::string buildSubtaskPrompt(
        const SubTask& subtask,
        const std::unordered_map<std::string, SubTaskResult>& results) const;

    // Execute one subtask (called inside std::async)
    SubTaskResult executeSubtask(
        const SubTask& subtask,
        const std::string& enriched_prompt,
        const AgentCallFn& call_agent);

    AgentRouter& router_;
    ExecutorConfig config_;
};

} // namespace orchestrator
} // namespace agent_rpc
