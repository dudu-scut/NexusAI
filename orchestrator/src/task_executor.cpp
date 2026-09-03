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

namespace agent_rpc {
namespace orchestrator {

TaskExecutor::TaskExecutor(AgentRouter& router, const ExecutorConfig& config)
    : router_(router)
    , config_(config)
{}

std::unordered_map<std::string, SubTaskResult> TaskExecutor::execute(
    const ExecutionPlan& plan,
    const AgentCallFn& call_agent,
    const ProgressCallback& on_progress,
    const CancelFn& on_cancel) {

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
                        r.error_message = "Global timeout exceeded";
                        results[tid] = std::move(r);
                    }
                }
            }
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
                r.error_message = "Global timeout exceeded";
                results[tid] = std::move(r);
                // Mark remaining layers as timed out
                for (size_t li = layer_idx + 1; li < layers.size(); ++li) {
                    for (const auto& rtid : layers[li]) {
                        if (results.find(rtid) == results.end()) {
                            SubTaskResult rr;
                            rr.subtask_id = rtid;
                            rr.success = false;
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

            auto fut = std::async(std::launch::async,
                [this, &st, p = std::move(prompt), &call_agent,
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
                });

            SubTaskResult result;
            auto remaining_now = std::chrono::duration_cast<std::chrono::milliseconds>(
                global_deadline - std::chrono::steady_clock::now());
            auto wait_for = std::min(subtask_cap, remaining_now);
            auto status = fut.wait_for(wait_for);
            if (status == std::future_status::ready) {
                result = fut.get();
            } else {
                result.subtask_id = tid;
                result.success = false;
                result.error_message = "Subtask timeout exceeded";
                // P20: abort the in-flight A2A call instead of leaving a
                // zombie thread blocked until its HTTP timeout.
                if (on_cancel && !cancel_url.empty()) {
                    on_cancel(cancel_url);
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
            // Multiple subtasks — execute in parallel via std::async
            std::vector<std::pair<std::string, std::future<SubTaskResult>>> futures;

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

                // Capture st by reference (valid throughout layer execution)
                // and prompt by value (moved into lambda)
                futures.emplace_back(tid,
                    std::async(std::launch::async,
                        [this, &st, p = std::move(prompt), &call_agent,
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
                        }));
            }

            // Collect results with deadline awareness (fixes #18: DAG global timeout
            // now actually cancels waiting on incomplete async tasks instead of
            // blocking indefinitely on fut.get())
            for (auto& [tid, fut] : futures) {
                try {
                    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                        global_deadline - std::chrono::steady_clock::now());

                    SubTaskResult result;
                    if (remaining <= std::chrono::milliseconds(0)) {
                        // Deadline already passed — mark as timed out
                        result.subtask_id = tid;
                        result.success = false;
                        result.error_message = "Global timeout exceeded";
                    } else {
                        // P20: wait no longer than the per-subtask cap OR the
                        // remaining global budget, whichever is tighter.
                        auto wait_for = std::min(subtask_cap, remaining);
                        auto status = fut.wait_for(wait_for);
                        if (status == std::future_status::ready) {
                            result = fut.get();
                        } else {
                            result.subtask_id = tid;
                            result.success = false;
                            result.error_message = "Global timeout exceeded (task did not complete in time)";
                            // P20: abort the in-flight A2A call.
                            auto target_it = layer_targets.find(tid);
                            if (on_cancel && target_it != layer_targets.end() &&
                                !target_it->second.first.empty()) {
                                on_cancel(target_it->second.first);
                            }
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

    for (const auto& t : tasks) {
        if (in_degree.find(t.id) == in_degree.end()) {
            in_degree[t.id] = 0;
        }
        for (const auto& dep : t.depends_on) {
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
    size_t total_emitted = 0;
    for (const auto& layer : layers) {
        total_emitted += layer.size();
    }
    if (total_emitted < tasks.size()) {
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
