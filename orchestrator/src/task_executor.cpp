/**
 * @file task_executor.cpp
 * @brief TaskExecutor — DAG execution engine implementation
 */

#include "agent_rpc/orchestrator/task_executor.h"
#include "agent_rpc/common/trace_context.h"
#include "agent_rpc/common/env_loader.h"
#include <algorithm>
#include <chrono>
#include <future>
#include <queue>
#include <sstream>
#include <unordered_set>

namespace agent_rpc {
namespace orchestrator {

namespace {

// P19: pool size resolution — explicit config wins, otherwise clamp the
// hardware concurrency hint into [4, 16].
int resolvePoolSize(const ExecutorConfig& config) {
    if (config.subtask_pool_size > 0) {
        return config.subtask_pool_size;
    }
    const unsigned hw = std::thread::hardware_concurrency();
    const int hint = hw > 0 ? static_cast<int>(hw) : 4;
    return std::max(4, std::min(16, hint));
}

// P24 C0.5: segmented wait on a subtask future — the collector must notice
// a client disconnect while an in-flight call is running, not only at the
// layer boundary. Each slice is capped so the probe runs at least every
// kProbeSlice while the deadline is still open.
enum class WaitOutcome { Ready, TimedOut, Cancelled };

WaitOutcome waitForSubtask(
    std::future<SubTaskResult>& fut,
    std::chrono::steady_clock::time_point wait_deadline,
    const std::function<bool()>& cancelled) {
    constexpr auto kProbeSlice = std::chrono::milliseconds(200);
    if (cancelled && cancelled()) {
        return WaitOutcome::Cancelled;
    }
    for (;;) {
        auto now = std::chrono::steady_clock::now();
        if (now >= wait_deadline) {
            return WaitOutcome::TimedOut;
        }
        const auto slice = std::min(
            kProbeSlice,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                wait_deadline - std::chrono::steady_clock::now()));
        if (fut.wait_for(slice) == std::future_status::ready) {
            return WaitOutcome::Ready;
        }
        if (cancelled && cancelled()) {
            return WaitOutcome::Cancelled;
        }
    }
}

} // namespace

TaskExecutor::TaskExecutor(AgentRouter& router, const ExecutorConfig& config)
    : router_(router)
    , config_(config)
    , pool_(resolvePoolSize(config))
{}

TaskExecutor::~TaskExecutor() = default;

std::future<SubTaskResult> TaskExecutor::launchSubtask(
    const std::string& task_id,
    std::function<SubTaskResult()> fn,
    std::chrono::steady_clock::time_point wait_deadline,
    std::shared_ptr<std::atomic<bool>> abandoned) {
    auto task = std::make_shared<std::packaged_task<SubTaskResult()>>(
        [task_id, fn = std::move(fn), wait_deadline,
         abandoned = std::move(abandoned)]() -> SubTaskResult {
            // P19 dequeue-before-run check: a task whose wait window has
            // already expired or whose caller has aborted it (timeout /
            // client disconnect) must not start executing after sitting in
            // the queue. The fabricated result is normally discarded by the
            // collector, which has already given up on this task; in the
            // rare race where the collector still picks it up, subtask_id
            // keeps the result self-describing and executed=false keeps it
            // out of health accounting.
            if (abandoned->load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= wait_deadline) {
                SubTaskResult skipped;
                skipped.subtask_id = task_id;
                skipped.success = false;
                skipped.executed = false;
                skipped.error_message =
                    abandoned->load(std::memory_order_relaxed)
                        ? "Cancelled before execution"
                        : "Skipped: wait window expired while queued";
                return skipped;
            }
            return fn();
        });
    std::future<SubTaskResult> fut = task->get_future();
    pool_.submit([task]() { (*task)(); });
    return fut;
}

std::unordered_map<std::string, SubTaskResult> TaskExecutor::execute(
    const ExecutionPlan& plan,
    const AgentCallFn& call_agent,
    const ProgressCallback& on_progress,
    const CancelFn& on_cancel,
    const std::function<bool()>& cancelled) {

    std::unordered_map<std::string, SubTaskResult> results;

    auto global_start = std::chrono::steady_clock::now();
    auto global_deadline = global_start + std::chrono::seconds(config_.global_timeout_seconds);

    // P20: per-subtask wait cap — subtask_timeout_seconds is now honored for
    // the first time (previously declared and assigned but never read). A
    // non-positive value disables the cap and falls back to the remaining
    // global budget.
    const std::chrono::milliseconds subtask_cap =
        (config_.subtask_timeout_seconds > 0)
        ? std::chrono::milliseconds(static_cast<long long>(config_.subtask_timeout_seconds) * 1000)
        : std::chrono::milliseconds::max();

    // Topological sort into layers
    auto layers = topologicalLayers(plan.tasks);

    // Build a lookup map: id → SubTask
    std::unordered_map<std::string, const SubTask*> task_map;
    for (const auto& t : plan.tasks) {
        task_map[t.id] = &t;
    }

    // Client-disconnect propagation: when the probe reports cancellation,
    // every not-yet-executed subtask is marked failed with executed=false
    // (no agent was contacted, so per-agent health metrics must not be
    // polluted) and execution stops immediately. Takes the start layer as a
    // parameter because it is defined before the layer cursor.
    auto markRemainingCancelled = [&](size_t from_layer) {
        for (size_t li = from_layer; li < layers.size(); ++li) {
            for (const auto& tid : layers[li]) {
                if (results.find(tid) == results.end()) {
                    SubTaskResult r;
                    r.subtask_id = tid;
                    r.success = false;
                    r.executed = false;
                    r.error_message = "Cancelled before execution";
                    results[tid] = std::move(r);
                }
            }
        }
    };

    // Execute layer by layer
    size_t layer_idx = 0;
    for (const auto& layer : layers) {
        // Check global timeout
        auto now = std::chrono::steady_clock::now();
        if (now >= global_deadline) {
            // Mark ALL remaining subtasks (current + future layers) as timed out
            for (size_t li = layer_idx; li < layers.size(); ++li) {
                for (const auto& tid : layers[li]) {
                    if (results.find(tid) == results.end()) {
                        SubTaskResult r;
                        r.subtask_id = tid;
                        r.success = false;
                        r.executed = false;
                        r.error_message = "Global timeout exceeded";
                        results[tid] = std::move(r);
                    }
                }
            }
            break;
        }

        if (cancelled && cancelled()) {
            markRemainingCancelled(layer_idx);
            break;
        }

        if (layer.size() == 1) {
            // Single subtask — P20: run on a worker thread like the parallel
            // branch. The legacy synchronous path only checked the global
            // deadline at the start, so a lone subtask could overrun without
            // bound; waiting with a deadline caps the overrun and enables
            // cancellation of the in-flight call.
            const auto& tid = layer[0];
            auto it = task_map.find(tid);
            if (it == task_map.end()) { ++layer_idx; continue; }

            const SubTask& st = *it->second;
            std::string prompt = buildSubtaskPrompt(st, results);

            if (on_progress) {
                on_progress({SubTaskEventType::START, tid, ""});
            }

            // Check global timeout before execution
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                global_deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                SubTaskResult r;
                r.subtask_id = tid;
                r.success = false;
                r.executed = false;
                r.error_message = "Global timeout exceeded";
                results[tid] = std::move(r);
                // Mark remaining layers as timed out
                for (size_t li = layer_idx + 1; li < layers.size(); ++li) {
                    for (const auto& rtid : layers[li]) {
                        if (results.find(rtid) == results.end()) {
                            SubTaskResult rr;
                            rr.subtask_id = rtid;
                            rr.success = false;
                            rr.executed = false;
                            rr.error_message = "Global timeout exceeded";
                            results[rtid] = std::move(rr);
                        }
                    }
                }
                break;
            }

            // Pre-resolve the target so a timeout can cancel the matching
            // in-flight call; the same target is handed to the worker so the
            // executed URL and the cancelled URL can never diverge.
            std::string cancel_url;
            std::string cancel_agent_id;
            try {
                auto target = resolveAgent(st);
                cancel_url = target.first;
                cancel_agent_id = target.second;
            } catch (const std::exception& e) {
                SubTaskResult r;
                r.subtask_id = tid;
                r.success = false;
                r.executed = false;
                r.error_message = e.what();
                if (on_progress) {
                    on_progress({SubTaskEventType::FAILED, tid, r.error_message});
                }
                results[tid] = std::move(r);
                ++layer_idx;
                continue;
            }

            // Capture parent trace context for subtask thread propagation
            std::string parent_trace_id;
            std::string parent_user_id;
            auto* parent_trace = agent_rpc::common::TraceContext::current();
            if (parent_trace) {
                parent_trace_id = parent_trace->traceId();
                parent_user_id = parent_trace->userId();
            }
            const bool trace_propagation =
                agent_rpc::common::envOrDefault("NEXUSAI_TRACE_PARENT_PROPAGATION", "1") != "0";

            // launchSubtask instead of std::async: abandoning the future
            // after the wait timeout must not block the layer on task
            // completion (std::async futures block in their destructor).
            // st / call_agent are captured BY VALUE: a timed-out task keeps
            // running after execute() returns, so capturing them by reference
            // would dangle once the caller destroys the plan and the call
            // lambda. The copy makes the abandoned worker self-contained.
            // P19: the wait deadline is fixed before submission so the pool
            // worker's dequeue check and the collector share one bound.
            auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                global_deadline - std::chrono::steady_clock::now());
            if (remaining_ms <= std::chrono::milliseconds(0)) {
                remaining_ms = std::chrono::milliseconds(1);
            }
            const auto wait_budget = std::min(subtask_cap, remaining_ms);
            const auto wait_deadline = std::chrono::steady_clock::now() + wait_budget;
            const bool global_capped = (wait_budget >= remaining_ms);
            auto abandoned = std::make_shared<std::atomic<bool>>(false);

            auto fut = launchSubtask(
                tid,
                [this, st, p = std::move(prompt), call_agent,
                 cancel_url, cancel_agent_id,
                 parent_trace_id, parent_user_id, trace_propagation]() {
                    if (trace_propagation) {
                        agent_rpc::common::TraceContext::init(
                            parent_user_id, "", parent_trace_id);
                    } else {
                        agent_rpc::common::TraceContext::init(parent_user_id, "");
                    }
                    auto* trace = agent_rpc::common::TraceContext::current();
                    trace->startSpan("subtask_" + st.id, "executor");
                    auto result = executeSubtask(
                        st, p, call_agent, cancel_url, cancel_agent_id);
                    trace->endSpan();
                    if (trace_propagation) {
                        result.child_spans = trace->completedSpans();
                    }
                    return result;
                },
                wait_deadline, abandoned);

            SubTaskResult result;
            auto wait_start = std::chrono::steady_clock::now();
            const auto outcome =
                waitForSubtask(fut, wait_deadline, cancelled);
            if (outcome == WaitOutcome::Ready) {
                result = fut.get();
            } else {
                // P19/P24: mark the task abandoned so a still-queued worker
                // skips it at the dequeue check; abort the in-flight A2A
                // call instead of leaving a zombie worker blocked until its
                // HTTP timeout.
                abandoned->store(true, std::memory_order_release);
                if (on_cancel && !cancel_url.empty()) {
                    on_cancel(cancel_url);
                }
                result.subtask_id = tid;
                result.success = false;
                // A real attempt was in flight — executed stays true so the
                // timeout counts as a genuine health sample; record the
                // actual wait duration instead of a misleading 0ms.
                result.duration_ms = static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - wait_start).count());
                if (outcome == WaitOutcome::Cancelled) {
                    // P24 C0.5: the client disconnected while the call was
                    // in flight — abort it instead of waiting out the HTTP
                    // timeout.
                    result.error_message =
                        "Cancelled while executing (client disconnected)";
                } else {
                    result.error_message = global_capped
                        ? "Global timeout exceeded"
                        : "Subtask timeout exceeded";
                }
                // B3/P20-7: a write-shaped task may have already applied its
                // effect — report UNCERTAIN instead of a plain failure.
                if (st.effect == SubTask::Effect::SideEffect) {
                    result.uncertain = true;
                    result.error_message +=
                        "; side-effect may have applied (unknown)";
                }
            }

            if (on_progress) {
                SubTaskEventType evt_type = result.success
                    ? SubTaskEventType::COMPLETE : SubTaskEventType::FAILED;
                on_progress({evt_type, tid,
                    result.success ? result.result : result.error_message});
            }

            if (trace_propagation && parent_trace && !result.child_spans.empty()) {
                auto& parent_spans = parent_trace->mutableSpans();
                parent_spans.insert(parent_spans.end(),
                                    result.child_spans.begin(),
                                    result.child_spans.end());
            }

            results[tid] = std::move(result);
        } else {
            // Multiple subtasks — execute in parallel on the subtask pool
            // (P19: fixed worker count instead of one thread per task).
            struct LaunchedSubtask {
                std::string tid;
                std::future<SubTaskResult> fut;
                std::chrono::steady_clock::time_point wait_deadline;
                bool global_capped = false;
                std::shared_ptr<std::atomic<bool>> abandoned;
                std::string cancel_url;
            };
            std::vector<LaunchedSubtask> launched;

            // Capture parent trace context for subtask thread propagation
            std::string parent_trace_id;
            std::string parent_user_id;
            auto* parent_trace = agent_rpc::common::TraceContext::current();
            if (parent_trace) {
                parent_trace_id = parent_trace->traceId();
                parent_user_id = parent_trace->userId();
            }

            // P5: cross-thread trace propagation (default on). When enabled,
            // worker threads reuse the parent trace id and their completed
            // spans are merged back into the parent TraceContext once every
            // future of this layer has been collected.
            const bool trace_propagation =
                agent_rpc::common::envOrDefault("NEXUSAI_TRACE_PARENT_PROPAGATION", "1") != "0";

            // Pre-resolve each target so a timeout can cancel the matching
            // in-flight call and the worker uses the SAME target (no second
            // routing pass — routing fallbacks are non-deterministic).
            std::unordered_map<std::string, std::pair<std::string, std::string>>
                layer_targets;
            for (const auto& tid : layer) {
                auto it = task_map.find(tid);
                if (it == task_map.end()) continue;
                try {
                    layer_targets[tid] = resolveAgent(*it->second);
                } catch (const std::exception& e) {
                    SubTaskResult r;
                    r.subtask_id = tid;
                    r.success = false;
                    r.executed = false;
                    r.error_message = e.what();
                    if (on_progress) {
                        on_progress({SubTaskEventType::FAILED, tid, r.error_message});
                    }
                    results[tid] = std::move(r);
                }
            }

            for (const auto& tid : layer) {
                auto it = task_map.find(tid);
                if (it == task_map.end() || results.count(tid) != 0) continue;

                const SubTask& st = *it->second;
                std::string prompt = buildSubtaskPrompt(st, results);

                if (on_progress) {
                    on_progress({SubTaskEventType::START, tid, ""});
                }

                const auto& target = layer_targets.at(tid);
                const std::string cancel_url = target.first;
                const std::string cancel_agent_id = target.second;

                // P19: fix this task's wait window at submission — the pool
                // worker's dequeue check and the collector share one bound,
                // and queueing time consumes the deadline by design.
                auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    global_deadline - std::chrono::steady_clock::now());
                if (remaining_ms <= std::chrono::milliseconds(0)) {
                    remaining_ms = std::chrono::milliseconds(1);
                }
                const auto wait_budget = std::min(subtask_cap, remaining_ms);

                LaunchedSubtask item;
                item.tid = tid;
                item.wait_deadline = std::chrono::steady_clock::now() + wait_budget;
                item.global_capped = (wait_budget >= remaining_ms);
                item.abandoned = std::make_shared<std::atomic<bool>>(false);
                item.cancel_url = cancel_url;

                // By-value capture as in the single-subtask branch: the
                // worker may outlive this execute() call after a timeout.
                item.fut = launchSubtask(
                        tid,
                        [this, st, p = std::move(prompt), call_agent,
                         cancel_url, cancel_agent_id,
                         parent_trace_id, parent_user_id, trace_propagation]() {
                            // Propagate trace context to subtask thread: reuse
                            // the parent trace id when propagation is enabled,
                            // otherwise keep the legacy fresh-id behavior.
                            if (trace_propagation) {
                                agent_rpc::common::TraceContext::init(
                                    parent_user_id, "", parent_trace_id);
                            } else {
                                agent_rpc::common::TraceContext::init(parent_user_id, "");
                            }
                            auto* trace = agent_rpc::common::TraceContext::current();
                            trace->startSpan("subtask_" + st.id, "executor");
                            auto result = executeSubtask(
                                st, p, call_agent, cancel_url, cancel_agent_id);
                            trace->endSpan();
                            // Hand the worker-thread spans back to the parent
                            // thread (copied before the thread-local context
                            // is destroyed with the thread).
                            if (trace_propagation) {
                                result.child_spans = trace->completedSpans();
                            }
                            return result;
                        },
                        item.wait_deadline, item.abandoned);
                launched.push_back(std::move(item));
            }

            // Collect results with deadline awareness (fixes #18: DAG global timeout
            // now actually cancels waiting on incomplete async tasks instead of
            // blocking indefinitely on fut.get())
            for (auto& item : launched) {
                const auto& tid = item.tid;
                try {
                    SubTaskResult result;
                    auto wait_start = std::chrono::steady_clock::now();
                    const auto outcome =
                        waitForSubtask(item.fut, item.wait_deadline, cancelled);
                    if (outcome == WaitOutcome::Ready) {
                        result = item.fut.get();
                    } else {
                        // P19/P24: mark abandoned (queued siblings of this
                        // task skip execution at the dequeue check) and
                        // abort the in-flight A2A call.
                        item.abandoned->store(true, std::memory_order_release);
                        if (on_cancel && !item.cancel_url.empty()) {
                            on_cancel(item.cancel_url);
                        }
                        result.subtask_id = tid;
                        result.success = false;
                        // Real attempt in flight — executed stays true;
                        // record the actual wait duration.
                        result.duration_ms = static_cast<int64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - wait_start).count());
                        if (outcome == WaitOutcome::Cancelled) {
                            // P24 C0.5: client disconnected while the call
                            // was in flight — aborted above, return fast.
                            result.error_message =
                                "Cancelled while executing (client disconnected)";
                        } else {
                            // P20: wait no longer than the per-subtask cap
                            // OR the remaining global budget, whichever is
                            // tighter.
                            result.error_message = item.global_capped
                                ? "Global timeout exceeded"
                                : "Subtask timeout exceeded";
                        }
                        // B3/P20-7: write-shaped task → UNCERTAIN.
                        auto task_it = task_map.find(tid);
                        if (task_it != task_map.end() &&
                            task_it->second->effect == SubTask::Effect::SideEffect) {
                            result.uncertain = true;
                            result.error_message +=
                                "; side-effect may have applied (unknown)";
                        }
                    }

                    if (on_progress) {
                        SubTaskEventType evt_type = result.success
                            ? SubTaskEventType::COMPLETE : SubTaskEventType::FAILED;
                        on_progress({evt_type, tid,
                            result.success ? result.result : result.error_message});
                    }

                    results[tid] = std::move(result);
                } catch (const std::exception& e) {
                    SubTaskResult r;
                    r.subtask_id = tid;
                    r.success = false;
                    r.error_message = std::string("Future exception: ") + e.what();

                    if (on_progress) {
                        on_progress({SubTaskEventType::FAILED, tid, r.error_message});
                    }

                    results[tid] = std::move(r);
                }
            }

            // P5: all futures of this layer are collected at this point, so
            // merging worker-thread spans back into the parent TraceContext
            // happens without any concurrency window. Timed-out/abandoned
            // tasks carry empty child_spans, so nothing is merged for them.
            if (trace_propagation && parent_trace) {
                for (const auto& tid : layer) {
                    auto rit = results.find(tid);
                    if (rit == results.end() || rit->second.child_spans.empty()) {
                        continue;
                    }
                    auto& parent_spans = parent_trace->mutableSpans();
                    parent_spans.insert(parent_spans.end(),
                                        rit->second.child_spans.begin(),
                                        rit->second.child_spans.end());
                }
            }
        }
        ++layer_idx;
    }

    return results;
}

std::vector<std::vector<std::string>> TaskExecutor::topologicalLayers(
    const std::vector<SubTask>& tasks) const {

    // Kahn's algorithm with layer tracking
    std::unordered_map<std::string, int> in_degree;
    std::unordered_map<std::string, std::vector<std::string>> dependents;

    // Pass 1: collect the known id set (forward references allowed) and guard
    // against duplicate task ids — a duplicated id would be emitted once via
    // the shared in_degree map while total_emitted is compared against the
    // raw task count, faking a "Circular dependency detected" failure.
    std::unordered_set<std::string> known_ids;
    for (const auto& t : tasks) {
        known_ids.insert(t.id);
    }

    // Pass 2: build in-degrees. First occurrence of an id wins; dependencies
    // pointing at ids outside the task set are ignored (they can never be
    // emitted, so counting them would wedge in_degree above zero and fake a
    // cycle for an actually acyclic plan).
    std::unordered_set<std::string> processed;
    for (const auto& t : tasks) {
        if (!processed.insert(t.id).second) {
            continue;
        }
        in_degree[t.id] = 0;
        for (const auto& dep : t.depends_on) {
            if (known_ids.find(dep) == known_ids.end()) {
                continue;
            }
            dependents[dep].push_back(t.id);
            in_degree[t.id]++;
        }
    }

    std::vector<std::vector<std::string>> layers;
    std::queue<std::string> ready;

    // Layer 0: all subtasks with no dependencies
    for (const auto& [id, deg] : in_degree) {
        if (deg == 0) {
            ready.push(id);
        }
    }

    while (!ready.empty()) {
        std::vector<std::string> current_layer;
        size_t layer_size = ready.size();

        for (size_t i = 0; i < layer_size; ++i) {
            std::string id = ready.front();
            ready.pop();
            current_layer.push_back(id);

            // Decrement in-degree for dependents
            auto it = dependents.find(id);
            if (it != dependents.end()) {
                for (const auto& dep_id : it->second) {
                    in_degree[dep_id]--;
                    if (in_degree[dep_id] == 0) {
                        ready.push(dep_id);
                    }
                }
            }
        }

        if (!current_layer.empty()) {
            // Sort within layer for deterministic order
            std::sort(current_layer.begin(), current_layer.end());
            layers.push_back(std::move(current_layer));
        }
    }

    // Cycle detection: if not all tasks were emitted, there's a cycle
    // (in_degree holds one entry per unique task id).
    size_t total_emitted = 0;
    for (const auto& layer : layers) {
        total_emitted += layer.size();
    }
    if (total_emitted < in_degree.size()) {
        throw std::runtime_error(
            "Circular dependency detected in execution plan: " +
            std::to_string(tasks.size() - total_emitted) + " task(s) in cycle");
    }

    return layers;
}

std::string TaskExecutor::buildSubtaskPrompt(
    const SubTask& subtask,
    const std::unordered_map<std::string, SubTaskResult>& results) const {

    std::string prompt = subtask.description;

    // Inject predecessor results as context
    if (!subtask.depends_on.empty()) {
        std::string context = "\n\n--- 前置任务结果 ---\n";
        bool has_context = false;

        for (const auto& dep_id : subtask.depends_on) {
            auto it = results.find(dep_id);
            if (it != results.end() && it->second.success) {
                context += "\n[" + dep_id + "] " + it->second.description + ":\n";
                context += it->second.result + "\n";
                has_context = true;
            } else if (it != results.end()) {
                context += "\n[" + dep_id + "] 执行失败: " + it->second.error_message + "\n";
                has_context = true;
            }
        }

        if (has_context) {
            prompt += context;
            prompt += "\n请基于以上前置任务的结果完成你的任务。";
        }
    }

    return prompt;
}

std::pair<std::string, std::string> TaskExecutor::resolveAgent(
    const SubTask& subtask) const {

    // Resolve agent URL: prefer pre-resolved agent, fallback to skill routing
    if (!subtask.preferred_agent_id.empty()) {
        auto agent = router_.getAgent(subtask.preferred_agent_id);
        if (agent.has_value() && agent->is_healthy) {
            return {agent->url, subtask.preferred_agent_id};
        }
    }

    // Fallback: route by skill (preferred agent unavailable or not set)
    std::vector<std::string> skills;
    if (!subtask.required_skill.empty()) {
        skills.push_back(subtask.required_skill);
    }
    auto agent = router_.selectAgent(subtask.description, skills);
    if (agent.has_value()) {
        return {agent->url, agent->id};
    }

    throw std::runtime_error(
        "No agent available for subtask: " + subtask.id +
        " (skill: " + subtask.required_skill + ")");
}

SubTaskResult TaskExecutor::executeSubtask(
    const SubTask& subtask,
    const std::string& enriched_prompt,
    const AgentCallFn& call_agent,
    const std::string& pre_resolved_url,
    const std::string& pre_resolved_agent_id) {

    SubTaskResult result;
    result.subtask_id = subtask.id;
    result.description = subtask.description;

    auto start = std::chrono::steady_clock::now();

    try {
        std::string agent_url = pre_resolved_url;
        std::string agent_id = pre_resolved_agent_id;
        if (agent_url.empty()) {
            // Legacy path (direct executeSubtask callers): resolve here.
            auto target = resolveAgent(subtask);
            agent_url = target.first;
            agent_id = target.second;
        }
        result.agent_id = agent_id;

        std::string response = call_agent(agent_url, enriched_prompt);

        auto end = std::chrono::steady_clock::now();
        result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        result.result = response;
        result.success = true;
    } catch (const std::exception& e) {
        auto end = std::chrono::steady_clock::now();
        result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        result.success = false;
        result.error_message = e.what();
    }

    return result;
}

} // namespace orchestrator
} // namespace agent_rpc
