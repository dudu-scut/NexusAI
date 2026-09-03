#pragma once

#include <string>

namespace agent_rpc {
namespace common {

class QueryDomainRepository;  // P17(m): optional PostgreSQL material source
/**
 * @brief ProfileSummarizer — User profile summarization with optional LLM extraction
 *
 * Batch 4, U2: Compresses identity and preference JSON into a short
 * human-readable summary string for injection into SystemContext.
 *
 * The static summarize() method uses template-based formatting.
 * processPending() uses an LLM API (DeepSeek) to extract structured
 * profiles from conversation history stored in Redis.
 */
class ProfileSummarizer {
public:
    /**
     * Produce a concise text summary from identity and preferences JSON.
     *
     * Input format expected:
     *   identity_json:     {"role": "...", "industry": "...", ...}
     *   preferences_json:  [{"key": "style", "value": "..."}, ...]
     *
     * @param identity_json     JSON blob describing the user's identity
     * @param preferences_json  JSON array of preference key-value pairs
     * @param max_tokens        Rough character budget (default 300)
     * @return                  Concatenated summary, e.g.
     *     "用户是开发者，偏好极简风格。最近在调试RPC框架。"
     */
    static std::string summarize(const std::string& identity_json,
                                  const std::string& preferences_json,
                                  int max_tokens = 300);

    /**
     * LLM-based extraction of user profiles from conversation history.
     *
     * Called periodically from BackgroundScheduler.  Connects to Redis,
     * reads pending user IDs from "profile:pending" list, gathers
     * conversation data, calls the DeepSeek chat-completions API to
     * extract structured profiles, and stores results in
     * "user_profile:<user_id>".
     *
     * @param domain_repo  P17(m): when non-null, conversation material is
     *        read from PostgreSQL (the authoritative history — the Redis
     *        Tier-1 mirror stays empty in production); null falls back to
     *        the legacy best-effort Redis scan.
     */
    static void processPending(QueryDomainRepository* domain_repo = nullptr);
};

}  // namespace common
}  // namespace agent_rpc
