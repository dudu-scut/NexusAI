#include "agent_rpc/common/profile_summarizer.h"

#include "agent_rpc/common/env_loader.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/redis_client.h"
#include "agent_rpc/common/query_domain_repository.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace agent_rpc {
namespace common {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// CURL write callback (same pattern as the a2a HTTP client)
// ---------------------------------------------------------------------------
static size_t writeCallback(void* contents, size_t size, size_t nmemb,
                             std::string* output) {
    size_t total = size * nmemb;
    output->append(static_cast<char*>(contents), total);
    return total;
}

std::string ProfileSummarizer::summarize(const std::string& identity_json,
                                          const std::string& preferences_json,
                                          int max_tokens) {
    std::ostringstream oss;

    // --- Identity ---
    if (!identity_json.empty()) {
        try {
            auto id = json::parse(identity_json);
            std::string role = id.value("role", std::string());
            std::string industry = id.value("industry", std::string());
            std::string name = id.value("name", std::string());

            if (!name.empty() && !role.empty()) {
                oss << "用户是" << role << " " << name;
            } else if (!role.empty()) {
                oss << "用户是" << role;
            } else if (!name.empty()) {
                oss << "用户" << name;
            }
            if (!industry.empty()) {
                oss << "，行业" << industry;
            }
            oss << "。";
        } catch (const json::exception&) {
            // Malformed JSON — skip identity
        }
    }

    // --- Preferences ---
    if (!preferences_json.empty()) {
        try {
            auto prefs = json::parse(preferences_json);
            if (prefs.is_array() && !prefs.empty()) {
                // Collect all values into a single preference line
                std::string style;
                std::string tone;
                std::string language;
                for (const auto& p : prefs) {
                    std::string key = p.value("key", "");
                    std::string val = p.value("value", "");
                    if (key == "style")       style = val;
                    else if (key == "tone")    tone = val;
                    else if (key == "language") language = val;
                }
                if (!style.empty() || !tone.empty() || !language.empty()) {
                    oss << "偏好";
                    if (!style.empty())  oss << style << "风格";
                    if (!tone.empty())   oss << "、" << tone << "语气";
                    if (!language.empty()) oss << "、" << language << "语言";
                    oss << "。";
                }
            }
        } catch (const json::exception&) {
            // Malformed JSON — skip preferences
        }
    }

    // --- Context snapshot placeholders ---
    // The context_snapshot field is handled separately via Redis
    // in memory_service.cpp — we add a placeholder here if the
    // summary is otherwise empty.
    if (oss.tellp() == 0) {
        oss << "用户信息摘要不可用。";
    }

    std::string result = oss.str();

    // Rough token-budget enforcement: truncate at max_tokens characters
    // (A reasonable approximation for plain-text CJK + ASCII).
    if (static_cast<int>(result.size()) > max_tokens) {
        result.resize(max_tokens);
        // Re-trim at the last sentence boundary to avoid cutting mid-sentence
        auto last_period = result.rfind("。");
        auto last_dot   = result.rfind('.');
        auto boundary   = std::max(last_period, last_dot);
        if (boundary != std::string::npos && static_cast<int>(boundary) > max_tokens / 2) {
            result.resize(boundary + 1);  // Keep the period
        }
    }

    return result;
}

void ProfileSummarizer::processPending(QueryDomainRepository* domain_repo) {
    // ---------------------------------------------------------------
    // LLM-based profile extraction (Batch 4 U2)
    // Called periodically from BackgroundScheduler ("profile_extraction",
    // every 5 min).  Reads pending user IDs from a Redis list, calls
    // the DeepSeek chat-completions API to extract structured profiles,
    // and persists the results back to Redis.
    // ---------------------------------------------------------------

    // 1. Connect to Redis
    RedisClient redis;
    std::string redis_host = envOrDefault("REDIS_HOST", "127.0.0.1");
    int redis_port = envOrInt("REDIS_PORT", 6379);
    if (!redis.connect(redis_host, redis_port)) {
        LOG_WARN("ProfileSummarizer: Redis connection failed, skipping");
        return;
    }

    // 2. Fetch pending user IDs from the list "profile:pending"
    std::vector<std::string> pending_users;
    if (!redis.lrange("profile:pending", 0, -1, pending_users) ||
        pending_users.empty()) {
        // No pending work — normal path
        return;
    }

    // 3. Read LLM configuration from environment
    std::string api_url  = envOrDefault("LLM_API_URL",
                                         "https://api.deepseek.com/v1/chat/completions");
    std::string api_key  = envOrDefault("LLM_API_KEY", "");
    std::string model    = envOrDefault("LLM_MODEL", "deepseek-v4-flash");

    if (api_key.empty()) {
        LOG_WARN("ProfileSummarizer: LLM_API_KEY not set, skipping");
        return;
    }

    // 4. Pop one user at a time so IDs pushed while this run is in flight
    //    survive for the next run. The dedup guard is released at the end of
    //    every iteration regardless of outcome, so a failed user can be
    //    re-enqueued by a later trigger.
    for (;;) {
        std::string user_id;
        if (!redis.lpop("profile:pending", user_id) || user_id.empty()) {
            break;  // queue drained
        }
        try {
            // 4a. Gather conversation data from Redis hash
            //     Key pattern: "nexusai:memory:<user_id>" (user long-term memory)
            std::map<std::string, std::string> user_memory;
            redis.hgetall("nexusai:memory:" + user_id, user_memory);

            // Also try to grab the most recent conversation list
            // (we look for a well-known context key; fall back to empty)
            std::vector<std::string> conv_msgs;
            // Scan a few common context IDs — in practice the caller may
            // store a list of context_ids per user; here we do best-effort.
            std::string user_conv_key = "nexusai:user_convs:" + user_id;
            std::vector<std::string> context_ids;
            redis.lrange(user_conv_key, 0, -1, context_ids);

            std::ostringstream history_oss;
            if (domain_repo) {
                // P17(m): PostgreSQL conversation history is the real
                // material source — the Redis Tier-1 mirror stays empty in
                // production (see P1). Take the last 20 messages per
                // conversation owned by this user.
                const auto conversations =
                    domain_repo->listConversations(user_id);
                for (const auto& conv : conversations) {
                    const auto msgs =
                        domain_repo->listMessages(user_id, conv.id);
                    const std::size_t start =
                        msgs.size() > 20 ? msgs.size() - 20 : 0;
                    for (std::size_t index = start; index < msgs.size();
                         ++index) {
                        history_oss << msgs[index].role << ": "
                                    << msgs[index].content << "\n";
                    }
                }
            } else {
                // Legacy best-effort Redis scan (fallback when no repo is
                // injected; the Tier-1 mirror is empty in production).
                for (const auto& ctx_id : context_ids) {
                // Get last agent used in this context
                std::string last_agent;
                redis.get("nexusai:last_agent:" + ctx_id, last_agent);
                if (last_agent.empty()) last_agent = "default";

                std::vector<std::string> msgs;
                std::string conv_key = "nexusai:conv:" + ctx_id + ":" + last_agent;
                redis.lrange(conv_key, -20, -1, msgs);  // last 20 messages
                for (const auto& m : msgs) {
                    history_oss << m << "\n";
                }
            }
            }

            // Combine memory hints + conversation into the prompt
            std::string conversation_history;
            if (!user_memory.empty()) {
                conversation_history += "User memory hints:\n";
                for (const auto& [k, v] : user_memory) {
                    conversation_history += "  " + k + ": " + v + "\n";
                }
            }
            if (!history_oss.str().empty()) {
                conversation_history += "\nRecent conversation:\n" + history_oss.str();
            }

            if (conversation_history.empty()) {
                // No data to extract — skip this user (guard released below)
                continue;
            }

            // 4b. Build LLM request body
            json request_body = {
                {"model", model},
                {"messages", json::array({
                    {{"role", "system"},
                     {"content", "你是一个用户画像分析助手。请从以下对话和记忆中提取用户的"
                                 "偏好、兴趣和沟通风格。"
                                 // P17(o): the prompt demands the READER-side
                                 // structure directly (write/read share one
                                 // schema — no post-hoc conversion layer).
                                 "严格以如下 JSON 结构输出，不要输出任何其他内容："
                                 "{\"identity\": {\"name\": \"\", \"role\": \"\", "
                                 "\"industry\": \"\"}, "
                                 "\"preferences\": [{\"key\": \"\", \"value\": \"\"}]}"}},
                    {{"role", "user"},
                     {"content", conversation_history}}
                })},
                {"temperature", 0.3},
                {"max_tokens", 500}
            };
            std::string json_body = request_body.dump();

            // 4c. Call LLM API via libcurl
            CURL* curl = curl_easy_init();
            if (!curl) {
                LOG_ERROR("ProfileSummarizer: curl_easy_init() failed");
                // Re-queue for the next run. Whatever the requeue outcome, the
                // dedup guard must be released BEFORE breaking out — break
                // skips the end-of-iteration hdel, and a held guard with no
                // queue entry would starve this user's profile forever.
                const bool requeued = redis.rpush("profile:pending", user_id);
                if (!requeued) {
                    LOG_ERROR("ProfileSummarizer: requeue push failed for user " +
                              user_id + "; releasing dedup guard anyway");
                }
                redis.hdel("profile:queued", user_id);
                break;
            }

            struct curl_slist* headers = nullptr;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            std::string auth_header = "Authorization: Bearer " + api_key;
            headers = curl_slist_append(headers, auth_header.c_str());

            std::string response_body;
            curl_easy_setopt(curl, CURLOPT_URL, api_url.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
            // TLS peer verification is on by default; opt out only for local
            // development against self-signed endpoints.
            if (envOrDefault("LLM_TLS_INSECURE", "0") == "1") {
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            }

            CURLcode res = curl_easy_perform(curl);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);

            if (res != CURLE_OK) {
                LOG_ERROR("ProfileSummarizer: LLM API call failed for user " +
                          user_id + ": " + curl_easy_strerror(res));
                continue;  // skip this user, try next
            }

            // 4d. Parse LLM response — extract choices[0].message.content
            std::string profile_text;
            try {
                auto resp_json = json::parse(response_body);
                if (resp_json.contains("choices") &&
                    resp_json["choices"].is_array() &&
                    !resp_json["choices"].empty()) {
                    profile_text = resp_json["choices"][0]
                                          .value("message", json::object())
                                          .value("content", std::string());
                }
            } catch (const json::exception& e) {
                LOG_ERROR("ProfileSummarizer: Failed to parse LLM response for " +
                          user_id + ": " + e.what());
                continue;
            }

            if (profile_text.empty()) {
                LOG_WARN("ProfileSummarizer: Empty profile from LLM for user " +
                         user_id);
                continue;
            }

            // 4e. Persist extracted profile to Redis.
            // P17(o): validate against the reader-side schema before
            // persisting. On failure the raw text is still kept (inspectable)
            // and a warning marks the mismatch instead of silently poisoning
            // the summarize() consumer with unparseable JSON.
            bool schema_ok = false;
            try {
                const auto parsed = json::parse(profile_text);
                schema_ok = parsed.is_object() &&
                            parsed.contains("identity") &&
                            parsed["identity"].is_object() &&
                            parsed.contains("preferences") &&
                            parsed["preferences"].is_array();
            } catch (const json::exception&) {
                schema_ok = false;
            }

            if (schema_ok) {
                redis.set("user_profile:" + user_id, profile_text);
                LOG_INFO("ProfileSummarizer: Extracted profile for user " + user_id);
                // B5 (P17n): PG is the durable source (V004 user_profiles);
                // a successful upsert retires the Redis projection ("PG
                // success → DEL Redis") per the storage-layering contract.
                // Redis keeps serving as the transitional fallback until the
                // read path confirms PG coverage.
                if (domain_repo) {
                    try {
                        const auto parsed_profile = json::parse(profile_text);
                        common::UserProfileRecord profile_record;
                        profile_record.user_id = user_id;
                        profile_record.identity =
                            parsed_profile.value("identity", json::object()).dump();
                        profile_record.preferences =
                            parsed_profile.value("preferences", json::array()).dump();
                        if (domain_repo->upsertUserProfile(profile_record)) {
                            redis.del("user_profile:" + user_id);
                            LOG_INFO("ProfileSummarizer: profile persisted to PG for user " +
                                     user_id);
                        }
                    } catch (const std::exception& pg_error) {
                        // Keep the Redis copy — PG is unavailable, the
                        // read-side fallback still serves this profile.
                        LOG_WARN("ProfileSummarizer: PG profile persist failed for user " +
                                 user_id + ": " + pg_error.what());
                    }
                }
            } else {
                // P17(o): a failed schema validation must NOT poison the
                // reader — keep the raw text under an inspection-only key.
                redis.set("user_profile_raw:" + user_id, profile_text);
                LOG_WARN("ProfileSummarizer: profile failed schema validation for user " +
                         user_id + " (raw text kept under user_profile_raw)");
            }

        } catch (const std::exception& e) {
            LOG_ERROR("ProfileSummarizer: Failed for user " + user_id + ": " +
                      e.what());
        }

        // Release the queue dedup guard so a future trigger can enqueue this
        // user again, whatever the outcome was.
        redis.hdel("profile:queued", user_id);
    }
}

}  // namespace common
}  // namespace agent_rpc
