#pragma once

#include "agent_rpc/common/redis_client.h"

#include <string>

namespace agent_rpc::common {
class AgentRuntimeRepository;
}  // namespace agent_rpc::common

namespace agent_rpc {
namespace orchestrator {

/**
 * @brief Feedback aggregator for owner-scoped routing quality and metrics.
 *
 * PostgreSQL is the sole source of truth: feedback rows and the
 * derived agent_route_quality / agent_invocations aggregates all live in PG
 * and are read/written through AgentRuntimeRepository (libpqxx parameter
 * binding). Redis is only refreshed as an optional metrics cache — there is
 * no shell-out and no separate connection string anywhere in this class.
 */
class FeedbackAggregator {
public:
    /**
     * @brief Attach the durable PostgreSQL runtime repository.
     * @param repository Pointer owned by the caller (RpcServer)
     */
    static void setRuntimeRepository(agent_rpc::common::AgentRuntimeRepository* repository);

    /**
     * @brief Recompute owner-scoped agent_route_quality from feedback rows.
     *
     * Iterates every distinct (owner, agent, skill) feedback triple and
     * upserts the Bayesian-smoothed (Beta(2,2)) quality row in PostgreSQL.
     * Scheduled to run hourly.
     */
    static void recalculate();

private:
    /** Durable runtime repository (set once via setRuntimeRepository). */
    static agent_rpc::common::AgentRuntimeRepository* runtime_repository_;
};

} // namespace orchestrator
} // namespace agent_rpc
