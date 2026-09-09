#pragma once

#include "agent_rpc/common/postgres_store.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace agent_rpc::common {

// Mirrors agent_registry (V011). Platform-level registrations use the fixed
// owner_id "system"; per-user routing quality lives in agent_route_quality.
struct AgentRegistryRecord {
    std::string id;
    std::string owner_id;
    std::string agent_id;
    std::string display_name;
    std::string capabilities;   // JSON text
    std::string health_status;
    std::string last_heartbeat;
    std::string created_at;
    std::string updated_at;
};

// Mirrors feedback (V011 + V013 trace_id/skill_name columns).
struct RuntimeFeedbackRecord {
    std::string id;
    std::string owner_id;
    std::string query_log_id;
    std::string trace_id;
    std::string agent_id;
    std::string skill_name;
    int rating = 0;
    std::string comment;
    std::string created_at;
    std::string updated_at;
};

// Mirrors agent_route_quality (V011 + V013 skill_name column).
struct RuntimeRouteQualityRecord {
    std::string id;
    std::string owner_id;
    std::string agent_id;
    std::string skill_name;
    std::int64_t sample_count = 0;
    std::string average_rating;
    std::string routing_weight;
    std::string created_at;
    std::string updated_at;
};

// Mirrors agent_invocations (V013): owner/query-log linked call facts.
struct AgentInvocationRecord {
    std::string id;
    std::string owner_id;
    std::string query_log_id;
    std::string agent_id;
    std::string skill_name;
    std::string status;
    std::int64_t latency_ms = 0;
    std::string created_at;
    std::string updated_at;
};

// One aggregated cost row per calendar day (UTC), derived from
// token_usage_ledger. cost_usd stays a decimal string to avoid double drift.
struct DailyCostRecord {
    std::string date;
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;
    std::string cost_usd;
    std::int64_t request_count = 0;
    bool estimated = false;
};

// Per-agent invocation aggregates derived from agent_invocations.
struct InvocationMetricsRecord {
    std::string agent_id;
    std::int64_t total_requests = 0;
    std::string success_rate;    // percent, two decimals, as text
    std::string avg_latency_ms;  // two decimals, as text
    std::string p95_latency_ms;  // two decimals, as text; empty when no latency samples
};

struct FeedbackKey {
    std::string owner_id;
    std::string agent_id;
    std::string skill_name;
};

// Owner-scoped runtime facts repository (registry, feedback, route quality,
// invocation facts, cost reports). Every query uses libpqxx parameter
// binding; PostgreSQL is the durable source of truth and Redis remains a
// cache/liveness layer only.
class AgentRuntimeRepository final {
public:
    explicit AgentRuntimeRepository(PostgresStore& store);

    bool upsertAgentRegistry(const AgentRegistryRecord& record);
    // Upsert semantics: refreshes last_heartbeat/health when the row exists and
    // heals a missing row (minimal system-owned placeholder) when it does not,
    // so a registration-time PostgreSQL outage self-heals on the next heartbeat.
    bool updateAgentHeartbeat(const std::string& agent_id);
    bool markAgentStatus(const std::string& agent_id, const std::string& health_status);
    // Latest health verdict per agent (agent_registry, deduplicated across
    // owner rows) — the backing read for GetAgents' health_status field.
    std::map<std::string, std::string> listAgentHealthStatus();
    std::optional<AgentRegistryRecord> getAgent(const std::string& agent_id);
    std::vector<AgentRegistryRecord> listAgents();

    bool insertFeedback(const RuntimeFeedbackRecord& feedback);

    // Beta(2,2)-smoothed approval rate (rating >= 3 counts as approval) for
    // one owner/agent/skill triple. std::nullopt when the owner has no
    // feedback for that pair yet — callers must fall back to a neutral
    // default instead of borrowing another owner's data.
    std::optional<double> feedbackApprovalRate(const std::string& owner_id,
                                               const std::string& agent_id,
                                               const std::string& skill_name);

    // Recomputes and upserts agent_route_quality for one owner/agent/skill
    // triple from the feedback table. Returns false when there is nothing to
    // aggregate or on database failure.
    bool aggregateRouteQuality(const std::string& owner_id,
                               const std::string& agent_id,
                               const std::string& skill_name);

    std::optional<RuntimeRouteQualityRecord> getRouteQuality(const std::string& owner_id,
                                                             const std::string& agent_id,
                                                             const std::string& skill_name);
    std::vector<FeedbackKey> listFeedbackKeys();

    // NOTE: production writer is the Query/QueryStream pipeline —
    // AIQueryServiceImpl records one owner-scoped fact per request on the
    // single-agent A2A path, MultiAgentHandler records per-call facts on
    // the orchestrator path (wired in RpcServer::initialize, final wrap-up).
    bool recordInvocation(const AgentInvocationRecord& invocation);
    std::vector<AgentInvocationRecord> listInvocationsByOwner(const std::string& owner_id);
    // Retired cross-tenant aggregate (deep-review D4): the Redis cache it
    // fed was removed with the owner-scoped GetAgentMetrics rewrite and no
    // production caller remains — kept only for the repository contract
    // tests. Prefer metricsForAgent (owner-scoped, real p95).
    std::vector<InvocationMetricsRecord> aggregateInvocationMetrics();

    // Owner-scoped point metrics for a single agent — the backing query for
    // GetAgentMetrics. deep-review: the legacy Redis agent_metrics cache was
    // written without an owner dimension, so any cross-tenant read of it
    // leaked another user's invocation aggregates; PostgreSQL is the sole
    // source here and there is deliberately no Redis fallback.
    // Returns nullopt when the owner has no invocations for the agent.
    std::optional<InvocationMetricsRecord> metricsForAgent(
        const std::string& owner_id, const std::string& agent_id);

    std::vector<DailyCostRecord> dailyCostReport(const std::string& owner_id,
                                                 const std::string& start_date,
                                                 const std::string& end_date);

private:
    PostgresStore& store_;
};

}  // namespace agent_rpc::common
