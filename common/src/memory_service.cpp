#include "agent_rpc/common/memory_service.h"
#include "agent_rpc/common/profile_summarizer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>

namespace agent_rpc {
namespace common {

MemoryService::MemoryService(std::shared_ptr<RedisClient> redis)
    : redis_(std::move(redis)) {}

// Tier 1: conversation history (Redis list, JSON-encoded messages)
void MemoryService::appendMessage(const std::string& context_id,
                                    const std::string& agent_id,
                                    const std::string& role,
                                    const std::string& content) {
    auto key = convKey(context_id, agent_id);
    auto ts = std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();

    nlohmann::json msg;
    msg["role"] = role;
    msg["content"] = content;
    msg["ts"] = ts;
    redis_->rpush(key, msg.dump());

    // Trim to max size
    redis_->ltrim(key, -kMaxHistoryPerAgent, -1);
    // Update last active agent
    redis_->set(lastAgentKey(context_id), agent_id);
}

std::string MemoryService::getConversationHistory(
    const std::string& context_id,
    const std::string& agent_id,
    int max_messages) const {
    auto key = convKey(context_id, agent_id);
    std::vector<std::string> raw;
    redis_->lrange(key, -max_messages, -1, raw);
    if (raw.empty()) return "";
    return formatHistory(raw, max_messages);
}

std::string MemoryService::getLastAgent(const std::string& context_id) const {
    std::string agent;
    redis_->get(lastAgentKey(context_id), agent);
    return agent;
}

void MemoryService::setLastAgent(const std::string& context_id,
                                   const std::string& agent_id) {
    redis_->set(lastAgentKey(context_id), agent_id);
}

// Tier 2: user long-term memory (Redis hash)
void MemoryService::setUserMemory(const std::string& user_id,
                                    const std::string& key,
                                    const std::string& value) {
    redis_->hset(memoryKey(user_id), key, value);
}

std::string MemoryService::getUserMemory(const std::string& user_id) const {
    std::map<std::string, std::string> all;
    if (!redis_->hgetall(memoryKey(user_id), all) || all.empty()) return "";

    std::ostringstream oss;
    for (const auto& [key, value] : all) {
        oss << "- " << key << ": " << value << "\n";
    }
    return oss.str();
}

void MemoryService::updateUserMemoryFromHints(
    const std::string& user_id,
    const std::map<std::string, std::string>& hints) {
    if (hints.empty() || user_id.empty()) return;

    auto key = memoryKey(user_id);
    for (const auto& [k, v] : hints) {
        if (v.empty()) {
            redis_->hdel(key, k);
        } else {
            redis_->hset(key, k, v);
        }
    }
}

// Cross-agent summary (Redis string)
void MemoryService::setCrossAgentSummary(const std::string& context_id,
                                            const std::string& summary) {
    redis_->set(summaryKey(context_id), summary);
}

std::string MemoryService::getCrossAgentSummary(
    const std::string& context_id) const {
    std::string summary;
    redis_->get(summaryKey(context_id), summary);
    return summary;
}

void MemoryService::setCrossAgentSummaryFor(const std::string& context_id,
                                            const std::string& agent_id,
                                            const std::string& summary,
                                            int ttl_seconds) {
    // SETEX: the agent-specialized summary is a cache entry with a bounded
    // lifetime (default 7 days), keyed per taking-over agent.
    redis_->setex(summaryKeyFor(context_id, agent_id), ttl_seconds, summary);
}

std::string MemoryService::getCrossAgentSummaryFor(
    const std::string& context_id, const std::string& agent_id) const {
    std::string summary;
    redis_->get(summaryKeyFor(context_id, agent_id), summary);
    return summary;
}

// SystemContext construction
agent_communication::SystemContext MemoryService::buildSystemContext(
    const std::string& user_id,
    const std::string& context_id,
    const std::string& agent_id,
    int max_history) const {

    agent_communication::SystemContext ctx;
    ctx.set_user_id(user_id);

    // Build user profile summary
    std::string profile_prefix;
    if (!user_id.empty()) {
        std::string profile_raw;
        if (redis_->get(profileKey(user_id), profile_raw) && !profile_raw.empty()) {
            try {
                auto profile = nlohmann::json::parse(profile_raw);
                std::string identity_json = profile.value("identity", "{}");
                std::string preferences_json = profile.value("preferences", "[]");
                std::string summary = ProfileSummarizer::summarize(
                    identity_json, preferences_json);
                if (!summary.empty()) {
                    profile_prefix = "[User Profile] " + summary + "\n";
                }
            } catch (const nlohmann::json::exception&) {
                // Malformed profile — skip silently
            }
        }
    }

    // Tier 2: user long-term memory, prefixed with the profile summary when available
    ctx.set_user_memory(profile_prefix + getUserMemory(user_id));

    // Tier 1: current agent conversation history (skip when agent_id is empty, i.e. pre-routing)
    if (!agent_id.empty()) {
        ctx.set_conversation_history(
            getConversationHistory(context_id, agent_id, max_history));
    }

    ctx.set_cross_agent_summary(getCrossAgentSummary(context_id));

    return ctx;
}

std::string MemoryService::formatHistory(
    const std::vector<std::string>& raw_messages,
    int max_messages) {
    if (raw_messages.empty()) return "";

    int start = std::max(0, static_cast<int>(raw_messages.size()) - max_messages);

    std::ostringstream oss;
    for (int i = start; i < static_cast<int>(raw_messages.size()); ++i) {
        try {
            auto j = nlohmann::json::parse(raw_messages[i]);
            std::string role = j.value("role", "agent");
            std::string content = j.value("content", "");
            oss << (role == "user" ? "用户: " : "助手: ") << content << "\n";
        } catch (...) {
            // Fallback: treat raw string as content
            oss << "助手: " << raw_messages[i] << "\n";
        }
    }
    return oss.str();
}

}  // namespace common
}  // namespace agent_rpc
