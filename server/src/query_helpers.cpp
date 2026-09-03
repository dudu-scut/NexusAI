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
#ifdef AGENT_RPC_ENABLE_MCP
#include <agent_rpc/mcp/rag/embedding_service.h>
#include <agent_rpc/mcp/rag/vector_index.h>
#endif
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

// P18 方向2 第三闸：embedding 去重索引。Hints 文本向量化后与已有条目
// 比余弦相似度，>0.9 的重复条目跳过写入。进程级单例、懒加载、mutex 保护；
// 仅 MCP 构建 + NEXUSAI_MEMORY_DEDUP_EMBEDDING=1 时启用；embedding 服务
// 未配置/失败时静默降级为不去重。
#ifdef AGENT_RPC_ENABLE_MCP
namespace {
struct HintDedupIndex {
    std::mutex mutex;
    std::unique_ptr<agent_rpc::mcp::rag::EmbeddingService> embedding;
    std::unique_ptr<agent_rpc::mcp::rag::VectorIndex> index;

    bool ensureInitializedLocked() {
        if (embedding) return true;
        try {
            agent_rpc::mcp::rag::EmbeddingConfig cfg;
            cfg.api_key = agent_rpc::common::envOrDefault("LLM_API_KEY", "");
            cfg.model = agent_rpc::common::envOrDefault("EMBEDDING_MODEL", "");
            cfg.api_url = agent_rpc::common::envOrDefault("EMBEDDING_API_URL", "");
            if (cfg.api_key.empty() && cfg.api_url.empty()) {
                return false;  // embedding not configured — degrade silently
            }
            embedding = std::make_unique<agent_rpc::mcp::rag::EmbeddingService>(cfg);
            index = std::make_unique<agent_rpc::mcp::rag::VectorIndex>();
            return true;
        } catch (const std::exception&) {
            embedding.reset();
            index.reset();
            return false;
        }
    }

    bool isDuplicate(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!ensureInitializedLocked()) return false;
        try {
            auto vec = embedding->embed(text);
            auto results = index->search(vec, 1, 0.9f);
            return !results.empty();
        } catch (const std::exception&) {
            return false;  // embedding failure — never blocks memory writes
        }
    }

    void remember(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!ensureInitializedLocked()) return;
        try {
            agent_rpc::mcp::rag::IndexedTool tool;
            tool.name = text;
            tool.embedding = embedding->embed(text);
            index->addTool(std::move(tool));
        } catch (const std::exception&) {
            // Best-effort: a missed dedup entry only costs a future embed.
        }
    }
};

HintDedupIndex& hintDedupIndex() {
    static HintDedupIndex instance;
    return instance;
}
}  // anonymous namespace
#endif

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

void QueryHelpers::maybeExtractMemorySegment(
    common::MemoryService* memory_service,
    void* memory_llm_client,
    common::QueryDomainRepository* domain_repo,
    const std::string& user_id,
    const std::string& context_id,
    int message_count) {

    if (!memory_service || user_id.empty() || context_id.empty()) {
        return;
    }

    // Trigger: every kSegmentMessages persisted messages (4 user turns at
    // 2 messages per turn). The segment index derives from the count, so
    // the dedup key is deterministic for retries.
    constexpr int kMessagesPerTurn = 2;
    constexpr int kSegmentTurns = 4;
    constexpr int kSegmentMessages = kMessagesPerTurn * kSegmentTurns;
    if (message_count <= 0 || message_count % kSegmentMessages != 0) {
        return;
    }
    const int segment_idx = message_count / kSegmentMessages;
    const std::string dedup_key =
        context_id + ":" + std::to_string(segment_idx);

    // Incremental-extraction guard: only ONE extraction per segment (set
    // covers reentrancy and races; process-level, so a restart may extract
    // one segment twice — the extraction is idempotent).
    reapFinishedSummaries();
    {
        std::lock_guard<std::mutex> lock(memory_llm_mutex);
        if (!segment_in_progress.insert(dedup_key).second) {
            return;
        }
    }

    // Material source: PostgreSQL conversation history (authoritative).
    std::string segment_history;
    if (domain_repo) {
        const auto messages = domain_repo->listMessages(user_id, context_id);
        const std::size_t start = messages.size() > kSegmentMessages
                                      ? messages.size() - kSegmentMessages : 0;
        for (std::size_t index = start; index < messages.size(); ++index) {
            segment_history += messages[index].role + ": " +
                               messages[index].content + "\n";
        }
    }
    if (segment_history.empty()) {
        std::lock_guard<std::mutex> lock(memory_llm_mutex);
        segment_in_progress.erase(dedup_key);
        return;
    }

    // Existing memory is fed back so the LLM can output ONLY new/changed
    // hints (incremental extraction — the primary dedup gate of P18 方向2).
    const std::string existing_memory = memory_service->getUserMemory(user_id);

    const std::string prompt =
        "你是用户长期记忆提取器。基于本轮对话提取值得长期记住的用户偏好与事实。\n"
        "已存在的记忆：\n" + (existing_memory.empty()
                                   ? "（无）" : existing_memory) +
        "\n\n只输出【新增或变更】的记忆条目；已存在且未变化的条目一律不要输出。\n"
        "严格以 JSON 输出，不要输出任何其他内容：\n"
        "{\"hints\": {\"键\": \"值\"}}（没有新增条目时输出 {\"hints\": {}}）";

    LLMClient* llm = static_cast<LLMClient*>(memory_llm_client);
    if (!llm) {
        std::lock_guard<std::mutex> lock(memory_llm_mutex);
        segment_in_progress.erase(dedup_key);
        return;
    }

    std::future<void> task;
    try {
        task = std::async(std::launch::async,
            [this, llm, prompt, segment_history, memory_service,
             user_id, context_id, dedup_key]() {
                try {
                    const std::string reply = llm->chat(prompt, segment_history);
                    auto parsed = nlohmann::json::parse(reply, nullptr, false);
                    // P18 方向2 第三闸：embedding 去重（MCP 构建 + 开关）。
                    const bool dedup_enabled =
                        agent_rpc::common::envOrDefault(
                            "NEXUSAI_MEMORY_DEDUP_EMBEDDING", "0") == "1";
                    std::map<std::string, std::string> hints;
                    if (parsed.is_object() && parsed.contains("hints") &&
                        parsed["hints"].is_object()) {
                        for (auto it = parsed["hints"].begin();
                             it != parsed["hints"].end(); ++it) {
                            if (!it.value().is_string()) continue;
                            const std::string hint_text =
                                it.key() + ": " + it.value().get<std::string>();
#ifdef AGENT_RPC_ENABLE_MCP
                            if (dedup_enabled &&
                                hintDedupIndex().isDuplicate(hint_text)) {
                                continue;  // semantically identical hint exists
                            }
#endif
                            hints[it.key()] = it.value().get<std::string>();
                        }
                    }
                    if (!hints.empty()) {
                        memory_service->updateUserMemoryFromHints(user_id, hints);
#ifdef AGENT_RPC_ENABLE_MCP
                        if (dedup_enabled) {
                            for (const auto& [k, v] : hints) {
                                hintDedupIndex().remember(k + ": " + v);
                            }
                        }
#endif
                        LOG_INFO("Memory segment extracted for context: " +
                                 context_id + " (" +
                                 std::to_string(hints.size()) + " hints)");
                    }
                } catch (const std::exception& e) {
                    LOG_WARN("Failed to extract memory segment: " +
                             std::string(e.what()));
                } catch (...) {
                    LOG_WARN("Failed to extract memory segment: unknown error");
                }
                std::lock_guard<std::mutex> lock(memory_llm_mutex);
                segment_in_progress.erase(dedup_key);
            });
    } catch (const std::exception& e) {
        LOG_WARN("Failed to launch memory segment task: " + std::string(e.what()));
        std::lock_guard<std::mutex> lock(memory_llm_mutex);
        segment_in_progress.erase(dedup_key);
        return;
    }

    try {
        std::lock_guard<std::mutex> lock(futures_mutex_);
        pending_summaries_.push_back(std::move(task));
    } catch (...) {
        std::lock_guard<std::mutex> lock(memory_llm_mutex);
        segment_in_progress.erase(dedup_key);
    }
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
