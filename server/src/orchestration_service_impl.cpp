/**
 * @file orchestration_service_impl.cpp
 * @brief OrchestrationService gRPC method implementations
 *
 * Extracted from ai_query_service.cpp:
 *   - ExecutePlan  (Batch 4 U4: user-modified DAG execution)
 *   - ReplayQuery  (Batch 5: exact / route-only replay)
 *   - ExportConversation (Batch 6: Markdown / HTML export)
 */

#include "agent_rpc/server/orchestration_service_impl.h"
#include "agent_rpc/server/auth_interceptor.h"
#include "agent_rpc/server/query_helpers.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/query_domain_repository.h"
#include "agent_rpc/common/postgres_budget_repository.h"
#include "agent_rpc/common/redis_client.h"
#include "agent_rpc/common/profile_summarizer.h"
#include "agent_rpc/orchestrator/export_service.h"
#include "agent_rpc/orchestrator/replay_service.h"

#include <a2a/client/a2a_client.hpp>
#include <nlohmann/json.hpp>

#include "orchestration.grpc.pb.h"
#include "orchestration.pb.h"

namespace agent_rpc {
namespace server {

OrchestrationServiceImpl::OrchestrationServiceImpl(
    orchestrator::TaskPlanner* planner,
    orchestrator::TaskExecutor* executor,
    orchestrator::AgentRouter* router,
    common::MemoryService* memory,
    common::RpcConfig* config,
    common::QueryDomainRepository* domain_repo,
    common::PostgresBudgetRepository* budget_repo,
    common::RedisClient* redis)
    : task_planner_(planner)
    , task_executor_(executor)
    , agent_router_(router)
    , memory_service_(memory)
    , rpc_config_(config)
    , domain_repo_(domain_repo)
    , budget_repo_(budget_repo)
    , redis_(redis) {
}

grpc::Status OrchestrationServiceImpl::executePlan(
    grpc::ServerContext* context,
    const agent_communication::ExecutePlanRequest* request,
    agent_communication::ExecutePlanResponse* response) {

    if (!AuthInterceptor::isAuthenticated()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                           "Valid authentication token required");
    }

    if (context->IsCancelled()) {
        return grpc::Status(grpc::StatusCode::CANCELLED, "Request cancelled");
    }

    std::string trace_id = QueryHelpers::generateRequestId();
    response->set_trace_id(trace_id);

    const auto& dag = request->dag();
    if (dag.nodes_size() == 0) {
        auto* status = response->mutable_status();
        status->set_code(-1);
        status->set_message("DAG must contain at least one node");
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                           "DAG must contain at least one node");
    }

    // P15 P0(a): the owner comes from the thread-local auth context, never
    // from the request body (same rule as the durable Query pipeline).
    std::string user_id = AuthInterceptor::currentUserId();

    // P15 P2(g): durable parity. The request gets a deterministic
    // query_log id (idempotency for retries), a running query_log/trace
    // pair, budget reservation and a terminal finalize — same shape as the
    // Query pipeline. Null repositories (unit tests) skip the whole block.
    const std::string request_id = "execute-plan-" + trace_id;
    const std::string context_id =
        request->context_id().empty() ? request_id : request->context_id();
    const bool durable = (domain_repo_ != nullptr) && (budget_repo_ != nullptr);

    // Crash guard: PG/Redis faults must never escape into the gRPC handler
    // (same contract as the Query pipeline). The durable initialization and
    // the memory assembly below all live inside this try.
    try {
    if (durable) {
        domain_repo_->ensureConversation(user_id, context_id, "");

        nlohmann::json plan_snapshot = nlohmann::json::array();
        for (int i = 0; i < dag.nodes_size(); ++i) {
            const auto& node = dag.nodes(i);
            nlohmann::json entry;
            entry["id"] = node.id();
            entry["description"] = node.description();
            entry["agent_id"] = node.agent_id();
            entry["dependencies"] = nlohmann::json::array();
            for (int j = 0; j < node.dependencies_size(); ++j) {
                entry["dependencies"].push_back(node.dependencies(j));
            }
            plan_snapshot.push_back(std::move(entry));
        }

        common::QueryLogRecord log;
        log.id = request_id;
        log.owner_id = user_id;
        log.conversation_id = context_id;
        log.request_text = "ExecutePlan (user-approved DAG)";
        log.route_decision = "execute-plan";
        log.execution_plan = plan_snapshot.dump();
        log.status = "running";
        domain_repo_->createQueryLog(log);

        common::TraceRecord trace_row;
        trace_row.id = "trace-" + request_id;
        trace_row.owner_id = user_id;
        trace_row.query_log_id = request_id;
        trace_row.status = "running";
        domain_repo_->createTrace(trace_row);

        // Budget reservation (idempotent per request_id); rejection ends
        // the run with a persisted "rejected" terminal state.
        const auto reserve = budget_repo_->reserve(
            user_id, context_id, request_id,
            static_cast<std::int64_t>(64 + dag.nodes_size() * 128),
            common::BudgetLimits{});
        if (!reserve.accepted) {
            log.status = "rejected";
            log.response_text = "Budget exhausted";
            domain_repo_->updateQueryLog(log);
            trace_row.status = "rejected";
            domain_repo_->updateTrace(trace_row);
            auto* status = response->mutable_status();
            status->set_code(-1);
            status->set_message("Budget exhausted");
            return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                                "Budget exhausted");
        }
    }

    LOG_INFO("ExecutePlan: " + std::to_string(dag.nodes_size()) +
             " nodes (trace: " + trace_id + ")");

    // Convert protobuf DAG → internal ExecutionPlan
    orchestrator::ExecutionPlan plan;
    plan.original_query = "ExecutePlan (user-modified DAG)";
    plan.is_single_agent = false;
    plan.tasks.clear();

    for (int i = 0; i < dag.nodes_size(); ++i) {
        const auto& node = dag.nodes(i);
        orchestrator::SubTask st;
        st.id = node.id();
        st.description = node.description();
        st.preferred_agent_id = node.agent_id();
        for (int j = 0; j < node.dependencies_size(); ++j) {
            st.depends_on.push_back(node.dependencies(j));
        }
        plan.tasks.push_back(std::move(st));
    }

    // P15 P2(h): memory convergence — the assembly mirrors the main query
    // path (PG window authoritative + Redis hints/profile acceleration),
    // replacing the legacy pure-Redis buildSystemContext. One memory block
    // is built once and each node prompt receives exactly ONE copy.
    std::string memory_ctx;
    if (!user_id.empty() && memory_service_) {
        if (domain_repo_) {
            // PG conversation window (authoritative).
            const auto messages = domain_repo_->listMessages(user_id, context_id);
            constexpr std::size_t kMaxHistory = 20;
            const std::size_t start =
                messages.size() > kMaxHistory ? messages.size() - kMaxHistory : 0;
            for (std::size_t index = start; index < messages.size(); ++index) {
                memory_ctx += messages[index].role + ": " +
                              messages[index].content + "\n";
            }
            // Redis acceleration tiers (silent degradation).
            const std::string hints = memory_service_->getUserMemory(user_id);
            if (!hints.empty()) {
                memory_ctx += "[User Memory]\n" + hints + "\n";
            }
            const std::string summary =
                memory_service_->getCrossAgentSummary(context_id);
            if (!summary.empty()) {
                memory_ctx += "[Prior Context]\n" + summary + "\n";
            }
            // User profile read-back (same shape as P17(k)).
            if (redis_ && redis_->isConnected()) {
                std::string profile_raw;
                if (redis_->get("user_profile:" + user_id, profile_raw) &&
                    !profile_raw.empty()) {
                    try {
                        const auto profile_json = nlohmann::json::parse(profile_raw);
                        const std::string identity =
                            profile_json.value("identity", nlohmann::json::object()).dump();
                        const std::string preferences =
                            profile_json.value("preferences", nlohmann::json::array()).dump();
                        const std::string profile_summary =
                            common::ProfileSummarizer::summarize(identity, preferences);
                        if (!profile_summary.empty()) {
                            memory_ctx += "[User Profile] " + profile_summary + "\n";
                        }
                    } catch (const nlohmann::json::exception&) {
                        // Corrupt profile — silent degradation guard.
                    }
                }
            }
        } else {
            // Legacy fallback (no repository wired — unit tests).
            auto sys_ctx = memory_service_->buildSystemContext(
                user_id, request->context_id(), "");
            if (!sys_ctx.user_memory().empty()) {
                memory_ctx = "[User Context]\n" + sys_ctx.user_memory() + "\n";
            }
            if (!sys_ctx.cross_agent_summary().empty()) {
                memory_ctx += "[Prior Context]\n" + sys_ctx.cross_agent_summary() + "\n";
            }
        }
    }

    // Build call_agent lambda
    auto call_agent = [this, &memory_ctx](const std::string& agent_url,
                             const std::string& prompt) -> std::string {
        std::string enriched_prompt = prompt;
        if (!memory_ctx.empty()) {
            enriched_prompt = memory_ctx + "\n" + prompt;
        }

        a2a::A2AClient client(agent_url);
        client.set_timeout(rpc_config_->timeout_seconds);

        a2a::AgentMessage msg = a2a::AgentMessage::create()
            .with_role(a2a::MessageRole::User)
            .with_text(enriched_prompt);

        auto params = a2a::MessageSendParams::create().with_message(msg);
        auto a2a_response = client.send_message(params);
        if (a2a_response.is_task()) {
            for (const auto& artifact : a2a_response.as_task().artifacts()) {
                if (artifact.content().has_value()) {
                    return artifact.content().value();
                }
            }
        } else if (a2a_response.is_message()) {
            return a2a_response.as_message().get_text();
        }
        return "";
    };

    try {
        // Resolve agents from the DAG
        for (auto& task : plan.tasks) {
            if (!task.preferred_agent_id.empty() && agent_router_) {
                auto agent = agent_router_->getAgent(task.preferred_agent_id);
                if (agent.has_value() && agent->is_healthy) {
                    // preferred_agent_id remains as-is; the call_agent
                    // lambda receives the agent_url from the executor
                }
            }
        }
        auto results = task_executor_->execute(plan, call_agent);

        // P15 P2(g): terminal finalize (durable mode) — same shape as the
        // Query pipeline; the execution result summary lands in the trace
        // payload and the query log flips to completed.
        if (durable) {
            nlohmann::json payload;
            payload["status"] = "completed";
            payload["request_id"] = request_id;
            nlohmann::json spans = nlohmann::json::array();
            for (const auto& [tid, result] : results) {
                nlohmann::json entry;
                entry["subtask_id"] = tid;
                entry["success"] = result.success;
                entry["duration_ms"] = result.duration_ms;
                entry["agent_id"] = result.agent_id;
                spans.push_back(std::move(entry));
            }
            payload["spans"] = spans;

            common::QueryLogRecord log;
            log.id = request_id;
            log.owner_id = user_id;
            log.conversation_id = context_id;
            log.response_text = "DAG executed (" +
                                std::to_string(plan.tasks.size()) + " subtasks)";
            log.status = "completed";
            domain_repo_->updateQueryLog(log);

            common::TraceRecord trace_row;
            trace_row.id = "trace-" + request_id;
            trace_row.owner_id = user_id;
            trace_row.query_log_id = request_id;
            trace_row.trace_payload = payload.dump();
            trace_row.status = "completed";
            domain_repo_->updateTrace(trace_row);
        }

        auto* status = response->mutable_status();
        status->set_code(0);
        status->set_message("OK");

        LOG_INFO("ExecutePlan completed: " + trace_id +
                 " (" + std::to_string(plan.tasks.size()) + " subtasks)");
        return grpc::Status::OK;

    } catch (const std::exception& e) {
        LOG_ERROR("ExecutePlan failed: " + trace_id + " - " + e.what());
        // P15 P2(g): failure terminal state (durable mode).
        if (durable) {
            common::QueryLogRecord log;
            log.id = request_id;
            log.owner_id = user_id;
            log.conversation_id = context_id;
            log.response_text = e.what();
            log.status = "failed";
            domain_repo_->updateQueryLog(log);

            common::TraceRecord trace_row;
            trace_row.id = "trace-" + request_id;
            trace_row.owner_id = user_id;
            trace_row.query_log_id = request_id;
            trace_row.status = "failed";
            domain_repo_->updateTrace(trace_row);
        }
        auto* status = response->mutable_status();
        status->set_code(-1);
        status->set_message(std::string("DAG execution failed: ") + e.what());
        return grpc::Status(grpc::StatusCode::INTERNAL,
                           std::string("DAG execution failed: ") + e.what());
    }
    } catch (const std::exception& e) {
        // Crash guard: PG/Redis infrastructure faults (durable init or
        // memory assembly) land here and must never escape into the gRPC
        // handler — the same contract the Query pipeline enforces.
        LOG_ERROR("ExecutePlan infrastructure fault: " + trace_id + " - " +
                  e.what());
        if (durable) {
            try {
                common::QueryLogRecord log;
                log.id = request_id;
                log.owner_id = user_id;
                log.conversation_id = context_id;
                log.response_text = e.what();
                log.status = "failed";
                domain_repo_->updateQueryLog(log);
            } catch (...) {
                // The store itself is down — nothing more to persist.
            }
        }
        auto* status = response->mutable_status();
        status->set_code(-1);
        status->set_message(
            std::string("ExecutePlan infrastructure unavailable: ") + e.what());
        return grpc::Status(
            grpc::StatusCode::UNAVAILABLE,
            std::string("ExecutePlan infrastructure unavailable: ") + e.what());
    }
}

grpc::Status OrchestrationServiceImpl::replayQuery(
    grpc::ServerContext* context,
    const agent_communication::ReplayQueryRequest* request,
    agent_communication::ReplayQueryResponse* response) {

    (void)context;

    if (!AuthInterceptor::isAuthenticated()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                           "Valid authentication token required");
    }

    // The owner always comes from the authenticated session. The durable
    // ReplayService loads the original trace from PostgreSQL (owner-scoped;
    // cross-owner or unknown traces are NOT_FOUND) and either compares routes
    // (mode=route, no execution) or re-executes under a NEW request id
    // (mode=exact) without ever modifying the original records.
    return orchestrator::ReplayService::handleReplayRequest(
        AuthInterceptor::currentUserId(), request, response);
}

grpc::Status OrchestrationServiceImpl::exportConversation(
    grpc::ServerContext* context,
    const agent_communication::ExportConversationRequest* request,
    agent_communication::ExportConversationResponse* response) {

    (void)context;

    if (!AuthInterceptor::isAuthenticated()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                           "Valid authentication token required");
    }

    // Owner-scoped export straight from PostgreSQL conversation messages;
    // missing or foreign conversations are NOT_FOUND and HTML output is
    // fully escaped.
    return orchestrator::ExportService::handleExportRequest(
        AuthInterceptor::currentUserId(), request, response);
}

} // namespace server
} // namespace agent_rpc
