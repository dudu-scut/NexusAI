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
#include "agent_rpc/server/ai_query_service.h"
#include "agent_rpc/server/auth_interceptor.h"
#include "agent_rpc/server/query_helpers.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/query_domain_repository.h"
#include "agent_rpc/common/postgres_budget_repository.h"
#include "agent_rpc/common/redis_client.h"
#include "agent_rpc/common/key_validation.h"
#include "agent_rpc/common/profile_summarizer.h"
#include "agent_rpc/orchestrator/export_service.h"
#include "agent_rpc/orchestrator/replay_service.h"
#include "agent_rpc/orchestrator/result_aggregator.h"
#include "agent_rpc/a2a_adapter/url_validation.h"
#include "agent_rpc/registry/service_registry.h"
#include "agent_rpc/server/in_flight_registry.h"

#include <a2a/client/a2a_client.hpp>
#include <nlohmann/json.hpp>

#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

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

    // A4: validate the user-submitted DAG up front instead of discovering
    // problems mid-execution. Duplicate ids and dangling dependencies used
    // to surface only as an executor-side "Circular dependency detected"
    // (or a wedged topological sort) — reject them with a precise error.
    {
        std::unordered_set<std::string> known_ids;
        std::string dup_error;
        for (const auto& node : dag.nodes()) {
            if (node.id().empty()) {
                dup_error = "DAG node id must not be empty";
                break;
            }
            if (!known_ids.insert(node.id()).second) {
                dup_error = "DAG contains duplicate node id: " + node.id();
                break;
            }
        }
        if (dup_error.empty()) {
            for (const auto& node : dag.nodes()) {
                for (const auto& dep : node.dependencies()) {
                    if (known_ids.find(dep) == known_ids.end()) {
                        dup_error = "DAG node " + node.id() +
                                    " depends on unknown node id: " + dep;
                        break;
                    }
                }
                if (!dup_error.empty()) break;
            }
        }
        if (dup_error.empty()) {
            // Kahn cycle pre-check (execution-time check stays as defense).
            std::unordered_map<std::string, int> in_degree;
            std::unordered_map<std::string, std::vector<std::string>> dependents;
            for (const auto& node : dag.nodes()) {
                in_degree[node.id()] += 0;
                for (const auto& dep : node.dependencies()) {
                    dependents[dep].push_back(node.id());
                    in_degree[node.id()]++;
                }
            }
            std::queue<std::string> ready;
            for (const auto& [id, deg] : in_degree) {
                if (deg == 0) ready.push(id);
            }
            size_t emitted = 0;
            while (!ready.empty()) {
                const std::string id = ready.front();
                ready.pop();
                ++emitted;
                auto dit = dependents.find(id);
                if (dit == dependents.end()) continue;
                for (const auto& nxt : dit->second) {
                    if (--in_degree[nxt] == 0) ready.push(nxt);
                }
            }
            if (emitted < in_degree.size()) {
                dup_error = "DAG contains a dependency cycle";
            }
        }
        if (!dup_error.empty()) {
            auto* status = response->mutable_status();
            status->set_code(-1);
            status->set_message(dup_error);
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, dup_error);
        }
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
    // P25 (批次十一): context_id 入口白名单 — 含 sanitize 有损字符集
    // （: \n \r 控制符）或超长直接拒绝，让有损字符到不了键清洗函数。
    if (!common::isSafeKeyComponent(context_id)) {
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            "context_id contains characters unsafe for conversation keys");
    }
    const bool durable = (domain_repo_ != nullptr) && (budget_repo_ != nullptr);

    // Crash guard: PG/Redis faults must never escape into the gRPC handler
    // (same contract as the Query pipeline). The durable initialization and
    // the memory assembly below all live inside this try.
    try {
    if (durable) {
        if (!domain_repo_->ensureConversation(user_id, context_id, "")) {
            throw std::runtime_error("ensureConversation failed for " + context_id);
        }

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
        // The column is jsonb (COALESCE(NULLIF($5,'')::jsonb)) — a bare
        // string would throw 22P02 and fail every durable ExecutePlan.
        log.route_decision = nlohmann::json{{"mode", "execute-plan"}}.dump();
        log.execution_plan = plan_snapshot.dump();
        log.status = "running";
        if (!domain_repo_->createQueryLog(log)) {
            throw std::runtime_error("createQueryLog failed for " + request_id);
        }

        common::TraceRecord trace_row;
        trace_row.id = "trace-" + request_id;
        trace_row.owner_id = user_id;
        trace_row.query_log_id = request_id;
        trace_row.status = "running";
        if (!domain_repo_->createTrace(trace_row)) {
            throw std::runtime_error("createTrace failed for " + request_id);
        }

        // Budget reservation (idempotent per request_id); rejection ends
        // the run with a persisted "rejected" terminal state. Same four-layer
        // limits as the Query pipeline — an all-zero BudgetLimits would mean
        // "unlimited" and silently bypass the configured budgets.
        const auto reserve = budget_repo_->reserve(
            user_id, context_id, request_id,
            static_cast<std::int64_t>(64 + dag.nodes_size() * 128),
            AIQueryServiceImpl::budgetLimitsFromEnvironment());
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
            if (!sys_ctx.user_facts().empty()) {
                memory_ctx += "[User Facts]\n" + sys_ctx.user_facts() + "\n";
            }
            if (!sys_ctx.cross_agent_summary().empty()) {
                memory_ctx += "[Prior Context]\n" + sys_ctx.cross_agent_summary() + "\n";
            }
        }
    }

    // Build call_agent lambda. memory_ctx is captured BY VALUE: a timed-out
    // or disconnected subtask keeps executing this lambda on a pool worker
    // after execute() returns, so a reference capture would dangle once the
    // executePlan stack frame is gone (same contract as the handler's
    // buildCallAgent, see the TaskExecutor launch-site comment).
    auto call_agent = [this, memory_ctx, request_id](const std::string& agent_url,
                             const std::string& prompt) -> std::string {
        std::string enriched_prompt = prompt;
        if (!memory_ctx.empty()) {
            enriched_prompt = memory_ctx + "\n" + prompt;
        }

        // P21 L1: ExecutePlan used to be the one delegation path that skipped
        // URL validation entirely — apply the same real-parse checks as the
        // direct paths and the DAG buildCallAgent path.
        std::string url_err;
        if (!agent_rpc::a2a_adapter::validateAgentUrl(agent_url, url_err)) {
            throw std::runtime_error("Agent URL rejected: " + url_err);
        }
        // P21 L2/L3 (strict mode): validate-only flavor — resolve the host and
        // reject blacklisted IPs / non-whitelisted ports. (No CURLOPT_RESOLVE
        // pin here: this path constructs a bare A2AClient; the pin lands with
        // the shared call-agent helper consolidation.)
        if (agent_rpc::a2a_adapter::ssrfStrictModeEnabled()) {
            std::string host;
            std::string port_str;
            if (agent_rpc::a2a_adapter::splitAgentUrlHostPort(agent_url, host, port_str)) {
                std::vector<std::string> ips;
                std::string host_err;
                if (!agent_rpc::a2a_adapter::validateResolvedHost(host, ips, host_err)) {
                    throw std::runtime_error("Agent host rejected: " + host_err);
                }
            }
        }

        a2a::A2AClient client(agent_url);
        client.set_timeout(rpc_config_->timeout_seconds);
        // P24 C0: register the in-flight call so a subtask timeout or a
        // client disconnect aborts the blocking transfer via on_cancel.
        InFlightRegistration in_flight(agent_url, request_id);
        client.set_abort_flag(in_flight.flag().get());

        a2a::AgentMessage msg = a2a::AgentMessage::create()
            .with_role(a2a::MessageRole::User)
            .with_text(enriched_prompt);

        auto params = a2a::MessageSendParams::create().with_message(msg);
        auto a2a_response = client.send_message(params);
        if (a2a_response.is_task()) {
            // P24 C1②: remember the task id so a later timeout/disconnect
            // can send a best-effort protocol-level tasks/cancel.
            InFlightAbortRegistry::instance().setInFlightTaskId(
                agent_url, in_flight.flag(), a2a_response.as_task().id());
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
        // Client-disconnect propagation + executed-only health accounting.
        auto cancelled_probe = [context]() { return context->IsCancelled(); };
        // P24 C0/C1: timed-out / disconnected subtasks abort their
        // in-flight A2A call (scoped to this request, same as the handler
        // DAG path) and send a best-effort protocol-level tasks/cancel.
        auto on_cancel = [request_id](const std::string& agent_url) {
            auto& registry = InFlightAbortRegistry::instance();
            registry.cancelInFlight(agent_url, request_id);
            const std::string task_id =
                registry.inFlightTaskId(agent_url, request_id);
            if (!task_id.empty()) {
                try {
                    std::thread([agent_url, task_id]() {
                        try {
                            a2a::A2AClient client(agent_url);
                            client.set_timeout(3);
                            client.cancel_task(task_id);
                        } catch (...) {
                            // Best effort — remote agents may not implement cancel.
                        }
                    }).detach();
                } catch (const std::exception&) {
                    // Thread creation failed — the local abort above already
                    // bounded the transfer; the protocol-level cancel is lost.
                }
            }
        };
        // A6 (批次十一): ExecutePlan 持有独立 gRPC deadline——已越过则不再
        // 发起 DAG 执行（throw 走既有 catch 的 failed 终态收口，与执行中
        // 超时的最终语义一致）。
        if (context->deadline() != std::chrono::system_clock::time_point::max() &&
            std::chrono::duration_cast<std::chrono::seconds>(
                context->deadline() - std::chrono::system_clock::now())
                    .count() <= 0) {
            throw std::runtime_error("Deadline passed before DAG execution");
        }
        auto results = task_executor_->execute(plan, call_agent, nullptr, on_cancel,
                                               cancelled_probe);
        for (const auto& [tid, result] : results) {
            (void)tid;
            if (!result.executed) {
                continue;  // fabricated failure — no health signal
            }
            // executeSubtask fills agent_id with the actually executed agent;
            // records with an empty id carry no health signal and are skipped.
            if (!result.agent_id.empty()) {
                agent_rpc::registry::ServiceRegistry::recordAgentCall(
                    result.agent_id, result.success,
                    static_cast<double>(result.duration_ms));
                // P8 S2: latency feed for REAL completed subtask calls only
                // (ExecutePlan shares the router's load-balancer tier).
                if (result.success && agent_router_) {
                    agent_router_->recordEndpointLatency(
                        result.agent_id, result.duration_ms);
                }
            }
        }

        // B2: aggregate the per-subtask answers into a real final response
        // (concat strategy, zero LLM cost). The previous placeholder
        // "DAG executed (N subtasks)" buried every subtask answer in the
        // trace payload only.
        orchestrator::ResultAggregator concat_aggregator(
            orchestrator::AggregatorConfig{});  // default strategy: concat
        auto aggregated = concat_aggregator.aggregate(plan, results);

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
            log.response_text = aggregated.final_answer.empty()
                ? "DAG executed (" + std::to_string(plan.tasks.size()) + " subtasks)"
                : aggregated.final_answer;
            log.status = "completed";
            domain_repo_->updateQueryLog(log);

            // B2: the DAG's final answer joins the conversation history as a
            // regular assistant message (same durable idempotency scheme as
            // the Query pipeline), so Chat history shows plan-driven answers
            // identically to normal Q&A. The nexusai:conv:* projection keys
            // have no production writer (P1), so no invalidation is needed.
            domain_repo_->appendMessageAutoSequence(
                "msg-assistant-" + request_id, user_id, context_id, "assistant",
                log.response_text);

            common::TraceRecord trace_row;
            trace_row.id = "trace-" + request_id;
            trace_row.owner_id = user_id;
            trace_row.query_log_id = request_id;
            trace_row.trace_payload = payload.dump();
            trace_row.status = "completed";
            domain_repo_->updateTrace(trace_row);

            // Token ledger for the debited reservation (same estimate the
            // reserve used): without it the daily cost report never saw
            // ExecutePlan traffic while budget counters had been charged.
            common::TokenUsageLedgerRecord usage;
            usage.id = "usage-" + request_id;
            usage.owner_id = user_id;
            usage.query_log_id = request_id;
            usage.model = "execute-plan";
            usage.prompt_tokens =
                static_cast<std::int64_t>(64 + dag.nodes_size() * 128);
            usage.completion_tokens =
                static_cast<std::int64_t>(aggregated.final_answer.size()) / 4;
            usage.estimated = true;
            usage.cost_usd = "0";
            domain_repo_->appendTokenUsageLedger(usage);
        }

        auto* status = response->mutable_status();
        status->set_code(0);
        status->set_message("OK");

        LOG_INFO("ExecutePlan completed: " + trace_id +
                 " (" + std::to_string(plan.tasks.size()) + " subtasks)");
        return grpc::Status::OK;

    } catch (const std::exception& e) {
        LOG_ERROR("ExecutePlan failed: " + trace_id + " - " + e.what());
        const std::string safe_error = QueryHelpers::sanitizeErrorMessage(e.what());
        // P15 P2(g): failure terminal state (durable mode).
        if (durable) {
            common::QueryLogRecord log;
            log.id = request_id;
            log.owner_id = user_id;
            log.conversation_id = context_id;
            log.response_text = safe_error;
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
        status->set_message("DAG execution failed: " + safe_error);
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "DAG execution failed: " + safe_error);
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
        // Fixed client-facing text: pqxx exception details embed SQL and must
        // never reach the client (same rule as Query/Replay/Export).
        status->set_message("ExecutePlan infrastructure unavailable");
        return grpc::Status(
            grpc::StatusCode::UNAVAILABLE,
            "ExecutePlan infrastructure unavailable");
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
