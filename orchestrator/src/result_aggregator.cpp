/**
 * @file result_aggregator.cpp
 * @brief ResultAggregator implementation
 */

#include "agent_rpc/orchestrator/result_aggregator.h"
#include <algorithm>
#include <chrono>
#include <sstream>

namespace agent_rpc {
namespace orchestrator {

ResultAggregator::ResultAggregator(const AggregatorConfig& config)
    : config_(config) {
    if (config.default_strategy == "llm_synthesize" && !config.api_key.empty()) {
        llm_client_ = std::make_unique<LLMClient>(config.api_key, config.model, config.api_url);
    }
}

AggregatedResult ResultAggregator::aggregate(
    const ExecutionPlan& plan,
    const std::unordered_map<std::string, SubTaskResult>& results,
    const std::atomic<bool>* abort_flag) {

    auto start = std::chrono::steady_clock::now();

    AggregatedResult agg;
    agg.strategy = config_.default_strategy;

    // P13(d)/A2: a plan that dropped subtasks (LLM format drift) must not
    // silently lose those requirements — surface a warning in both the
    // answer path and the structured warnings field.
    if (plan.dropped_tasks > 0) {
        agg.warnings.push_back(
            "[系统] 有 " + std::to_string(plan.dropped_tasks) +
            " 个子任务因规划输出不完整被丢弃，部分需求可能未被覆盖。");
    }
    // B3/P20-7: a write-shaped task that timed out may have applied its
    // external effect — never present it as a plain retryable failure.
    for (const auto& entry : results) {
        if (entry.second.uncertain) {
            agg.warnings.push_back(
                "[系统] 子任务 " + entry.first +
                " 超时且可能已执行外部写入，其最终状态未知。");
        }
    }

    // Collect sub_results in task order
    for (const auto& task : plan.tasks) {
        auto it = results.find(task.id);
        if (it != results.end()) {
            agg.sub_results.push_back(it->second);
        }
    }

    // Route to strategy
    if (config_.default_strategy == "llm_synthesize" && llm_client_) {
        auto [answer, used_fallback] = aggregateLLMSynthesize(plan, results, abort_flag);
        agg.final_answer = answer;
        // Label the strategy truthfully: the internal concat fallback must
        // not be reported as a successful llm_synthesize run.
        if (used_fallback || agg.final_answer.empty()) {
            if (agg.final_answer.empty()) {
                agg.final_answer = aggregateConcat(plan, results);
            }
            agg.strategy = "concat";
        }
    } else {
        agg.final_answer = aggregateConcat(plan, results);
        agg.strategy = "concat";
    }

    // Append the dropped-tasks notice to the answer itself so the user sees
    // it regardless of how the frontend renders metadata.
    for (const auto& warning : agg.warnings) {
        if (!agg.final_answer.empty()) {
            agg.final_answer += "\n\n";
        }
        agg.final_answer += warning;
    }

    auto end = std::chrono::steady_clock::now();
    agg.total_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    return agg;
}

std::string ResultAggregator::aggregateConcat(
    const ExecutionPlan& plan,
    const std::unordered_map<std::string, SubTaskResult>& results) const {

    // Concatenate results in topological order (plan.tasks order)
    std::ostringstream out;
    bool first = true;

    for (const auto& task : plan.tasks) {
        auto it = results.find(task.id);
        if (it == results.end()) continue;

        const auto& r = it->second;
        if (!r.success && !r.uncertain) continue;  // Skip failed subtasks

        if (!first) {
            out << "\n\n";
        }
        first = false;

        out << "## " << task.id << ": " << task.description << "\n\n";
        if (r.uncertain) {
            // B3/P20-7: make the unknown outcome explicit instead of skipping.
            out << "[状态未知：" << r.error_message << "]\n";
        } else {
            out << r.result;
        }
    }

    if (first) {
        // All subtasks failed
        out << "所有子任务均未成功完成。";
        for (const auto& task : plan.tasks) {
            auto it = results.find(task.id);
            if (it != results.end() && !it->second.success) {
                out << "\n- " << task.id << ": " << it->second.error_message;
            }
        }
    }

    return out.str();
}

std::pair<std::string, bool> ResultAggregator::aggregateLLMSynthesize(
    const ExecutionPlan& plan,
    const std::unordered_map<std::string, SubTaskResult>& results,
    const std::atomic<bool>* abort_flag) {

    // Build context from all successful subtask results
    std::string context;
    for (const auto& task : plan.tasks) {
        auto it = results.find(task.id);
        if (it == results.end()) continue;

        const auto& r = it->second;
        if (!r.success && !r.uncertain) continue;

        context += "[" + task.id + " - " + task.description + "]\n";
        if (r.uncertain) {
            context += "[状态未知：" + r.error_message + "]\n";
        } else {
            context += r.result + "\n\n";
        }
    }

    if (context.empty()) {
        // Fall back to concat if no successful results
        return {aggregateConcat(plan, results), true};
    }

    std::string system_prompt =
        "你是一个智能助手。以下是多个专业 Agent 协作完成用户请求后的各自结果。\n"
        "请将这些结果综合整理，给出一个完整、连贯、有条理的最终回答。\n"
        "不要提及'子任务'、'Agent'等内部概念，直接回答用户的问题。";

    std::string user_message =
        "用户的原始请求：" + plan.original_query + "\n\n"
        "各子任务结果：\n" + context +
        "\n请综合以上内容，给出最终回答。";

    // P13(d)/A2: tell the synthesizer about dropped subtasks so the final
    // answer itself carries the disclaimer (fail-soft does not mean silent).
    if (plan.dropped_tasks > 0) {
        user_message += "\n\n注意：有 " + std::to_string(plan.dropped_tasks) +
            " 个子任务因规划输出不完整被丢弃，请在回答末尾用一句话提示用户"
            "部分需求可能未被覆盖。";
    }

    try {
        std::string answer =
            llm_client_->chat(system_prompt, user_message,
                              LLMClient::kDefaultChatTimeoutSeconds, abort_flag);
        if (answer.empty()) {
            // LLM returned empty response — fall back to concat
            return {aggregateConcat(plan, results), true};
        }
        return {answer, false};
    } catch (const std::exception&) {
        // LLM synthesis failed — fall back to concat
        return {aggregateConcat(plan, results), true};
    }
}

} // namespace orchestrator
} // namespace agent_rpc
