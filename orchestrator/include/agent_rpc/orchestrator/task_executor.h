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
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
    // Actual agent that executed the subtask (preferred agent when it was
    // healthy, otherwise the routing fallback pick). Populated by
    // executeSubtask so server-layer callers can record per-agent metrics
    // without depending on the registry module.
    std::string agent_id;
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

// P20: cancellation hook for in-flight subtask calls. Invoked with the
// pre-resolved agent URL when a subtask times out or the global deadline
// is exhausted, so the caller can abort the blocked A2A HTTP call instead
// of leaving a zombie thread behind.
using CancelFn = std::function<void(const std::string& agent_url)>;

// ── TaskExecutor class ─────────────────────────────────────────────────────

class TaskExecutor {
public:
    TaskExecutor(AgentRouter& router, const ExecutorConfig& config);
    ~TaskExecutor();

    /**
     * Execute the full DAG plan and collect results.
     * @param plan              The execution plan from TaskPlanner
     * @param call_agent        Function that sends a prompt to an agent by skill
     * @param on_progress       Optional callback for real-time subtask events
     * @param on_cancel         Optional callback invoked with the agent URL of
     *                          a subtask whose execution timed out (P20); null
     *                          keeps the legacy no-cancel behavior
     * @return Map of subtask_id → SubTaskResult
     */
    std::unordered_map<std::string, SubTaskResult> execute(
        const ExecutionPlan& plan,
        const AgentCallFn& call_agent,
        const ProgressCallback& on_progress = nullptr,
        const CancelFn& on_cancel = nullptr);

private:
    // Topological sort into layers (same-layer = parallel, cross-layer = serial)
    std::vector<std::vector<std::string>> topologicalLayers(
        const std::vector<SubTask>& tasks) const;

    // Build prompt with predecessor results injected as context
    std::string buildSubtaskPrompt(
        const SubTask& subtask,
        const std::unordered_map<std::string, SubTaskResult>& results) const;

    // Resolve the execution target for a subtask: preferred (healthy) agent
    // first, four-tier routing fallback second. Throws when no agent is
    // available. Extracted so the single/parallel branches and the P20
    // cancellation hook share one resolution path.
    std::pair<std::string /*url*/, std::string /*agent_id*/> resolveAgent(
        const SubTask& subtask) const;

    // Execute one subtask (called inside the subtask worker thread). When
    // pre_resolved_url is non-empty the pre-resolved target is used directly
    // instead of re-routing — the caller resolves once so the timeout
    // cancellation target matches the actually executed agent (routing
    // fallbacks are non-deterministic across calls).
    SubTaskResult executeSubtask(
        const SubTask& subtask,
        const std::string& enriched_prompt,
        const AgentCallFn& call_agent,
        const std::string& pre_resolved_url = "",
        const std::string& pre_resolved_agent_id = "");

    // Launch fn on a background thread whose handle is parked internally.
    // Unlike std::async(std::launch::async), abandoning the returned future
    // after a timeout never blocks the caller on task completion; parked
    // threads are joined in the destructor (bounded by the HTTP timeout).
    std::future<SubTaskResult> launchSubtask(
        std::function<SubTaskResult()> fn);

    AgentRouter& router_;
    ExecutorConfig config_;

    // Worker threads of timed-out subtasks. `done` lets finished threads be
    // reaped lazily; the rest are joined in the destructor.
    struct ParkedThread {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex zombie_mutex_;
    std::vector<ParkedThread> zombie_threads_;
};

} // namespace orchestrator
} // namespace agent_rpc
