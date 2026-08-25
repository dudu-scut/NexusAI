/**
 * @file ai_query_service.h
 * @brief AI Query Service implementation for gRPC
 * 
 * Requirements: 2.1, 2.2, 2.5
 *
 * Architecture: This class composes three helper modules:
 *   - OrchestrationServiceImpl  (ExecutePlan, ReplayQuery, ExportConversation)
 *   - MultiAgentHandler         (multi-agent sync/stream query handling)
 *   - QueryHelpers              (task status, metrics, agent-switch, UUID)
 */

#pragma once

#include "agent_rpc/common/types.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/metrics.h"
#include "agent_rpc/common/circuit_breaker.h"
#include "agent_rpc/common/memory_service.h"
#include "agent_rpc/common/query_domain_repository.h"
#include "agent_rpc/common/postgres_budget_repository.h"
#include "agent_rpc/common/agent_runtime_repository.h"
#include "agent_rpc/a2a_adapter/a2a_adapter.h"
#include "agent_rpc/a2a_adapter/a2a_config.h"
#include "agent_rpc/orchestrator/agent_router.h"
#include "agent_rpc/orchestrator/task_planner.h"
#include "agent_rpc/orchestrator/task_executor.h"
#include "agent_rpc/orchestrator/result_aggregator.h"
#include "agent_rpc/server/orchestration_service_impl.h"
#include "agent_rpc/server/multi_agent_handler.h"
#include "agent_rpc/server/query_helpers.h"

#include "ai_query.grpc.pb.h"
#include "ai_query.pb.h"
#include "orchestration.grpc.pb.h"
#include "orchestration.pb.h"

#include <grpcpp/grpcpp.h>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <atomic>

namespace agent_rpc {
namespace server {

// Defined in multi_agent_handler.cpp: the streaming multi-agent handler keeps
// its accumulated answer/error in a thread-local slot instead of emitting
// terminal stream events itself. The top-level QueryStream relay is the only
// component allowed to emit "complete"/"error" events. The acting agent id/
// name are handed back the same way so the streaming path can run the
// agent-switch memory pipeline (last_agent + cross-agent summary).
std::string takeMultiAgentStreamedAnswer();
std::string takeMultiAgentStreamError();
std::string takeMultiAgentStreamedAgentId();
std::string takeMultiAgentStreamedAgentName();

/**
 * @brief AI Query Service implementation
 * 
 * Implements the AIQueryService gRPC service, bridging RPC requests
 * to the A2A protocol via the A2AAdapter.
 *
 * Also implements OrchestrationService (ExecutePlan, ReplayQuery, ExportConversation)
 * via delegation to OrchestrationServiceImpl.
 */
class AIQueryServiceImpl final : public agent_communication::AIQueryService::Service,
                                  public agent_communication::OrchestrationService::Service {
public:
    AIQueryServiceImpl();
    ~AIQueryServiceImpl();
    
    /**
     * @brief Initialize the service with configuration
     * @param rpc_config RPC configuration
     * @param a2a_config A2A adapter configuration
     * @param redis Redis client (non-owning, cache-only)
     * @param store PostgreSQL store owned by RpcServer (non-owning reference)
     * @param domain durable query-domain repository owned by RpcServer
     * @param budget PostgreSQL budget repository owned by RpcServer
     * @return true if initialization successful
     */
    bool initialize(const common::RpcConfig& rpc_config,
                   const a2a_adapter::A2AConfig& a2a_config,
                   common::RedisClient* redis,
                   common::PostgresStore& store,
                   common::QueryDomainRepository& domain,
                   common::PostgresBudgetRepository& budget);

    /**
     * @brief Wire the agent_invocations producer repository (non-owning).
     *
     * Query/QueryStream become the production writer of agent_invocations.
     * Facts are owner-scoped via the authenticated session; write failures
     * are logged only and never affect the query outcome (observability
     * data, not the source of truth). Forwarded to MultiAgentHandler when
     * the orchestrator path is enabled.
     */
    void setInvocationRepository(common::AgentRuntimeRepository* repository);
    
    /**
     * @brief Shutdown the service
     */
    void shutdown();
    
    /**
     * @brief Check if service is available
     */
    bool isAvailable() const;
    
    grpc::Status Query(
        grpc::ServerContext* context,
        const agent_communication::AIQueryRequest* request,
        agent_communication::AIQueryResponse* response) override;
    
    grpc::Status QueryStream(
        grpc::ServerContext* context,
        const agent_communication::AIQueryRequest* request,
        grpc::ServerWriter<agent_communication::AIStreamEvent>* writer) override;
    
    grpc::Status GetQueryStatus(
        grpc::ServerContext* context,
        const agent_communication::QueryStatusRequest* request,
        agent_communication::QueryStatusResponse* response) override;

    grpc::Status GetAgentMetrics(
        grpc::ServerContext* context,
        const agent_communication::GetAgentMetricsRequest* request,
        agent_communication::GetAgentMetricsResponse* response) override;

    grpc::Status ExecutePlan(
        grpc::ServerContext* context,
        const agent_communication::ExecutePlanRequest* request,
        agent_communication::ExecutePlanResponse* response) override;

    grpc::Status ReplayQuery(
        grpc::ServerContext* context,
        const agent_communication::ReplayQueryRequest* request,
        agent_communication::ReplayQueryResponse* response) override;

    grpc::Status ExportConversation(
        grpc::ServerContext* context,
        const agent_communication::ExportConversationRequest* request,
        agent_communication::ExportConversationResponse* response) override;
    
    a2a_adapter::A2AAdapter* getA2AAdapter() { return a2a_adapter_.get(); }

    orchestrator::AgentRouter* getAgentRouter() { return agent_router_.get(); }

    // Same source of truth the Query pipeline uses to pick the route label
    // ("multi-agent" vs "single-agent-a2a"). Prefer this over probing
    // getAgentRouter(): the router can exist while orchestration is off.
    bool isOrchestratorEnabled() const { return orchestrator_enabled_.load(); }

    common::MemoryService* getMemoryService() { return memory_service_.get(); }

private:
    std::unique_ptr<a2a_adapter::A2AAdapter> a2a_adapter_;
    std::shared_ptr<common::CircuitBreaker> circuit_breaker_;
    common::RpcConfig rpc_config_;
    std::atomic<bool> initialized_{false};
    std::unique_ptr<common::MemoryService> memory_service_;
    common::RedisClient* redis_client_ = nullptr;

    // Durable pipeline dependencies (non-owning; RpcServer owns the objects)
    common::PostgresStore* store_ = nullptr;
    common::QueryDomainRepository* domain_repo_ = nullptr;
    common::PostgresBudgetRepository* budget_repo_ = nullptr;
    common::BudgetLimits budget_limits_;

    // Per-request durable state carried through the fixed six-step pipeline.
    struct DurableQueryRun {
        std::string owner_id;
        std::string conversation_id;
        std::string request_id;
        std::string trace_row_id;
        std::string question;
        std::string model;
        std::int64_t estimated_tokens = 0;
        bool first_attempt = false;
        std::atomic<bool> finalized{false};
    };

    // Steps 2-3: ensure conversation, create running query_log/trace rows.
    bool beginDurableRows(DurableQueryRun& run, const std::string& route,
                          const std::string& plan_json);
    // Step 4: PostgreSQL budget reservation; rejected requests are persisted
    // as "rejected" before the caller returns RESOURCE_EXHAUSTED.
    grpc::Status reserveBudgetOrReject(DurableQueryRun& run);
    // Step 5: SystemContext assembled from PostgreSQL (Redis is cache-only).
    // sandbox_request mirrors the write-side !request->sandbox() guard: when
    // true, the Redis long-term-memory / summary recall is skipped.
    void buildSystemContextFromPg(const std::string& owner_id,
                                  const std::string& conversation_id,
                                  agent_communication::SystemContext* system_context,
                                  bool sandbox_request = false);
    // Step 6: terminal persistence, executed at most once per run.
    void finalizeDurableQuery(DurableQueryRun& run, const std::string& status,
                              const std::string& response_text,
                              const std::string& error_message);
    // Crash guard: best-effort "failed" finalize when the pipeline throws,
    // so runtime PG/Redis faults never escape into the gRPC handler.
    void abortDurableRun(DurableQueryRun& run, const std::string& reason);
    static std::int64_t estimateTokens(const std::string& question);
    static common::BudgetLimits budgetLimitsFromEnvironment();

    // agent_invocations producer: best-effort owner-scoped fact write; a
    // failure here is logged and swallowed, never propagated to the caller.
    void recordInvocationFact(const std::string& owner_id,
                              const std::string& query_log_id,
                              const std::string& agent_id,
                              const std::string& skill_name,
                              const std::string& status,
                              std::int64_t latency_ms);

    // Agent-switch memory pipeline shared by Query() and QueryStream(): looks
    // up the taking-over agent's name/duties from the router and drives
    // helpers_.handleAgentSwitch (last_agent write + async cross-agent
    // summary). Callers wrap this in runCacheOnly — Redis faults are
    // swallowed and never affect the query outcome. agent_id must be the
    // real acting agent (never "default" unless it really is one).
    void writeAgentSwitchMemory(const std::string& owner_id,
                                const std::string& context_id,
                                const std::string& agent_id,
                                const std::string& agent_name);

    // Non-owning; RpcServer owns the repository.
    common::AgentRuntimeRepository* invocation_repository_ = nullptr;

    // Multi-agent orchestration
    std::unique_ptr<orchestrator::AgentRouter> agent_router_;
    std::unique_ptr<orchestrator::TaskPlanner> task_planner_;
    std::unique_ptr<orchestrator::TaskExecutor> task_executor_;
    std::unique_ptr<orchestrator::ResultAggregator> result_aggregator_;
    std::atomic<bool> orchestrator_enabled_{false};

    // Memory: LLM client for cross-agent summary generation
    std::unique_ptr<LLMClient> memory_llm_client_;

    std::unique_ptr<OrchestrationServiceImpl> orchestration_impl_;
    std::unique_ptr<MultiAgentHandler> multi_agent_handler_;
    QueryHelpers helpers_;
};

} // namespace server
} // namespace agent_rpc