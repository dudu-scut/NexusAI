/**
 * @file observability_service.h
 * @brief ObservabilityService gRPC implementation — GetTraceDetail & GetCostReport
 *
 * PostgreSQL is the durable source of truth (traces / token_usage_ledger);
 * Redis is no longer consulted for observability reads.
 *
 * Auth-disabled semantics: the owner key is ALWAYS taken from the
 * authenticated context. When auth is disabled there is no identity, so the
 * owner is empty and every owner-scoped read returns NOT_FOUND / empty rows.
 * This is intentional: observability data is owner-scoped by construction and
 * there is no fallback to owner-less views.
 */

#pragma once

#include "observability.grpc.pb.h"
#include "observability.pb.h"

#include <agent_rpc/common/redis_client.h>
#include <agent_rpc/common/logger.h>

#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

namespace agent_rpc::common {
class AgentRuntimeRepository;
class QueryDomainRepository;
class PostgresBudgetRepository;
}  // namespace agent_rpc::common

namespace agent_rpc {
namespace server {

class ObservabilityServiceImpl final
    : public agent_communication::ObservabilityService::Service {
public:
    explicit ObservabilityServiceImpl(common::RedisClient* redis_client);
    ~ObservabilityServiceImpl() override = default;

    // PostgreSQL repositories are injected by RpcServer; without them the
    // RPCs report UNAVAILABLE instead of falling back to owner-less caches.
    void setAgentRuntimeRepository(common::AgentRuntimeRepository* repository);
    void setQueryDomainRepository(common::QueryDomainRepository* repository);
    // Durable budget counters + owner policies (backing GetBudgetSummary).
    void setBudgetRepository(common::PostgresBudgetRepository* repository);

    grpc::Status GetTraceDetail(
        grpc::ServerContext* context,
        const agent_communication::GetTraceDetailRequest* request,
        agent_communication::GetTraceDetailResponse* response) override;

    grpc::Status GetCostReport(
        grpc::ServerContext* context,
        const agent_communication::GetCostReportRequest* request,
        agent_communication::GetCostReportResponse* response) override;

    grpc::Status GetBudgetSummary(
        grpc::ServerContext* context,
        const agent_communication::GetBudgetSummaryRequest* request,
        agent_communication::GetBudgetSummaryResponse* response) override;

private:
    // P6 遗留收口：PG 无记录时的 Redis 批量键兜底读（PG 主、Redis 兜底）。
    grpc::Status buildRedisFallbackTrace(
        const std::string& redis_key,
        const std::string& trace_id,
        const std::string& owner,
        agent_communication::GetTraceDetailResponse* response);

    common::RedisClient* redis_client_;
    common::AgentRuntimeRepository* runtime_repository_ = nullptr;
    common::QueryDomainRepository* query_repository_ = nullptr;
    common::PostgresBudgetRepository* budget_repository_ = nullptr;
};

} // namespace server
} // namespace agent_rpc
