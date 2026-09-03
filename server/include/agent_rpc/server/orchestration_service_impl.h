/**
 * @file orchestration_service_impl.h
 * @brief OrchestrationService gRPC methods (extracted from ai_query_service.cpp)
 *
 * Implements: ExecutePlan, ReplayQuery, ExportConversation
 */

#pragma once

#include "agent_rpc/common/types.h"
#include "agent_rpc/common/memory_service.h"
#include "agent_rpc/orchestrator/task_planner.h"
#include "agent_rpc/orchestrator/task_executor.h"
#include "agent_rpc/orchestrator/agent_router.h"

#include <grpcpp/grpcpp.h>

namespace agent_rpc {
namespace common {
class QueryDomainRepository;      // P2(g/h): durable rows + PG history
class PostgresBudgetRepository;   // P2(g): budget reservation
class RedisClient;                // P2(h): profile read-back
}  // namespace common
}  // namespace agent_rpc

namespace agent_communication {
class ExecutePlanRequest;
class ExecutePlanResponse;
class ReplayQueryRequest;
class ReplayQueryResponse;
class ExportConversationRequest;
class ExportConversationResponse;
}

namespace agent_rpc {
namespace server {

/**
 * @brief Implements OrchestrationService gRPC methods
 *
 * This class owns the logic for DAG execution, query replay, and conversation export.
 * It is composed into AIQueryServiceImpl, which delegates the override methods here.
 */
class OrchestrationServiceImpl {
public:
    OrchestrationServiceImpl(
        orchestrator::TaskPlanner* planner,
        orchestrator::TaskExecutor* executor,
        orchestrator::AgentRouter* router,
        common::MemoryService* memory,
        common::RpcConfig* config,
        common::QueryDomainRepository* domain_repo = nullptr,
        common::PostgresBudgetRepository* budget_repo = nullptr,
        common::RedisClient* redis = nullptr);

    grpc::Status executePlan(
        grpc::ServerContext* context,
        const agent_communication::ExecutePlanRequest* request,
        agent_communication::ExecutePlanResponse* response);

    grpc::Status replayQuery(
        grpc::ServerContext* context,
        const agent_communication::ReplayQueryRequest* request,
        agent_communication::ReplayQueryResponse* response);

    grpc::Status exportConversation(
        grpc::ServerContext* context,
        const agent_communication::ExportConversationRequest* request,
        agent_communication::ExportConversationResponse* response);

private:
    orchestrator::TaskPlanner* task_planner_;
    orchestrator::TaskExecutor* task_executor_;
    orchestrator::AgentRouter* agent_router_;
    common::MemoryService* memory_service_;
    common::RpcConfig* rpc_config_;
    // P2(g/h): durable pipeline + PG-authoritative memory assembly. Nullable
    // (unit tests); production wires all three via AIQueryServiceImpl.
    common::QueryDomainRepository* domain_repo_ = nullptr;
    common::PostgresBudgetRepository* budget_repo_ = nullptr;
    common::RedisClient* redis_ = nullptr;
};

} // namespace server
} // namespace agent_rpc
