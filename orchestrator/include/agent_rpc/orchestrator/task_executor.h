/**
 * @file task_executor.h
 * @brief TaskExecutor — DAG execution engine for multi-agent orchestration
 *
 * Executes an ExecutionPlan by topologically sorting subtasks into layers,
 * running same-layer tasks in parallel on a fixed subtask worker pool
 * (P19), and propagating predecessor results into dependent subtask
 * prompts.
 */

#pragma once

#include "agent_rpc/orchestrator/task_planner.h"
#include "agent_rpc/orchestrator/agent_router.h"
#include "agent_rpc/common/trace_context.h"
#include <a2a/llm_client.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
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
    // False when the subtask never reached an agent (global deadline hit at
    // a layer boundary, cancellation before launch, or target resolution
    // failed). Fabricated failure results must not pollute per-agent health
    // metrics: callers gate recordAgentCall/metrics on this flag.
    bool executed = true;
    // B3/P20-7: true when a write-shaped (SideEffect) subtask timed out —
    // the external action MAY have applied, so the result must be reported
    // as unknown rather than a plain retryable failure.
    bool uncertain = false;
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
    // P19: fixed subtask worker pool size. 0 = auto (max(4, min(16,
    // hardware_concurrency))). Tests inject a small pool via this field;
    // there is deliberately no env switch (P19 decision: no rollback knob).
    int subtask_pool_size        = 0;
};

// ── Subtask worker pool (P19) ──────────────────────────────────────────────
// Fixed set of resident workers shared by every DAG execution of this
// TaskExecutor (a process-level singleton in the server, so the pool is
// effectively process-wide). Pins the thread-count upper bound to the pool
// size instead of (concurrent requests × layer width), and packaged_task
// futures carry no destructor-join semantics. Workers stuck in an in-flight
// HTTP call are bounded by the A2A HTTP timeout plus the P20/P24 abort
// flag, so shutdown joins cannot block indefinitely.
class SubtaskPool {
public:
    explicit SubtaskPool(int worker_count) {
        const int n = worker_count > 0 ? worker_count : 1;
        workers_.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            workers_.emplace_back([this]() { workerLoop(); });
        }
    }

    ~SubtaskPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    SubtaskPool(const SubtaskPool&) = delete;
    SubtaskPool& operator=(const SubtaskPool&) = delete;

    void submit(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return;  // shutdown race: drop instead of enqueueing forever
            }
            jobs_.push(std::move(job));
        }
        cv_.notify_one();
    }

private:
    void workerLoop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this]() { return stopping_ || !jobs_.empty(); });
                if (stopping_) {
                    return;  // queued jobs are dropped at shutdown
                }
                job = std::move(jobs_.front());
                jobs_.pop();
            }
            job();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> jobs_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
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
     * @param cancelled         Optional probe checked at every layer boundary;
     *                          when it returns true, all remaining subtasks are
     *                          marked failed with executed=false and execution
     *                          stops (client-disconnect propagation). Null keeps
     *                          the run-to-completion behavior.
     * @return Map of subtask_id → SubTaskResult
     */
    std::unordered_map<std::string, SubTaskResult> execute(
        const ExecutionPlan& plan,
        const AgentCallFn& call_agent,
        const ProgressCallback& on_progress = nullptr,
        const CancelFn& on_cancel = nullptr,
        const std::function<bool()>& cancelled = nullptr);

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

    // Submit fn to the subtask pool (P19). The wrapper installed before the
    // payload enforces the dequeue-before-run check: a queued task whose
    // wait deadline (min(subtask cap, remaining global budget), fixed at
    // submission) has already expired, or whose caller has aborted it
    // (timeout / client disconnect via `abandoned`), is skipped without
    // issuing its HTTP call — a saturated pool must never execute a stale
    // task long after its result was given up on.
    std::future<SubTaskResult> launchSubtask(
        const std::string& task_id,
        std::function<SubTaskResult()> fn,
        std::chrono::steady_clock::time_point wait_deadline,
        std::shared_ptr<std::atomic<bool>> abandoned);

    AgentRouter& router_;
    ExecutorConfig config_;
    SubtaskPool pool_;
};

} // namespace orchestrator
} // namespace agent_rpc
