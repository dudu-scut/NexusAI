#include "agent_rpc/common/memory_service.h"
#include "agent_rpc/common/profile_summarizer.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/query_domain_repository.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <sstream>

namespace agent_rpc {
namespace common {

MemoryService::MemoryService(std::shared_ptr<RedisClient> redis,
                             QueryDomainRepository* domain_repo)
    : redis_(std::move(redis))
    , domain_repo_(domain_repo) {}

// C2 direction 3: fact-type keys follow a rule-based overwrite policy (the
// previous value is preserved into the PG history column, no LLM call);
// preference-type keys collect conflicting candidates for disambiguation
// instead of silently overwriting.
namespace {
bool isValidHintKey(const std::string& key);  // defined below (P18 gate 2)
const char* const kFactKeys[] = {
    "occupation", "role", "employer", "company", "title", "job",
    "location", "city", "country", "education", "school", "degree",
    "language", "timezone", "email", "phone",
};
bool isInList(const char* const* list, size_t size, const std::string& key) {
    for (size_t i = 0; i < size; ++i) {
        if (key == list[i]) return true;
    }
    return false;
}
}  // anonymous namespace

bool MemoryService::isFactKey(const std::string& key) {
    return isInList(kFactKeys, sizeof(kFactKeys) / sizeof(kFactKeys[0]), key);
}

bool MemoryService::isPreferenceKey(const std::string& key) {
    static const char* const kPreferenceKeys[] = {
        "communication_style", "response_style", "verbosity", "tone",
        "detail_level", "language_preference",
    };
    if (isInList(kPreferenceKeys,
                 sizeof(kPreferenceKeys) / sizeof(kPreferenceKeys[0]), key)) {
        return true;
    }
    return key.rfind("pref_", 0) == 0;
}

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

// Tier 2: user long-term memory - PG (V015) durable + Redis projection.
void MemoryService::setUserMemory(const std::string& user_id,
                                    const std::string& key,
                                    const std::string& value) {
    if (!isValidHintKey(key)) {
        LOG_WARN("MemoryService: dropping setUserMemory with invalid key '" + key + "'");
        return;
    }
    if (domain_repo_) {
        try {
            UserMemoryHintRecord hint;
            hint.owner_id = user_id;
            hint.key = key;
            hint.value = value;
            hint.source = "direct";
            // Fact keys preserve their previous value in the PG history
            // column (C2 direction 3, rule-based conflict policy).
            if (domain_repo_->upsertUserMemoryHint(hint, isFactKey(key))) {
                UserMemoryEventRecord event;
                event.owner_id = user_id;
                event.key = key;
                event.value = value;
                event.op = "upsert";
                domain_repo_->insertUserMemoryEvent(event);
                // C1: PG success -> invalidate the Redis projection.
                redis_->del(memoryKey(user_id));
                return;
            }
        } catch (const std::exception& e) {
            LOG_WARN("MemoryService: PG hint write failed, falling back to Redis: " +
                     std::string(e.what()));
        }
    }
    redis_->hset(memoryKey(user_id), key, value);
}

std::string MemoryService::getUserMemory(const std::string& user_id) const {
    // C1: PG is the source; Redis is a projection rebuilt on PG miss.
    if (domain_repo_) {
        try {
            const auto hints = domain_repo_->listUserMemoryHints(user_id);
            if (!hints.empty()) {
                std::ostringstream oss;
                for (const auto& hint : hints) {
                    oss << "- " << hint.key << ": " << hint.value << "\n";
                }
                return oss.str();
            }
        } catch (const std::exception& e) {
            LOG_WARN("MemoryService: PG hint read failed, falling back to Redis: " +
                     std::string(e.what()));
        }
    }
    std::map<std::string, std::string> all;
    const bool redis_hit =
        redis_->hgetall(memoryKey(user_id), all) && !all.empty();
    if (!redis_hit) return "";

    std::ostringstream oss;
    for (const auto& [key, value] : all) {
        oss << "- " << key << ": " << value << "\n";
    }
    const std::string text = oss.str();
    // Cache-aside rebuild: a Redis-only hint set (pre-V015 data) is
    // backfilled into PG so the durable source converges.
    if (domain_repo_) {
        for (const auto& [key, value] : all) {
            UserMemoryHintRecord hint;
            hint.owner_id = user_id;
            hint.key = key;
            hint.value = value;
            hint.source = "redis-backfill";
            // P2-1: insert-if-absent — a stale projection read must never
            // overwrite a newer durable value written between our PG miss
            // and this backfill.
            try {
                domain_repo_->insertUserMemoryHintIfAbsent(hint);
            } catch (const std::exception&) {
                // Lost insert-if-absent race = the durable value is newer;
                // absorb and keep backfilling the remaining keys.
            }
        }
    }
    return text;
}

// P18 方向2 第二闸：hints 键受控枚举。合法键 = ASCII 小写字母/数字/下划线/
// 连字符或 UTF-8 多字节（中文键）；长度 1-64；写入前校验，非法键静默丢弃
// （防同义不同键污染与 ':' 等键注入字节）。
namespace {
bool isValidHintKey(const std::string& key) {
    if (key.empty() || key.size() > 64) return false;
    for (const unsigned char c : key) {
        const bool ascii_ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                              c == '_' || c == '-';
        const bool utf8 = c >= 0x80;
        if (!ascii_ok && !utf8) return false;
    }
    return true;
}
}  // anonymous namespace

void MemoryService::updateUserMemoryFromHints(
    const std::string& user_id,
    const std::map<std::string, std::string>& hints) {
    if (hints.empty() || user_id.empty()) return;

    auto key = memoryKey(user_id);

    // C1: durable path — PG is the source, Redis is invalidated on success.
    if (domain_repo_) {
        try {
            // One round-trip for the current values (conflict detection for
            // preference keys, C2 direction 3).
            std::map<std::string, std::string> current;
            for (const auto& hint : domain_repo_->listUserMemoryHints(user_id)) {
                current[hint.key] = hint.value;
            }
            std::vector<UserMemoryHintOp> ops;
            for (const auto& [k, v] : hints) {
                if (!isValidHintKey(k)) {
                    LOG_WARN("MemoryService: dropping hint with invalid key '" + k + "'");
                    continue;
                }
                UserMemoryHintOp op;
                op.key = k;
                op.value = v;
                op.source = "segment";
                if (v.empty()) {
                    op.kind = UserMemoryHintOp::Kind::Delete;
                    op.event_op = "delete";
                } else {
                    op.kind = UserMemoryHintOp::Kind::Upsert;
                    // Fact keys preserve the previous value in history; a
                    // preference CONFLICT must also keep the old value — the
                    // event row alone loses the disambiguation input (P2-3).
                    op.append_history = isFactKey(k) || isPreferenceKey(k);
                    op.event_op = "upsert";
                    if (isPreferenceKey(k)) {
                        auto it = current.find(k);
                        if (it != current.end() && it->second != v) {
                            op.event_op = "conflict";
                        }
                    }
                }
                ops.push_back(std::move(op));
            }
            if (!ops.empty() && domain_repo_->applyUserMemoryHintBatch(user_id, ops)) {
                for (const auto& op : ops) {
                    if (op.event_op == "conflict") {
                        // Conflict candidate set (transient, feeds the
                        // profile re-summarization pass when it lands).
                        const std::string conflict_key =
                            "nexusai:memory:conflict:" + user_id + ":" + op.key;
                        redis_->sadd(conflict_key, op.value);
                        redis_->expire(conflict_key, 7 * 24 * 3600);
                        LOG_INFO("MemoryService: preference conflict recorded for " +
                                 op.key + " (user " + user_id + ")");
                    }
                }
                // C1: PG success -> invalidate the Redis projection.
                redis_->del(key);
            }
            return;
        } catch (const std::exception& e) {
            LOG_WARN("MemoryService: PG hints write failed, falling back to Redis: " +
                     std::string(e.what()));
        }
    }

    // Legacy Redis-only path (repo-less tests / degraded PG).
    for (const auto& [k, v] : hints) {
        if (!isValidHintKey(k)) {
            LOG_WARN("MemoryService: dropping hint with invalid key '" + k + "'");
            continue;
        }
        if (v.empty()) {
            redis_->hdel(key, k);
        } else {
            redis_->hset(key, k, v);
        }
    }
}

// Cross-agent summary - PG (V015) durable + Redis projection.
void MemoryService::setCrossAgentSummary(const std::string& context_id,
                                            const std::string& summary) {
    if (domain_repo_) {
        try {
            if (domain_repo_->upsertCrossAgentSummary(context_id, "", summary)) {
                // C1: PG success -> invalidate the Redis projection.
                redis_->del(summaryKey(context_id));
                return;
            }
        } catch (const std::exception& e) {
            LOG_WARN("MemoryService: PG summary write failed, falling back to Redis: " +
                     std::string(e.what()));
        }
    }
    redis_->set(summaryKey(context_id), summary);
}

std::string MemoryService::getCrossAgentSummary(
    const std::string& context_id) const {
    if (domain_repo_) {
        try {
            for (const auto& [agent_id, summary] :
                 domain_repo_->listCrossAgentSummaries(context_id)) {
                if (agent_id.empty()) {
                    return summary;
                }
            }
        } catch (const std::exception& e) {
            LOG_WARN("MemoryService: PG summary read failed, falling back to Redis: " +
                     std::string(e.what()));
        }
    }
    std::string summary;
    if (redis_->get(summaryKey(context_id), summary) && !summary.empty() &&
        domain_repo_) {
        // Cache-aside rebuild: transitional Redis-only summary is backfilled
        // into PG so the durable source converges.
        try {
            domain_repo_->upsertCrossAgentSummary(context_id, "", summary);
        } catch (const std::exception&) {
            // Best-effort; the read already succeeded.
        }
    }
    return summary;
}

void MemoryService::setCrossAgentSummaryFor(const std::string& context_id,
                                            const std::string& agent_id,
                                            const std::string& summary,
                                            int ttl_seconds) {
    // SETEX: the agent-specialized summary is a cache entry with a bounded
    // lifetime (default 7 days), keyed per taking-over agent.
    redis_->setex(summaryKeyFor(context_id, agent_id), ttl_seconds, summary);
    // Write-through to PG (V015 cross_agent_summaries): without it a
    // restart lost the specialization. Redis stays the read path.
    if (domain_repo_) {
        try {
            domain_repo_->upsertCrossAgentSummary(context_id, agent_id, summary);
        } catch (const std::exception&) {
            // Best-effort projection; the Redis entry already served the read.
        }
    }
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
