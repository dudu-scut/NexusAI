/**
 * @file query_helpers.cpp
 * @brief Implementation of QueryHelpers (extracted from ai_query_service.cpp)
 */

#include "agent_rpc/server/query_helpers.h"
#include "agent_rpc/common/metrics.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/env_loader.h"
#include "agent_rpc/common/memory_service.h"
#include "agent_rpc/common/query_domain_repository.h"
#include <a2a/llm_client.hpp>
#include "ai_query.pb.h"

#include <future>

#ifdef _WIN32
#include <objbase.h>
#include <rpc.h>
#pragma comment(lib, "rpcrt4.lib")
#else
#include <uuid/uuid.h>
#endif

namespace agent_rpc {
namespace server {

namespace {
constexpr std::size_t kSummaryHistoryLimit = 20;
}  // namespace

QueryHelpers::~QueryHelpers() {
    // Drain pending summary tasks so destruction never races with a live
    // async worker and no detached task outlives this object.
    std::vector<std::future<void>> pending;
    {
        std::lock_guard<std::mutex> lock(futures_mutex_);
        pending.swap(pending_summaries_);
    }
    for (auto& future : pending) {
        if (future.valid()) {
            future.wait();  // tasks swallow their own exceptions
        }
    }
}

void QueryHelpers::reapFinishedSummaries() {
    std::lock_guard<std::mutex> lock(futures_mutex_);
    for (auto it = pending_summaries_.begin(); it != pending_summaries_.end();) {
        if (it->valid() &&
            it->wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            try {
                it->get();
            } catch (...) {
                // The task body already logs its own failures.
            }
            it = pending_summaries_.erase(it);
        } else {
            ++it;
        }
    }
}

std::string QueryHelpers::buildSummaryPrompt(const std::string& target_agent_name,
                                             const std::string& target_agent_duties) {
    std::string prompt =
        "你是一个对话摘要助手。请用2-3句话简洁总结以下用户与助手的对话要点，"
        "保留关键信息和上下文，以便下一个助手能够无缝接续对话。直接输出摘要，不要加前缀。";
    if (!target_agent_name.empty() || !target_agent_duties.empty()) {
        prompt += "\n即将接手的助手职责是：";
        if (!target_agent_name.empty()) {
            prompt += target_agent_name;
        }
        if (!target_agent_duties.empty()) {
            prompt += "（技能/职责：" + target_agent_duties + "）";
        }
        prompt += "。只保留与其职责相关的信息。";
    }
    return prompt;
}

// The in-memory task-status cache was write-only: GetQueryStatus reads the
// durable PostgreSQL query_logs row and nothing else ever consumed the cache,
// so the cache was deleted. These entry points keep their signatures (the
// Query pipeline still calls them as state-transition hooks) but are no-ops.
void QueryHelpers::updateTaskStatus(
    const std::string& /*task_id*/,
    const std::string& /*state*/,
    const std::string& /*agent_id*/,
    const std::string& /*agent_name*/,
    const std::string& /*error_msg*/) {
    // Intentionally empty: durable status lives in PostgreSQL query_logs.
}

void QueryHelpers::cleanupExpiredTasks() {
    // Intentionally empty: nothing to clean up anymore (see updateTaskStatus).
}

void QueryHelpers::recordMetrics(
    const std::string& method,
    int64_t duration_ms,
    bool success) {

    auto& metrics = common::Metrics::getInstance();
    metrics.recordRpcRequest("AIQueryService", method, duration_ms);

    if (success) {
        metrics.recordRpcResponse("AIQueryService", method, 0);
    } else {
        metrics.recordRpcError("AIQueryService", method, "Error");
    }
}

std::string QueryHelpers::generateRequestId() {
#ifdef _WIN32
    UUID uuid;
    UuidCreate(&uuid);
    RPC_CSTR szUuid = nullptr;
    UuidToStringA(&uuid, &szUuid);
    std::string uuid_str(reinterpret_cast<const char*>(szUuid));
    RpcStringFreeA(&szUuid);
    return uuid_str;
#else
    uuid_t uuid;
    uuid_generate(uuid);

    char uuid_str[37];
    uuid_unparse_lower(uuid, uuid_str);

    return std::string(uuid_str);
#endif
}

std::string QueryHelpers::sanitizeErrorMessage(const std::string& msg) {
    if (msg.find("CURL error") != std::string::npos) {
        if (msg.find("Couldn't connect") != std::string::npos ||
            msg.find("couldn't connect") != std::string::npos) {
            return "Agent service is currently unreachable. Please verify the agent is running and try again later.";
        }
        if (msg.find("timeout") != std::string::npos ||
            msg.find("Timeout") != std::string::npos) {
            return "Agent service did not respond in time. Please try again later.";
        }
        if (msg.find("URL using bad") != std::string::npos ||
            msg.find("missing URL") != std::string::npos) {
            return "Invalid agent endpoint configuration. Please contact the administrator.";
        }
        return "Failed to connect to agent service. Please try again later.";
    }
    return msg;
}

void QueryHelpers::handleAgentSwitch(
    common::MemoryService* memory_service,
    void* memory_llm_client,
    common::QueryDomainRepository* domain_repo,
    const std::string& user_id,
    const std::string& context_id,
    const std::string& current_agent_id,
    const std::string& target_agent_name,
    const std::string& target_agent_duties) {

    if (!memory_service || user_id.empty() || context_id.empty() ||
        current_agent_id.empty()) {
        return;
    }

    // Harvest finished summary tasks first so the pending vector stays bounded.
    reapFinishedSummaries();

    const std::string last_agent = memory_service->getLastAgent(context_id);

    // Tier-3 cross-agent summary. NEXUSAI_CROSS_AGENT_SUMMARY (default on)
    // gates only the summary pipeline; the real last_agent recording below
    // always happens.
    const bool summary_enabled =
        common::envOrDefault("NEXUSAI_CROSS_AGENT_SUMMARY", "1") != "0";
    const bool agent_switched = !last_agent.empty() && last_agent != current_agent_id;

    if (summary_enabled && agent_switched) {
        // Agent switched — generate summary asynchronously
        LOG_INFO("Agent switch detected: " + last_agent + " → " + current_agent_id +
                 " (context: " + context_id + ")");

        // History source is PostgreSQL (the durable source of truth). The
        // legacy Redis Tier-1 list was write-only on this path and therefore
        // always returned empty; it remains only as a fallback when no
        // domain repository is wired in.
        std::string old_history;
        if (domain_repo) {
            const auto messages = domain_repo->listMessages(user_id, context_id);
            const std::size_t start = messages.size() > kSummaryHistoryLimit
                ? messages.size() - kSummaryHistoryLimit : 0;
            for (std::size_t index = start; index < messages.size(); ++index) {
                old_history += messages[index].role + ": " + messages[index].content + "\n";
            }
        } else {
            old_history = memory_service->getConversationHistory(
                context_id, last_agent, static_cast<int>(kSummaryHistoryLimit));
        }

        LLMClient* llm = static_cast<LLMClient*>(memory_llm_client);

        // Dedup key: when a summary specialized for the taking-over agent
        // already exists, skip the LLM round-trip entirely.
        const std::string existing_summary =
            memory_service->getCrossAgentSummaryFor(context_id, current_agent_id);
        const bool already_summarized = !existing_summary.empty();

        if (!old_history.empty() && already_summarized) {
            // Dedup hit: the specialized key (7-day TTL) may be fresher than
            // the legacy context-level recall key read by P1 — e.g. the
            // context key was overwritten by a later switch or externally
            // cleared. Refresh it so recall never injects a stale summary
            // within the dedup window. No LLM round-trip happens here.
            memory_service->setCrossAgentSummary(context_id, existing_summary);
        }

        if (!old_history.empty() && !already_summarized && (llm || summarize_fn)) {
            bool inserted = false;
            {
                // Critical section stays minimal: it guards only the
                // in-progress bookkeeping set. The LLM call runs outside.
                std::lock_guard<std::mutex> lock(memory_llm_mutex);
                inserted = summary_in_progress.insert(context_id).second;
            }
            if (inserted) {
                const std::string prompt =
                    buildSummaryPrompt(target_agent_name, target_agent_duties);
                const SummaryFn summary_fn = summarize_fn;  // copy into the task
                std::future<void> task;
                try {
                    task = std::async(std::launch::async,
                        [this, llm, summary_fn, prompt, old_history, memory_service,
                         context_id, current_agent_id]() {
                            try {
                                const std::string summary = summary_fn
                                    ? summary_fn(prompt, old_history)
                                    : llm->chat(prompt, old_history);
                                if (!summary.empty()) {
                                    // Agent-scoped key with TTL plus the legacy
                                    // context-only key, so the recall path
                                    // (getCrossAgentSummary) keeps seeing fresh
                                    // summaries.
                                    memory_service->setCrossAgentSummaryFor(
                                        context_id, current_agent_id, summary);
                                    memory_service->setCrossAgentSummary(context_id, summary);
                                    LOG_INFO("Cross-agent summary generated for context: " + context_id);
                                }
                            } catch (const std::exception& e) {
                                LOG_WARN("Failed to generate cross-agent summary: " + std::string(e.what()));
                            } catch (...) {
                                LOG_WARN("Failed to generate cross-agent summary: unknown error");
                            }
                            // Only the bookkeeping erase runs under the lock.
                            std::lock_guard<std::mutex> lock(memory_llm_mutex);
                            summary_in_progress.erase(context_id);
                        });
                } catch (const std::exception& e) {
                    // Launch failed (resource exhaustion etc.): roll back the
                    // in-progress marker, otherwise this context_id would be
                    // permanently barred from summary generation.
                    LOG_WARN("Failed to launch cross-agent summary task: " + std::string(e.what()));
                    std::lock_guard<std::mutex> lock(memory_llm_mutex);
                    summary_in_progress.erase(context_id);
                } catch (...) {
                    LOG_WARN("Failed to launch cross-agent summary task: unknown error");
                    std::lock_guard<std::mutex> lock(memory_llm_mutex);
                    summary_in_progress.erase(context_id);
                }
                try {
                    std::lock_guard<std::mutex> lock(futures_mutex_);
                    pending_summaries_.push_back(std::move(task));
                } catch (...) {
                    // push_back threw (bad_alloc): the future would otherwise
                    // block on destruction; roll back the marker as well.
                    std::lock_guard<std::mutex> lock(memory_llm_mutex);
                    summary_in_progress.erase(context_id);
                }
            }
        }
    }

    // Converged last_agent write: always record the real agent id. The
    // streaming path no longer writes a fake "default" placeholder.
    memory_service->setLastAgent(context_id, current_agent_id);
}

std::string QueryHelpers::buildMemoryContext(
    const agent_communication::AIQueryRequest* request) {
    std::string memory_ctx;
    if (request->has_system_context()) {
        const auto& sys_ctx = request->system_context();
        if (!sys_ctx.user_memory().empty()) {
            memory_ctx += "[User Context]\n" + sys_ctx.user_memory() + "\n";
        }
        if (!sys_ctx.cross_agent_summary().empty()) {
            memory_ctx += "[Prior Context]\n" + sys_ctx.cross_agent_summary() + "\n";
        }
    }
    return memory_ctx;
}

} // namespace server
} // namespace agent_rpc
