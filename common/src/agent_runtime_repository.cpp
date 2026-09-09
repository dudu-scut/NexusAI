#include "agent_rpc/common/agent_runtime_repository.h"

#include <pqxx/version>

#include <cstdint>
#include <random>
#include <string>

namespace agent_rpc::common {
namespace {

template <typename... Arguments>
pqxx::result execParams(pqxx::transaction_base& transaction, const std::string& query,
                        Arguments&&... arguments) {
#if PQXX_VERSION_MAJOR >= 8
    return transaction.exec(query, pqxx::params{std::forward<Arguments>(arguments)...});
#else  // libpqxx 6.x/7.x
    return transaction.exec_params(query, std::forward<Arguments>(arguments)...);
#endif
}

// Locally generated primary keys keep the repository free of database
// sequences while staying collision-safe.
std::string generateRowId(const char* prefix) {
    static thread_local std::mt19937_64 generator{std::random_device{}()};
    constexpr char kHex[] = "0123456789abcdef";
    std::string suffix;
    suffix.reserve(32);
    for (int round = 0; round < 2; ++round) {
        std::uint64_t value = generator();
        for (int index = 0; index < 16; ++index) {
            suffix.push_back(kHex[value & 0xf]);
            value >>= 4;
        }
    }
    return std::string{prefix} + "-" + suffix;
}

template <typename Row>
AgentRegistryRecord registryFromRow(const Row& row) {
    return AgentRegistryRecord{
        .id = row["id"].template as<std::string>(),
        .owner_id = row["owner_id"].template as<std::string>(),
        .agent_id = row["agent_id"].template as<std::string>(),
        .display_name = row["display_name"].template as<std::string>(),
        .capabilities = row["capabilities"].template as<std::string>(),
        .health_status = row["health_status"].template as<std::string>(),
        .last_heartbeat = row["last_heartbeat"].template as<std::string>(),
        .created_at = row["created_at"].template as<std::string>(),
        .updated_at = row["updated_at"].template as<std::string>(),
    };
}

template <typename Row>
RuntimeRouteQualityRecord routeQualityFromRow(const Row& row) {
    return RuntimeRouteQualityRecord{
        .id = row["id"].template as<std::string>(),
        .owner_id = row["owner_id"].template as<std::string>(),
        .agent_id = row["agent_id"].template as<std::string>(),
        .skill_name = row["skill_name"].template as<std::string>(),
        .sample_count = row["sample_count"].template as<std::int64_t>(),
        .average_rating = row["average_rating"].template as<std::string>(),
        .routing_weight = row["routing_weight"].template as<std::string>(),
        .created_at = row["created_at"].template as<std::string>(),
        .updated_at = row["updated_at"].template as<std::string>(),
    };
}

template <typename Row>
AgentInvocationRecord invocationFromRow(const Row& row) {
    return AgentInvocationRecord{
        .id = row["id"].template as<std::string>(),
        .owner_id = row["owner_id"].template as<std::string>(),
        .query_log_id = row["query_log_id"].template as<std::string>(),
        .agent_id = row["agent_id"].template as<std::string>(),
        .skill_name = row["skill_name"].template as<std::string>(),
        .status = row["status"].template as<std::string>(),
        .latency_ms = row["latency_ms"].template as<std::int64_t>(),
        .created_at = row["created_at"].template as<std::string>(),
        .updated_at = row["updated_at"].template as<std::string>(),
    };
}

}  // namespace

AgentRuntimeRepository::AgentRuntimeRepository(PostgresStore& store) : store_(store) {}

bool AgentRuntimeRepository::upsertAgentRegistry(const AgentRegistryRecord& record) {
    bool ok = false;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "INSERT INTO agent_registry (id, owner_id, agent_id, display_name, capabilities, "
            "health_status, last_heartbeat, updated_at) "
            "VALUES ($1, $2, $3, $4, $5::jsonb, $6, NOW(), NOW()) "
            "ON CONFLICT (owner_id, agent_id) DO UPDATE SET "
            "display_name = EXCLUDED.display_name, "
            "capabilities = EXCLUDED.capabilities, "
            "health_status = EXCLUDED.health_status, "
            "last_heartbeat = NOW(), "
            "updated_at = NOW()",
            record.id, record.owner_id, record.agent_id, record.display_name,
            record.capabilities, record.health_status);
        ok = result.affected_rows() > 0;
    });
    return ok;
}

bool AgentRuntimeRepository::updateAgentHeartbeat(const std::string& agent_id) {
    bool ok = false;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "INSERT INTO agent_registry (id, owner_id, agent_id, display_name, capabilities, "
            "health_status, last_heartbeat, updated_at) "
            "VALUES ('registry-' || $1, 'system', $1, $1, '{}'::jsonb, 'healthy', NOW(), NOW()) "
            "ON CONFLICT (owner_id, agent_id) DO UPDATE SET "
            "health_status = 'healthy', "
            "last_heartbeat = NOW(), "
            "updated_at = NOW()",
            agent_id);
        ok = result.affected_rows() > 0;
    });
    return ok;
}

bool AgentRuntimeRepository::markAgentStatus(const std::string& agent_id,
                                             const std::string& health_status) {
    bool ok = false;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "UPDATE agent_registry SET health_status = $2, updated_at = NOW() "
            "WHERE agent_id = $1",
            agent_id, health_status);
        ok = result.affected_rows() > 0;
    });
    return ok;
}

std::optional<AgentRegistryRecord> AgentRuntimeRepository::getAgent(const std::string& agent_id) {
    std::optional<AgentRegistryRecord> record;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "SELECT id, owner_id, agent_id, display_name, capabilities::text AS capabilities, "
            "health_status, last_heartbeat::text AS last_heartbeat, "
            "created_at::text AS created_at, updated_at::text AS updated_at "
            "FROM agent_registry WHERE agent_id = $1 ORDER BY updated_at DESC LIMIT 1",
            agent_id);
        if (!result.empty()) {
            record = registryFromRow(result[0]);
        }
    });
    return record;
}

std::vector<AgentRegistryRecord> AgentRuntimeRepository::listAgents() {
    std::vector<AgentRegistryRecord> records;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "SELECT id, owner_id, agent_id, display_name, capabilities::text AS capabilities, "
            "health_status, last_heartbeat::text AS last_heartbeat, "
            "created_at::text AS created_at, updated_at::text AS updated_at "
            "FROM agent_registry ORDER BY updated_at DESC");
        records.reserve(result.size());
        for (const auto& row : result) {
            records.push_back(registryFromRow(row));
        }
    });
    return records;
}

bool AgentRuntimeRepository::insertFeedback(const RuntimeFeedbackRecord& feedback) {
    bool inserted = false;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "INSERT INTO feedback (id, owner_id, query_log_id, trace_id, agent_id, "
            "skill_name, rating, comment) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8)",
            feedback.id, feedback.owner_id, feedback.query_log_id, feedback.trace_id,
            feedback.agent_id, feedback.skill_name, feedback.rating, feedback.comment);
        inserted = result.affected_rows() > 0;
    });
    return inserted;
}

std::optional<double> AgentRuntimeRepository::feedbackApprovalRate(
    const std::string& owner_id, const std::string& agent_id, const std::string& skill_name) {
    std::optional<double> rate;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "SELECT COUNT(*) AS total, "
            "COUNT(*) FILTER (WHERE rating >= 3) AS positive "
            "FROM feedback WHERE owner_id = $1 AND agent_id = $2 AND skill_name = $3",
            owner_id, agent_id, skill_name);
        if (result.empty()) {
            return;
        }
        const auto total = result[0]["total"].as<std::int64_t>();
        const auto positive = result[0]["positive"].as<std::int64_t>();
        if (total <= 0) {
            return;
        }
        // Beta(2,2) prior keeps low-volume agents away from extreme weights.
        rate = static_cast<double>(positive + 2) / static_cast<double>(total + 4);
    });
    return rate;
}

bool AgentRuntimeRepository::aggregateRouteQuality(const std::string& owner_id,
                                                   const std::string& agent_id,
                                                   const std::string& skill_name) {
    bool updated = false;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto stats = execParams(
            transaction,
            "SELECT COUNT(*) AS total, "
            "COALESCE(AVG(rating), 0) AS average_rating, "
            "COUNT(*) FILTER (WHERE rating >= 3) AS positive "
            "FROM feedback WHERE owner_id = $1 AND agent_id = $2 AND skill_name = $3",
            owner_id, agent_id, skill_name);
        if (stats.empty()) {
            return;
        }
        const auto total = stats[0]["total"].as<std::int64_t>();
        if (total <= 0) {
            return;
        }
        const auto positive = stats[0]["positive"].as<std::int64_t>();
        const auto average = stats[0]["average_rating"].as<double>();
        const double weight =
            static_cast<double>(positive + 2) / static_cast<double>(total + 4);

        const auto result = execParams(
            transaction,
            "INSERT INTO agent_route_quality (id, owner_id, agent_id, skill_name, "
            "sample_count, average_rating, routing_weight, updated_at) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, NOW()) "
            "ON CONFLICT (owner_id, agent_id, skill_name) DO UPDATE SET "
            "sample_count = EXCLUDED.sample_count, "
            "average_rating = EXCLUDED.average_rating, "
            "routing_weight = EXCLUDED.routing_weight, "
            "updated_at = NOW()",
            generateRowId("quality"), owner_id, agent_id, skill_name, total, average,
            weight);
        updated = result.affected_rows() > 0;
    });
    return updated;
}

std::optional<RuntimeRouteQualityRecord> AgentRuntimeRepository::getRouteQuality(
    const std::string& owner_id, const std::string& agent_id, const std::string& skill_name) {
    std::optional<RuntimeRouteQualityRecord> record;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "SELECT id, owner_id, agent_id, skill_name, sample_count, "
            "average_rating::text AS average_rating, routing_weight::text AS routing_weight, "
            "created_at::text AS created_at, updated_at::text AS updated_at "
            "FROM agent_route_quality "
            "WHERE owner_id = $1 AND agent_id = $2 AND skill_name = $3",
            owner_id, agent_id, skill_name);
        if (!result.empty()) {
            record = routeQualityFromRow(result[0]);
        }
    });
    return record;
}

std::vector<FeedbackKey> AgentRuntimeRepository::listFeedbackKeys() {
    std::vector<FeedbackKey> keys;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "SELECT DISTINCT owner_id, agent_id, skill_name FROM feedback");
        keys.reserve(result.size());
        for (const auto& row : result) {
            keys.push_back(FeedbackKey{
                .owner_id = row["owner_id"].as<std::string>(),
                .agent_id = row["agent_id"].as<std::string>(),
                .skill_name = row["skill_name"].as<std::string>(),
            });
        }
    });
    return keys;
}

bool AgentRuntimeRepository::recordInvocation(const AgentInvocationRecord& invocation) {
    bool inserted = false;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "INSERT INTO agent_invocations (id, owner_id, query_log_id, agent_id, "
            "skill_name, status, latency_ms) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7)",
            invocation.id, invocation.owner_id, invocation.query_log_id,
            invocation.agent_id, invocation.skill_name, invocation.status,
            invocation.latency_ms);
        inserted = result.affected_rows() > 0;
    });
    return inserted;
}

std::vector<AgentInvocationRecord> AgentRuntimeRepository::listInvocationsByOwner(
    const std::string& owner_id) {
    std::vector<AgentInvocationRecord> records;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "SELECT id, owner_id, query_log_id, agent_id, skill_name, status, latency_ms, "
            "created_at::text AS created_at, updated_at::text AS updated_at "
            "FROM agent_invocations WHERE owner_id = $1 ORDER BY created_at",
            owner_id);
        records.reserve(result.size());
        for (const auto& row : result) {
            records.push_back(invocationFromRow(row));
        }
    });
    return records;
}

std::vector<InvocationMetricsRecord> AgentRuntimeRepository::aggregateInvocationMetrics() {
    std::vector<InvocationMetricsRecord> records;
    store_.executeTransaction([&](pqxx::work& transaction) {
        const auto result = execParams(
            transaction,
            "SELECT agent_id, COUNT(*) AS total_requests, "
            "ROUND(100.0 * COUNT(*) FILTER (WHERE status = 'success') / COUNT(*), 2)::text "
            "  AS success_rate, "
            "ROUND(AVG(latency_ms), 2)::text AS avg_latency_ms "
            "FROM agent_invocations GROUP BY agent_id");
        records.reserve(result.size());
        for (const auto& row : result) {
            records.push_back(InvocationMetricsRecord{
                .agent_id = row["agent_id"].as<std::string>(),
                .total_requests = row["total_requests"].as<std::int64_t>(),
                .success_rate = row["success_rate"].as<std::string>(),
                .avg_latency_ms = row["avg_latency_ms"].as<std::string>(),
            });
        }
    });
    return records;
}

std::optional<InvocationMetricsRecord> AgentRuntimeRepository::metricsForAgent(
    const std::string& owner_id, const std::string& agent_id) {
    std::optional<InvocationMetricsRecord> record;
    store_.executeTransaction([&](pqxx::work& transaction) {
        // Owner-scoped single-agent aggregates with a real 95th-percentile
        // latency (PERCENTILE_CONT over the recorded latency_ms samples).
        // No Redis fallback: the legacy agent_metrics cache carried no owner
        // dimension and a cross-tenant read would leak aggregates.
        const auto result = execParams(
            transaction,
            "SELECT COUNT(*) AS total_requests, "
            "ROUND(100.0 * COUNT(*) FILTER (WHERE status = 'success') / "
            "NULLIF(COUNT(*), 0), 2)::text AS success_rate, "
            "ROUND(AVG(latency_ms), 2)::text AS avg_latency_ms, "
            "ROUND(PERCENTILE_CONT(0.95) WITHIN GROUP (ORDER BY latency_ms), "
            "2)::text AS p95_latency_ms "
            "FROM agent_invocations WHERE owner_id = $1 AND agent_id = $2",
            owner_id, agent_id);
        if (result.empty() || result[0]["total_requests"].as<std::int64_t>() == 0) {
            return;
        }
        const auto& row = result[0];
        const auto text_or_empty = [](const pqxx::field& field) {
            return field.is_null() ? std::string{} : field.as<std::string>();
        };
        record = InvocationMetricsRecord{
            .agent_id = agent_id,
            .total_requests = row["total_requests"].as<std::int64_t>(),
            .success_rate = text_or_empty(row["success_rate"]),
            .avg_latency_ms = text_or_empty(row["avg_latency_ms"]),
            .p95_latency_ms = text_or_empty(row["p95_latency_ms"]),
        };
    });
    return record;
}

std::vector<DailyCostRecord> AgentRuntimeRepository::dailyCostReport(
    const std::string& owner_id, const std::string& start_date, const std::string& end_date) {
    std::vector<DailyCostRecord> records;
    store_.executeTransaction([&](pqxx::work& transaction) {
        // M4 (PR-C3): bucket by (day, estimated), never by day alone. Mixing
        // estimate-only rows with provider-exact rows into one bucket would
        // lie about the flag and the totals; once provider usage back-fills
        // exact values, those rows form their own exact bucket per day.
        const auto result = execParams(
            transaction,
            "SELECT to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD') AS day, "
            "COALESCE(SUM(prompt_tokens), 0) AS prompt_tokens, "
            "COALESCE(SUM(completion_tokens), 0) AS completion_tokens, "
            "COALESCE(SUM(cost_usd), 0)::text AS cost_usd, "
            "COUNT(*) AS request_count, "
            "estimated "
            "FROM token_usage_ledger "
            "WHERE owner_id = $1 AND created_at >= ($2::date) "
            "AND created_at < ($3::date + INTERVAL '1 day') "
            "GROUP BY day, estimated ORDER BY day, estimated",
            owner_id, start_date, end_date);
        records.reserve(result.size());
        for (const auto& row : result) {
            records.push_back(DailyCostRecord{
                .date = row["day"].as<std::string>(),
                .prompt_tokens = row["prompt_tokens"].as<std::int64_t>(),
                .completion_tokens = row["completion_tokens"].as<std::int64_t>(),
                .cost_usd = row["cost_usd"].as<std::string>(),
                .request_count = row["request_count"].as<std::int64_t>(),
                .estimated = row["estimated"].as<bool>(),
            });
        }
    });
    return records;
}

}  // namespace agent_rpc::common
