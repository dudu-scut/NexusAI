#pragma once

#include "agent_rpc/common/redis_client.h"
#include "ai_query.pb.h"

#include <string>

namespace agent_rpc {
namespace common {

/**
 * Multi-tier memory service (Redis-backed).
 *
 * Tier 1 — conversation history: sharded by (context_id, agent_id), stored as
 *           a Redis list of JSON-encoded messages.
 * Tier 2 — user long-term memory: stored per user_id in a Redis hash.
 * Cross-agent summary: context summary produced on agent switch, stored as a
 *           Redis string.
 */
class MemoryService {
public:
    explicit MemoryService(std::shared_ptr<RedisClient> redis);
    ~MemoryService() = default;

    struct Message {
        std::string role;     // "user" | "agent"
        std::string content;
        int64_t timestamp;
    };

    /** Appends one message to the conversation history */
    void appendMessage(const std::string& context_id,
                       const std::string& agent_id,
                       const std::string& role,
                       const std::string& content);

    /** Returns the (context_id, agent_id) conversation history formatted as text */
    std::string getConversationHistory(const std::string& context_id,
                                       const std::string& agent_id,
                                       int max_messages = 10) const;

    /** Returns the last agent_id used for this context_id */
    std::string getLastAgent(const std::string& context_id) const;

    /** Records the current agent as the last active agent for this context_id */
    void setLastAgent(const std::string& context_id, const std::string& agent_id);

    /** Sets a single key-value pair in the user's long-term memory */
    void setUserMemory(const std::string& user_id,
                       const std::string& key,
                       const std::string& value);

    /** Returns all user long-term memory formatted as text */
    std::string getUserMemory(const std::string& user_id) const;

    /** Batch-updates user memory from agent-reported memory hints */
    void updateUserMemoryFromHints(
        const std::string& user_id,
        const std::map<std::string, std::string>& hints);

    /** Sets the cross-agent switch summary */
    void setCrossAgentSummary(const std::string& context_id,
                               const std::string& summary);

    /** Gets the cross-agent switch summary */
    std::string getCrossAgentSummary(const std::string& context_id) const;

    /** Sets the cross-agent summary specialized for the taking-over agent.
     *  Stored under nexusai:summary:<ctx>:<agent> with a TTL (SETEX). */
    void setCrossAgentSummaryFor(const std::string& context_id,
                                  const std::string& agent_id,
                                  const std::string& summary,
                                  int ttl_seconds = kCrossAgentSummaryTtlSeconds);

    /** Gets the agent-specialized cross-agent summary ("" when absent) */
    std::string getCrossAgentSummaryFor(const std::string& context_id,
                                         const std::string& agent_id) const;

    /** Builds the full SystemContext for injection into AIQueryRequest */
    agent_communication::SystemContext buildSystemContext(
        const std::string& user_id,
        const std::string& context_id,
        const std::string& agent_id,
        int max_history = 10) const;

private:
    // Sanitize a key component for safe use in colon-delimited Redis keys.
    // Replaces ':' with '_' to prevent key namespace boundary injection,
    // and strips control characters (newlines, nulls, etc.).
    static std::string sanitizeKeyComponent(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == ':')       out += '_';
            else if (c == '\n') out += ' ';
            else if (c == '\r') {}           // strip CR
            else if (c >= 0x01 && c < 0x20) {}  // strip control chars
            else out += c;
        }
        return out;
    }

    // Redis key helpers — all components sanitized to prevent injection.
    //
    // P23 键分类约定（存储分层治理，2026-09-03）：
    //   - [事实源]（PG 无表、暂居 Redis，待 V014 迁移）——
    //       nexusai:memory:<uid>（Tier-2 长期记忆 hints）
    //       nexusai:summary:*（Tier-3 跨 Agent 摘要，7 天 TTL）
    //       user_profile:<uid>（用户画像）
    //   - [cache-only]（PG 为事实源的投影，丢失可重建）——
    //       nexusai:conv:*（Tier-1 会话镜像，主查询读 PG）
    //   - [transient]（无业务后果的瞬态/协调键）——
    //       nexusai:last_agent:*、限流计数、分布式短锁、预算实时计数面
    //   约定：不再新增事实源键；新增键必须按上述三类标注归类。
    static std::string convKey(const std::string& ctx, const std::string& agent) {
        return "nexusai:conv:" + sanitizeKeyComponent(ctx) + ":" + sanitizeKeyComponent(agent);
    }
    static std::string lastAgentKey(const std::string& ctx) {
        return "nexusai:last_agent:" + sanitizeKeyComponent(ctx);
    }
    static std::string memoryKey(const std::string& uid) {
        return "nexusai:memory:" + sanitizeKeyComponent(uid);
    }
    static std::string summaryKey(const std::string& ctx) {
        return "nexusai:summary:" + sanitizeKeyComponent(ctx);
    }
    static std::string summaryKeyFor(const std::string& ctx, const std::string& agent) {
        return "nexusai:summary:" + sanitizeKeyComponent(ctx) + ":" + sanitizeKeyComponent(agent);
    }
    static std::string profileKey(const std::string& uid) {
        return "user_profile:" + sanitizeKeyComponent(uid);
    }

    static constexpr int kMaxHistoryPerAgent = 50;
    // Agent-specialized cross-agent summaries expire after 7 days.
    static constexpr int kCrossAgentSummaryTtlSeconds = 7 * 24 * 3600;

    std::shared_ptr<RedisClient> redis_;  // shared ownership prevents use-after-free

    static std::string formatHistory(const std::vector<std::string>& raw_messages,
                                      int max_messages);
};

}  // namespace common
}  // namespace agent_rpc
