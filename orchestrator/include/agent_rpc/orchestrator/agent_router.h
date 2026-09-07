/**
 * @file agent_router.h
 * @brief Agent Router - selects appropriate agent for requests
 */

#pragma once

#include "agent_rpc/common/env_loader.h"
#include "agent_info.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Forward declarations for the common vector-routing types (P7: moved from
// agent_rpc_mcp to agent_rpc::common::rag, unconditional in every build).
namespace agent_rpc { namespace common { namespace rag {
    class EmbeddingService;
    class VectorIndex;
    class EmbeddingCache;
    class SemanticCacheIndex;
}}}

// Forward declaration for LLM-based intent classification
class LLMClient;

namespace agent_rpc {
namespace common {
class RedisClient;
class LoadBalancerManager;  // P8: optional load-balancing tier
}
namespace orchestrator {

/**
 * @brief Configuration for embedding-based skill routing
 */
struct EmbeddingRouterConfig {
    bool enabled = false;
    float high_threshold = agent_rpc::common::envOrFloat("ROUTING_HIGH_THRESHOLD", 0.85f);
    float low_threshold = agent_rpc::common::envOrFloat("ROUTING_LOW_THRESHOLD", 0.50f);
    std::string api_key;            // embedding API key (falls back to LLM_API_KEY env)
    std::string model = agent_rpc::common::envOrDefault("EMBEDDING_MODEL", agent_rpc::common::envOrDefault("LLM_MODEL", "deepseek-v4-pro"));
    int dimension = 1024;
    std::string api_url = agent_rpc::common::envOrDefault("EMBEDDING_API_URL", "https://api.deepseek.com/v1/embeddings");
};

/**
 * @brief Result of hybrid skill analysis
 */
struct SkillMatchResult {
    std::string skill_name;
    float confidence = 0.0f;
    enum Source { KEYWORD, EMBEDDING, NONE } source = NONE;
};

/**
 * @brief Agent Router - routes requests to appropriate agents
 * 
 * Features:
 * - Multiple routing strategies (round-robin, random, skill-match, least-load)
 * - Health status tracking
 * - Skill-based filtering
 * - Thread-safe operations
 */
class AgentRouter {
public:
    AgentRouter();
    ~AgentRouter();
    
    // Disable copy
    AgentRouter(const AgentRouter&) = delete;
    AgentRouter& operator=(const AgentRouter&) = delete;
    
    /**
     * @brief Initialize router with strategy
     * @param strategy Routing strategy to use
     * @return true if initialization successful
     */
    bool initialize(RoutingStrategy strategy = RoutingStrategy::SKILL_MATCH);
    
    /**
     * @brief Shutdown router
     */
    void shutdown();

    /**
     * @brief Feed a completed agent call's latency for the optional load-
     *        balancer tier (P8 S2). Only REAL completed calls may be fed —
     *        timeouts/failures are skipped by the caller. No-op when the
     *        load balancer is disabled (NEXUSAI_ROUTER_LB_STRATEGY unset).
     * @param agent_id   The agent that actually served the call
     * @param latency_ms Completed-call latency in milliseconds
     */
    void recordEndpointLatency(const std::string& agent_id, double latency_ms);
    
    /**
     * @brief Select an agent for a request
     * @param question The question/request content (used for skill analysis)
     * @param required_skills Optional list of required skills
     * @return Selected agent info, or nullopt if no suitable agent found
     * 
     * Property 4: Agent Selection Determinism
     */
    std::optional<AgentInfo> selectAgent(
        const std::string& question,
        const std::vector<std::string>& required_skills = {});

    // P12(a): routing decision with a truthy confidence trace. `confidence`
    // carries the REAL embedding cosine similarity only when `source` is
    // "embedding"; LLM/IDF/fallback tiers have no numeric confidence, so
    // confidence is 0 and consumers must fall back to rank placeholders
    // (labelled via confidence_source). selectAgent() forwards here and keeps
    // its byte-stable signature for existing callers and property tests.
    struct RouteDecision {
        std::optional<AgentInfo> agent;
        std::string skill;          // matched skill (embedding/LLM/IDF tier)
        double confidence = 0.0;    // real similarity only for source=="embedding"
        std::string source;         // "embedding" | "llm" | "idf" | "fallback" | ""
        bool used_fallback = false;
    };
    RouteDecision selectAgentDetailed(
        const std::string& question,
        const std::vector<std::string>& required_skills = {});

    /**
     * @brief Callback type for agent invocation
     *
     * Parameters: (agent_url, prompt)
     * Returns: agent response text, or empty string on failure.
     */
    using AgentDispatchFn = std::function<std::string(
        const std::string& agent_url, const std::string& prompt)>;

    /**
     * @brief Result of a dispatch() call
     */
    struct DispatchResult {
        std::string response;          // Agent response text
        std::string agent_id;          // Selected agent ID (empty if routing failed)
        std::string agent_name;        // Selected agent name
        std::string matched_skill;     // Skill that was matched
        int64_t latency_ms = 0;        // Total dispatch latency
        bool success = false;          // Whether the call succeeded
    };

    /**
     * @brief Generic dispatch: route + call agent
     *
     * Combines selectAgent() with an injectable call function.
     * Eliminates if/else dispatch — any registered agent is reachable
     * through the same code path.
     *
     * @param question User input text
     * @param call_fn  Injectable agent call function
     * @param required_skills Optional skill filter
     * @return DispatchResult with response and routing metadata
     */
    DispatchResult dispatch(
        const std::string& question,
        const AgentDispatchFn& call_fn,
        const std::vector<std::string>& required_skills = {});

    /**
     * @brief Find agents by skill
     * @param skill Skill to search for
     * @return Vector of agents with the skill
     */
    std::vector<AgentInfo> findAgentsBySkill(const std::string& skill);
    
    /**
     * @brief Find agents by tags
     * @param tags Tags to search for (agent must have all tags)
     * @return Vector of matching agents
     */
    std::vector<AgentInfo> findAgentsByTags(const std::vector<std::string>& tags);
    
    /**
     * @brief Find healthy agents with required skills
     * @param required_skills Skills to match
     * @return Vector of healthy agents with matching skills
     */
    std::vector<AgentInfo> findHealthyAgentsWithSkills(
        const std::vector<std::string>& required_skills);
    
    /**
     * @brief Update the list of available agents
     * @param agents New agent list
     */
    void updateAgentList(const std::vector<AgentInfo>& agents);
    
    /**
     * @brief Add a single agent
     * @param agent Agent to add
     */
    void addAgent(const AgentInfo& agent);
    
    /**
     * @brief Remove an agent by ID
     * @param agent_id Agent identifier
     * @return true if agent was removed
     */
    bool removeAgent(const std::string& agent_id);
    
    /**
     * @brief Get agent by ID
     * @param agent_id Agent identifier
     * @return Agent info if found
     */
    std::optional<AgentInfo> getAgent(const std::string& agent_id);
    
    /**
     * @brief Get all registered agents
     * @return Vector of all agents
     */
    std::vector<AgentInfo> getAllAgents();
    
    /**
     * @brief Get all healthy agents
     * @return Vector of healthy agents
     */
    std::vector<AgentInfo> getHealthyAgents();
    
    /**
     * @brief Mark an agent as unhealthy
     * @param agent_id Agent identifier
     * 
     * Property 8: Agent Health State Consistency
     */
    void markAgentUnhealthy(const std::string& agent_id);
    
    /**
     * @brief Mark an agent as healthy
     * @param agent_id Agent identifier
     */
    void markAgentHealthy(const std::string& agent_id);
    
    /**
     * @brief Check if agent is healthy
     * @param agent_id Agent identifier
     * @return true if agent is healthy
     */
    bool isAgentHealthy(const std::string& agent_id);
    
    /**
     * @brief Update agent heartbeat timestamp
     * @param agent_id Agent identifier
     */
    void updateHeartbeat(const std::string& agent_id);
    
    /**
     * @brief Update agent load
     * @param agent_id Agent identifier
     * @param load New load value
     */
    void updateAgentLoad(const std::string& agent_id, int load);
    
    /**
     * @brief Set routing strategy
     * @param strategy New strategy
     */
    void setStrategy(RoutingStrategy strategy);
    
    /**
     * @brief Get current routing strategy
     * @return Current strategy
     */
    RoutingStrategy getStrategy() const;
    
    /**
     * @brief Get total number of agents
     */
    size_t getAgentCount() const;
    
    /**
     * @brief Get number of healthy agents
     */
    size_t getHealthyAgentCount() const;
    
    /**
     * @brief Build a dynamic intent classification prompt from registered agents
     * 
     * Constructs an LLM prompt that lists all registered skills dynamically.
     * Replaces the hardcoded math/code/general prompt in the orchestrator.
     * 
     * @param user_text The user's input text
     * @return Complete prompt string ready to send to LLM
     */
    std::string buildDynamicIntentPrompt(const std::string& user_text) const;
    
    /**
     * @brief Get all unique skill names with their descriptions
     * 
     * Returns a map of skill_name → description for prompt building.
     */
    std::unordered_map<std::string, std::string> getAllSkillDescriptions() const;

    /**
     * @brief Set LLM client for dynamic intent classification
     *
     * When set, selectAgent() will use LLM-based intent classification
     * with buildDynamicIntentPrompt() before falling back to embedding/keyword routing.
     *
     * @param client LLMClient instance (caller transfers ownership)
     */
    void setLLMClient(std::unique_ptr<LLMClient> client);

    /**
     * @brief Find a fallback agent for a given skill, excluding a specific agent
     * @param skill_name The skill to find a fallback for
     * @param exclude_agent_id Agent ID to exclude from results
     * @return Fallback agent ID, or empty string if none found
     */
    std::string findFallbackAgent(const std::string& skill_name, const std::string& exclude_agent_id);

    /**
     * @brief Get quality coefficient for an agent-skill pair based on approval rate
     * @param agent_id Agent identifier
     * @param skill_name Skill name
     * @return Quality coefficient in range [0.5, 1.0]
     */
    double getQualityCoefficient(const std::string& agent_id, const std::string& skill_name);

    /**
     * @brief Owner-aware quality data source.
     *
     * Returns the approval rate in [0,1] for an agent/skill pair in the
     * context of the current owner, or a negative value when the owner has
     * no feedback for that pair (callers then use the neutral default).
     * The provider is invoked on the routing thread, so implementations may
     * read thread-local auth context to scope the lookup to the owner.
     * Owner-less Redis feedback keys are never used as the source of truth.
     */
    using QualityProvider = std::function<double(const std::string& agent_id,
                                                 const std::string& skill_name)>;

    /**
     * @brief Inject the owner-aware quality provider
     */
    void setQualityProvider(QualityProvider provider);

    /**
     * @brief Set the Redis client (retained for compatibility; feedback
     *        quality no longer reads owner-less Redis keys).
     * @param redis Pointer to RedisClient instance
     */
    void setRedisClient(agent_rpc::common::RedisClient* redis) { redis_ = redis; }

    /**
     * @brief Enable (or disable) the embedding routing tier.
     *
     * Public since P7: the service bootstrap reads NEXUSAI_EMBEDDING_ROUTER
     * and wires the tier here. P7 (批次十一): the vector building blocks are
     * unconditional (agent_rpc_common) — every build hosts the real tier;
     * failures inside enableEmbedding reset to the 3-layer pipeline.
     */
    bool enableEmbedding(const EmbeddingRouterConfig& config);

    /**
     * @brief P10: high-confidence embedding skill hit with its similarity.
     *
     * Returns the best-matching skill plus its similarity when the
     * embedding tier is enabled and the match reaches the configured
     * high threshold; nullopt otherwise (tier disabled, no hit, or below
     * threshold).
     */
    struct HighConfidenceSkill {
        std::string skill;
        double confidence = 0.0;
    };
    std::optional<HighConfidenceSkill> resolveHighConfidenceSkill(const std::string& question);

    /**
     * @brief B6 route-then-plan: rank skills by embedding similarity to the
     * question. Aggregates the per-agent duplicate index entries by skill
     * name (highest similarity wins) and returns them descending.
     * Returns an empty vector whenever the embedding tier is unavailable
     * (default build / embedding disabled) — callers fall back to the full
     * skill set. Cost: one query embed per call.
     */
    std::vector<std::pair<std::string, double>> rankSkillsBySimilarity(
        const std::string& question, int top_k = 20, float threshold = 0.6f);

private:

    /**
     * @brief High-confidence embedding-only skill matching.
     *
     * Returns a skill only when embedding similarity >= high_threshold.
     * Used as the first tier in the restructured routing pipeline:
     *   Embedding(high) → LLM → Keyword → Fallback
     *
     * @param question User input text
     * @param out_query_vector When non-null and the embedding tier is
     *        enabled, receives the query embedding — reused by the P10(c)
     *        intent cache so caching never pays an extra embed call.
     * @param out_similarity When non-null and a real embedding-tier hit
     *        occurs, receives the cosine similarity of the best match.
     *        Left untouched for cache hits / misses (no real similarity).
     * @return Skill name if high-confidence match found, empty string otherwise
     */
    std::string analyzeRequiredSkillEmbedding(
        const std::string& question,
        std::vector<float>* out_query_vector = nullptr,
        double* out_similarity = nullptr);

    /**
     * @brief P10(c): store a resolved intent for the given query vector.
     * Only acts when the intent cache exists (NEXUSAI_INTENT_CACHE=1);
     * lazy cleanup runs before the store to keep expired entries bounded.
     */
    void storeIntentCache(const std::vector<float>& query_vector,
                          const std::string& skill);

    /**
     * @brief Check if embedding routing is enabled
     */
    bool isEmbeddingEnabled() const;

private:
    /**
     * @brief Analyze question to determine required skill
     * @param question Question content
     * @return Detected skill or empty string
     */
    std::string analyzeRequiredSkill(const std::string& question);
    
    /**
     * @brief Select agent using current strategy
     * @param candidates List of candidate agents (snapshot taken under lock)
     * @return Selected agent
     *
     * Must be called WITHOUT holding agents_mutex_: quality-based strategies
     * consult the injected provider, which may hit PostgreSQL.
     */
    AgentInfo selectByStrategy(const std::vector<AgentInfo>& candidates,
                               const std::string& lb_key = {});
    
    /**
     * @brief Select using round-robin strategy
     */
    AgentInfo selectRoundRobin(const std::vector<AgentInfo>& candidates);
    
    /**
     * @brief Select using random strategy
     */
    AgentInfo selectRandom(const std::vector<AgentInfo>& candidates);
    
    /**
     * @brief Select using least-load strategy
     */
    AgentInfo selectLeastLoad(const std::vector<AgentInfo>& candidates);

    /**
     * @brief Select using quality-weighted random (feedback-driven)
     *
     * Agents with higher quality coefficients (derived from approval rate) are
     * more likely to be selected. Uses a weighted random selection.
     */
    AgentInfo selectWeightedByQuality(const std::vector<AgentInfo>& candidates);
    AgentInfo selectWeightedByQualityWithFallback(const std::vector<AgentInfo>& candidates,
                                                  const std::string& lb_key = {});

    /**
     * @brief P8: lazily read NEXUSAI_ROUTER_LB_STRATEGY and construct the
     * optional LoadBalancerManager (once per router instance).
     *
     * Unset variable or an invalid value leaves lb_manager_ null, and the
     * legacy quality-weighted path stays in effect byte-for-byte.
     */
    void ensureLbInitialized();

    /**
     * @brief P8: decide among candidates through the load balancer tier.
     *
     * lb_key is the deterministic selection key for the consistent_hash
     * strategy (the caller passes the user question); with the strategy active
     * and a non-empty key, selection goes through selectEndpointByKey so the
     * same question deterministically maps to the same endpoint. Other
     * strategies ignore the key.
     *
     * Returns nullopt when the tier is unavailable or fails, in which case
     * the caller falls back to the legacy quality-weighted logic.
     */
    std::optional<AgentInfo> selectViaLoadBalancer(const std::vector<AgentInfo>& candidates,
                                                   const std::string& lb_key = {});

    /**
     * @brief Rebuild the skill keyword index from current agents
     * 
     * Extracts keywords from healthy agents' skill names and descriptions.
     * Must be called while agents_mutex_ is held (e.g. from addAgent/removeAgent).
     */
    void rebuildSkillKeywordIndex();

    /**
     * @brief LLM-based intent classification using buildDynamicIntentPrompt()
     *
     * Calls LLM with a dynamically built prompt, parses the response as a
     * skill name, and does exact matching against registered skills.
     *
     * @param question User input text
     * @return Matched skill name, or empty string on failure
     */
    std::string analyzeIntentWithLLM(const std::string& question);

    /**
     * @brief Build/rebuild the skill embedding index from current agents
     *
     * Embeds each skill's "name + description" text and stores in skill_index_.
     * Called from rebuildSkillKeywordIndex() when embedding is enabled.
     */
    void buildSkillEmbeddingIndex();

    /**
     * @brief P10: embed + best-skill search.
     *
     * Must be called while holding embedding_mutex_. Shared by
     * analyzeRequiredSkillEmbedding() and resolveHighConfidenceSkill().
     */
    std::optional<std::pair<std::string, double>>
    searchBestSkillEmbeddingLocked(const std::string& question);

    /**
     * @brief P10: best-skill search over an already-computed query vector.
     * Must be called while holding embedding_mutex_ (P10(c) intent cache
     * reuses the vector the tier already embedded).
     */
    std::optional<std::pair<std::string, double>>
    searchBestSkillEmbeddingLockedWithVector(const std::vector<float>& query_vector);

    mutable std::mutex agents_mutex_;
    std::unordered_map<std::string, AgentInfo> agents_;
    std::atomic<RoutingStrategy> strategy_{RoutingStrategy::SKILL_MATCH};
    std::atomic<size_t> round_robin_index_{0};
    bool initialized_ = false;

    // Inverted keyword index: keyword → list of (skill, IDF weight) entries.
    // Rebuilt on addAgent() / removeAgent().
    struct KeywordEntry {
        std::string skill_name;
        double weight;  // IDF: 1.0 / number of skills sharing this keyword
    };
    std::unordered_map<std::string, std::vector<KeywordEntry>> skill_keywords_;

    // Skill → agent IDs map for fast fallback lookup
    std::unordered_map<std::string, std::vector<std::string>> skill_to_agents_;

    // Embedding-based routing
    EmbeddingRouterConfig embedding_config_;
    std::unique_ptr<agent_rpc::common::rag::EmbeddingService> embedding_service_;
    std::unique_ptr<agent_rpc::common::rag::VectorIndex> skill_index_;
    std::unique_ptr<agent_rpc::common::rag::EmbeddingCache> embedding_cache_;
    // P10(c): intent cache — similar queries reuse the intent skill without
    // an LLM classification call. Created when NEXUSAI_INTENT_CACHE=1 and
    // the embedding tier is enabled; lookup shares the tier's query vector.
    std::unique_ptr<agent_rpc::common::rag::SemanticCacheIndex> intent_cache_;
    mutable std::mutex embedding_mutex_;
    std::atomic<uint64_t> embedding_query_count_{0};
    std::atomic<uint64_t> embedding_hit_count_{0};

    // LLM-based intent classification
    std::unique_ptr<LLMClient> llm_client_;

    // Redis client (retained for API compatibility; quality lookups are
    // owner-aware via quality_provider_).
    agent_rpc::common::RedisClient* redis_ = nullptr;

    // Owner-aware quality provider. Guarded by its own mutex: the
    // provider callable is copied under this lock, then invoked OUTSIDE the
    // lock (it may hit PostgreSQL), and neither step runs under
    // agents_mutex_.
    mutable std::mutex quality_provider_mutex_;
    QualityProvider quality_provider_;

    // P8: optional load-balancing tier (NEXUSAI_ROUTER_LB_STRATEGY, default
    // off). Lazily constructed on first use by ensureLbInitialized(); when
    // the switch is unset/invalid these stay null/empty and routing keeps
    // the legacy behavior unchanged.
    std::string lb_strategy_name_;
    std::unique_ptr<common::LoadBalancerManager> lb_manager_;
    std::once_flag lb_init_flag_;
    // P8: fingerprint of the last endpoint set pushed into the load
    // balancer. selectViaLoadBalancer() only calls updateEndpoints() when
    // the candidate set actually changed — repeated calls on an unchanged
    // set must NOT reset strategy state (round-robin cursor, weighted
    // accumulators), otherwise round_robin would always pick the first
    // candidate and weighted_round_robin would always pick the highest
    // weight.
    std::mutex lb_fingerprint_mutex_;
    std::string lb_endpoint_fingerprint_;
};

} // namespace orchestrator
} // namespace agent_rpc
