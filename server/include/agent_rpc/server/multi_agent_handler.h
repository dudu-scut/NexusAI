/**
 * @file multi_agent_handler.h
 * @brief Multi-agent orchestration query handling (extracted from ai_query_service.cpp)
 *
 * Contains: handleMultiAgentQuery, handleMultiAgentQueryStream, initializeOrchestrator
 */

#pragma once

#include "agent_rpc/common/types.h"
#include "agent_rpc/a2a_adapter/a2a_adapter.h"
#include "agent_rpc/orchestrator/agent_router.h"
#include "agent_rpc/orchestrator/task_planner.h"
#include "agent_rpc/orchestrator/task_executor.h"
#include "agent_rpc/orchestrator/result_aggregator.h"

#include <grpcpp/grpcpp.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace agent_communication {
class AIQueryRequest;
class AIQueryResponse;
class AIStreamEvent;
}

namespace agent_rpc {
namespace common { class RedisClient; class AgentRuntimeRepository; }
namespace server {

/**
 * @brief Handles multi-agent query orchestration
 *
 * Encapsulates the logic for planning, executing, and aggregating
 * multi-agent queries (both sync and streaming).
 */
class MultiAgentHandler {
public:
    using StatusUpdateFn = std::function<void(const std::string& task_id,
                                               const std::string& state,
                                               const std::string& agent_id,
                                               const std::string& agent_name,
                                               const std::string& error_msg)>;
    using MetricsRecordFn = std::function<void(const std::string& method,
                                                int64_t duration_ms,
                                                bool success)>;

    MultiAgentHandler(
        orchestrator::TaskPlanner* planner,
        orchestrator::AgentRouter* router,
        orchestrator::TaskExecutor* executor,
        orchestrator::ResultAggregator* aggregator,
        a2a_adapter::A2AAdapter* adapter,
        common::RpcConfig* config);

    void setCallbacks(StatusUpdateFn status_fn, MetricsRecordFn metrics_fn);

    /**
     * @brief Wire the agent_invocations producer repository (non-owning).
     *
     * The orchestrator path records one owner-scoped fact per real agent
     * call (single-agent fast path: one fact; multi-agent DAG: one fact per
     * subtask). Owner always comes from the thread-local auth context, never
     * from the request body; write failures are logged only.
     */
    void setInvocationRepository(common::AgentRuntimeRepository* repository);

    /**
     * @brief Static factory: create and initialize orchestrator components
     */
    static bool initializeOrchestrator(
        const std::string& api_key,
        const std::string& model,
        const std::string& api_url,
        common::RedisClient* redis_client,
        const common::RpcConfig& rpc_config,
        std::unique_ptr<orchestrator::AgentRouter>& out_router,
        std::unique_ptr<orchestrator::TaskPlanner>& out_planner,
        std::unique_ptr<orchestrator::TaskExecutor>& out_executor,
        std::unique_ptr<orchestrator::ResultAggregator>& out_aggregator);

    /**
     * @brief Handle a synchronous multi-agent query
     */
    grpc::Status handleQuery(
        grpc::ServerContext* context,
        const agent_communication::AIQueryRequest* request,
        agent_communication::AIQueryResponse* response,
        const std::string& request_id);

    /**
     * @brief Handle a streaming multi-agent query
     */
    grpc::Status handleQueryStream(
        grpc::ServerContext* context,
        const agent_communication::AIQueryRequest* request,
        grpc::ServerWriter<agent_communication::AIStreamEvent>* writer,
        const std::string& request_id);

    // ── P10: single-intent fast path ────────────────────────────────────

    /**
     * @brief P10: conservative multi-intent signal detector.
     *
     * Public static so tests can pin the veto rules. Strict by design: any
     * parallel/sequence signal (Chinese connectors such as 然后/并且/接着/
     * 先…再, numbered lists "1."/"2.", English "and then"/"first…then",
     * clause separators) vetoes the fast path and the query goes through
     * full planning.
     */
    static bool hasMultiIntentSignals(const std::string& text);

    /**
     * @brief P10: injectable high-confidence skill resolver (test seam).
     *
     * When unset, agent_router_->resolveHighConfidenceSkill() is used.
     */
    using FastPathSkillFn = std::function<std::optional<
        orchestrator::AgentRouter::HighConfidenceSkill>(const std::string&)>;
    void setFastPathSkillResolver(FastPathSkillFn fn);

    /**
     * @brief P10: decide whether the single-intent fast path applies and,
     * if so, fill a single-agent plan without any planning LLM call.
     *
     * Requires NEXUSAI_SINGLE_INTENT_FAST_PATH=1, a high-confidence skill
     * hit, and no multi-intent signals. Only constructs the plan — it never
     * emits stream events.
     */
    bool tryBuildFastPathPlan(const std::string& question,
                              orchestrator::ExecutionPlan& plan);

private:
    orchestrator::TaskPlanner* task_planner_;
    orchestrator::AgentRouter* agent_router_;
    orchestrator::TaskExecutor* task_executor_;
    orchestrator::ResultAggregator* result_aggregator_;
    a2a_adapter::A2AAdapter* a2a_adapter_;
    common::RpcConfig* rpc_config_;

    StatusUpdateFn update_status_;
    MetricsRecordFn record_metrics_;

    // agent_invocations producer (observability facts, best-effort).
    common::AgentRuntimeRepository* invocation_repository_ = nullptr;
    void recordInvocationFact(const std::string& query_log_id,
                              const std::string& agent_id,
                              const std::string& skill_name,
                              const std::string& status,
                              std::int64_t latency_ms);

    orchestrator::ExecutionPlan planQuery(const std::string& question);
    std::function<std::string(const std::string&, const std::string&)>
        buildCallAgent(const agent_communication::AIQueryRequest* request,
                       int effective_timeout_seconds);

    // P20: in-flight A2A call registry. Every DAG subtask registers a
    // per-call abort flag keyed by agent URL before the blocking
    // send_message; a timed-out subtask flips the flag, which aborts the
    // blocking HTTP transfer via the HttpClient progress callback.
    // multimap + flag-matched erase: parallel subtasks may share one URL,
    // so a URL can carry several live flags.
    // Each entry carries its owning request_id: cancellation is scoped to
    // the request that timed out, so a concurrent request hitting the same
    // agent URL is never aborted as collateral (R18).
    struct InFlightCall {
        std::shared_ptr<std::atomic<bool>> flag;
        std::string owner_request_id;
    };
    void cancelInFlight(const std::string& agent_url,
                        const std::string& owner_request_id);
    void unregisterInFlight(const std::string& agent_url,
                            const std::shared_ptr<std::atomic<bool>>& flag);
    std::mutex in_flight_mutex_;
    std::unordered_multimap<std::string, InFlightCall>
        in_flight_calls_;

    // P10: injected fast-path skill resolver (null → router embedding tier).
    FastPathSkillFn fast_path_skill_resolver_;

    // P10 helpers shared by the sync/stream fast-path fallback logic.
    bool executeSingleAgentSync(const orchestrator::ExecutionPlan& plan,
                                const agent_communication::AIQueryRequest& request,
                                agent_communication::AIQueryResponse* response);

    enum class SingleStreamOutcome { Success, Cancelled, Failed };
    SingleStreamOutcome executeSingleAgentStream(
        const orchestrator::ExecutionPlan& plan,
        grpc::ServerContext* context,
        const agent_communication::AIQueryRequest* request,
        grpc::ServerWriter<agent_communication::AIStreamEvent>* writer,
        const std::string& request_id,
        std::chrono::steady_clock::time_point start_time,
        bool fast_path_probe = false);
};

} // namespace server
} // namespace agent_rpc
