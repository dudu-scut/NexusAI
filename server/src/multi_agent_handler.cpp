/**
 * @file multi_agent_handler.cpp
 * @brief Multi-agent query handling implementation
 *
 * Extracted from ai_query_service.cpp:
 *   - handleMultiAgentQuery (sync multi-agent path)
 *   - handleMultiAgentQueryStream (streaming multi-agent path)
 *   - initializeOrchestrator (static factory for orchestrator components)
 */

#include "agent_rpc/server/multi_agent_handler.h"
#include "agent_rpc/server/query_helpers.h"
#include "agent_rpc/server/auth_interceptor.h"
#include "agent_rpc/server/in_flight_registry.h"
#include "agent_rpc/common/agent_runtime_repository.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/env_loader.h"
#include "agent_rpc/common/trace_context.h"
#include "agent_rpc/common/memory_service.h"
#include "agent_rpc/registry/service_registry.h"
#include "agent_rpc/a2a_adapter/url_validation.h"

#include <a2a/client/a2a_client.hpp>
#include <a2a/llm_client.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "ai_query.grpc.pb.h"
#include "ai_query.pb.h"

namespace agent_rpc {
namespace server {

namespace {

// A3/P20-8: the planning LLM must not run a full 20s when the request's
// remaining gRPC deadline budget is already tight. Returns the per-call
// timeout (bounded by kMaxPlanningTimeoutSeconds) or 0 when the remaining
// budget is too small to plan at all — the caller then falls straight to
// the single-agent baseline instead of paying for a doomed plan call.
constexpr int kMaxPlanningTimeoutSeconds = 20;
constexpr int kPlanningReserveSeconds = 5;    // execution headroom
constexpr int kMinPlanningBudgetSeconds = 3;  // below this, skip planning
// B6 route-then-plan: prune the planning prompt's skill list to the
// embedding-nearest subset (NEXUSAI_PLAN_SKILL_PRUNE=1, default off).
// Skill set selection happens entirely at the call site — plan() itself is
// untouched. Falls back to the full set when the pruner yields nothing
// (planning against an empty set would disable the whole DAG).
std::unordered_map<std::string, std::string> prunedSkillsForPlanning(
    orchestrator::AgentRouter* router, const std::string& question,
    std::unordered_map<std::string, std::string> all_skills) {
    const bool enabled =
        agent_rpc::common::envOrDefault("NEXUSAI_PLAN_SKILL_PRUNE", "0") == "1";
    if (!enabled || !router || all_skills.empty()) {
        return all_skills;
    }
    constexpr int kPruneTopK = 20;
    constexpr float kPruneThreshold = 0.6f;
    constexpr size_t kMaxPrunedSkills = 50;
    auto ranked = router->rankSkillsBySimilarity(question, kPruneTopK, kPruneThreshold);
    if (ranked.empty()) {
        return all_skills;  // embedding tier unavailable — legacy behavior
    }
    std::unordered_map<std::string, std::string> subset;
    for (const auto& [skill, similarity] : ranked) {
        if (subset.size() >= kMaxPrunedSkills) break;
        auto it = all_skills.find(skill);
        if (it != all_skills.end()) {
            subset.emplace(skill, it->second);
        }
    }
    if (subset.empty()) {
        return all_skills;  // ranked names missed the registry map — fail open
    }
    LOG_INFO("Skill pruning for planning: " + std::to_string(all_skills.size()) +
             " -> " + std::to_string(subset.size()) + " skills (query similarity)");
    return subset;
}

int planningTimeoutFor(grpc::ServerContext* context, int effective_timeout_seconds) {
    if (context == nullptr ||
        context->deadline() == std::chrono::system_clock::time_point::max()) {
        return std::min(kMaxPlanningTimeoutSeconds,
                        std::max(1, effective_timeout_seconds));
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
        context->deadline() - std::chrono::system_clock::now()).count();
    if (remaining < kMinPlanningBudgetSeconds) {
        return 0;
    }
    const long long capped = remaining - kPlanningReserveSeconds;
    const long long bounded = std::min<long long>(
        capped, std::max(1, effective_timeout_seconds));
    return static_cast<int>(std::max<long long>(
        1, std::min<long long>(kMaxPlanningTimeoutSeconds, bounded)));
}

// MultiAgentHandler never emits terminal stream events. The top-level
// AIQueryServiceImpl::QueryStream is the single emitter of "complete" and
// "error" events; this thread-local slot hands the accumulated answer/error
// back to it after handleQueryStream returns.
struct StreamResultSlot {
    std::string answer;
    std::string error;
    // Acting agent of the streamed run: the single-agent path records the
    // executed agent, the multi-agent path records the last executed
    // subtask's agent. The service layer uses it for the agent-switch
    // memory pipeline (last_agent + cross-agent summary) on the streaming
    // path, which otherwise has no agent identity surface.
    std::string agent_id;
    std::string agent_name;
    // True once any non-terminal event reached writer->Write(). Guards the
    // P10 fast-path fallback: retrying is only safe when nothing was
    // delivered to the client (a status event may have been sent before the
    // agent reported an error).
    bool any_written = false;
};

thread_local StreamResultSlot tls_stream_result;

}  // namespace

std::string takeMultiAgentStreamedAnswer() {
    std::string answer = std::move(tls_stream_result.answer);
    tls_stream_result.answer.clear();
    return answer;
}

std::string takeMultiAgentStreamError() {
    std::string error = std::move(tls_stream_result.error);
    tls_stream_result.error.clear();
    return error;
}

std::string takeMultiAgentStreamedAgentId() {
    std::string agent_id = std::move(tls_stream_result.agent_id);
    tls_stream_result.agent_id.clear();
    return agent_id;
}

std::string takeMultiAgentStreamedAgentName() {
    std::string agent_name = std::move(tls_stream_result.agent_name);
    tls_stream_result.agent_name.clear();
    return agent_name;
}

MultiAgentHandler::MultiAgentHandler(
    orchestrator::TaskPlanner* planner,
    orchestrator::AgentRouter* router,
    orchestrator::TaskExecutor* executor,
    orchestrator::ResultAggregator* aggregator,
    a2a_adapter::A2AAdapter* adapter,
    common::RpcConfig* config)
    : task_planner_(planner)
    , agent_router_(router)
    , task_executor_(executor)
    , result_aggregator_(aggregator)
    , a2a_adapter_(adapter)
    , rpc_config_(config) {
}

void MultiAgentHandler::setCallbacks(StatusUpdateFn status_fn, MetricsRecordFn metrics_fn) {
    update_status_ = std::move(status_fn);
    record_metrics_ = std::move(metrics_fn);
}

void MultiAgentHandler::setInvocationRepository(
    common::AgentRuntimeRepository* repository) {
    invocation_repository_ = repository;
}

// ── P10: single-intent fast path ───────────────────────────────────────────

bool MultiAgentHandler::hasMultiIntentSignals(const std::string& text) {
    // Strict-by-design veto: any parallel/sequence signal sends the query
    // back to full planning. Missing a signal (wrong fast-path shortcut) is
    // worse than an extra planning call, so the detector errs wide.
    static const char* const kSignals[] = {
        // Chinese sequence/parallel connectors
        "然后", "并且", "接着", "以及", "其次", "另外", "此外",
        "同时", "还要", "还需", "一方面", "第一步", "第二步",
        // English sequence/parallel connectors
        "and then", "after that", "as well as", "in addition",
        "furthermore", "moreover", "firstly", "secondly",
    };

    // ASCII-lowercased copy for case-insensitive English matching.
    std::string lower;
    lower.reserve(text.size());
    for (const unsigned char c : text) {
        lower.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c));
    }
    for (const char* signal : kSignals) {
        if (lower.find(signal) != std::string::npos) {
            return true;
        }
    }

    // Paired connectors: 先…再 and first…then.
    if (text.find("先") != std::string::npos &&
        text.find("再") != std::string::npos) {
        return true;
    }
    if (lower.find("first") != std::string::npos &&
        lower.find("then") != std::string::npos) {
        return true;
    }

    // Numbered list markers ("1." / "2)" / "1、" ...) appearing at least
    // twice strongly indicate parallel subtasks. The boundary is "previous
    // byte is not ASCII alphanumeric" rather than whitespace-only: Chinese
    // punctuation is multi-byte UTF-8, so "1.写代码，2.写文档" would
    // otherwise miss the second marker (previous byte is a CJK byte) and
    // take the fast path. The relaxed check can only add vetoes, which
    // matches the strict-by-design goal.
    int numbered = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        // Short-circuit keeps text[i - 1] untouched when i == 0. char may be
        // signed, but any negative byte (CJK UTF-8 continuation) fails all
        // three ASCII-alphanumeric comparisons, which is exactly the intent.
        const bool at_boundary = (i == 0) ||
            !((text[i - 1] >= '0' && text[i - 1] <= '9') ||
              (text[i - 1] >= 'A' && text[i - 1] <= 'Z') ||
              (text[i - 1] >= 'a' && text[i - 1] <= 'z'));
        if (!at_boundary || text[i] < '0' || text[i] > '9') {
            continue;
        }
        size_t j = i;
        while (j < text.size() && text[j] >= '0' && text[j] <= '9') {
            ++j;
        }
        if (j >= text.size()) {
            continue;
        }
        const bool marker = text[j] == '.' || text[j] == ')' ||
                            text.compare(j, 3, "\xE3\x80\x81") == 0 ||  // 、
                            text.compare(j, 3, "\xEF\xBC\x89") == 0;    // ）
        if (marker && ++numbered >= 2) {
            return true;
        }
    }

    // Clause separators commonly join parallel intents; veto them too.
    if (text.find(';') != std::string::npos ||
        text.find("；") != std::string::npos) {
        return true;
    }

    return false;
}

void MultiAgentHandler::setFastPathSkillResolver(FastPathSkillFn fn) {
    fast_path_skill_resolver_ = std::move(fn);
}

bool MultiAgentHandler::tryBuildFastPathPlan(const std::string& question,
                                             orchestrator::ExecutionPlan& plan) {
    // P10: default off; only an explicit opt-in enables the fast path.
    if (common::envOrDefault("NEXUSAI_SINGLE_INTENT_FAST_PATH", "0") != "1") {
        return false;
    }

    std::optional<orchestrator::AgentRouter::HighConfidenceSkill> hit;
    if (fast_path_skill_resolver_) {
        hit = fast_path_skill_resolver_(question);
    } else if (agent_router_) {
        hit = agent_router_->resolveHighConfidenceSkill(question);
    }
    if (!hit || hit->skill.empty()) {
        return false;  // low confidence or tier unavailable → full planning
    }
    if (hasMultiIntentSignals(question)) {
        return false;  // conservative veto → full planning
    }

    // Only the plan is constructed here; no stream event is emitted.
    plan.original_query = question;
    plan.is_single_agent = true;
    plan.single_agent_skill = hit->skill;
    LOG_INFO("P10 single-intent fast path: skill=" + hit->skill +
             " confidence=" + std::to_string(hit->confidence) +
             ", planning LLM skipped");
    return true;
}

bool MultiAgentHandler::executeSingleAgentSync(
    const orchestrator::ExecutionPlan& plan,
    const agent_communication::AIQueryRequest& request,
    agent_communication::AIQueryResponse* response) {
    std::string agent_url;
    if (!plan.single_agent_id.empty()) {
        auto agent = agent_router_->getAgent(plan.single_agent_id);
        if (agent.has_value() && agent->is_healthy) {
            agent_url = agent->url;
        }
    }
    // P24 C0: uniform in-flight registration. The blocking sync call has no
    // concurrent observer on this path (no mid-call trigger today — that is
    // the honest C2 boundary), but the abort flag is installed on the HTTP
    // client so any future cancellation entry point can use it.
    const std::string target_url = !agent_url.empty()
        ? agent_url : a2a_adapter_->getConfig().orchestrator_url;
    InFlightRegistration in_flight(target_url, request.request_id());
    if (!agent_url.empty()) {
        return a2a_adapter_->processQueryDirect(request, response, agent_url,
                                                in_flight.flag());
    }
    return a2a_adapter_->processQuery(request, response, in_flight.flag());
}

// agent_invocations producer for the orchestrator path. The owner is read
// from the thread-local auth context (this runs synchronously on the RPC
// serving thread), never from the request body. Write failures are logged
// and swallowed: invocation facts are observability data, not the source
// of truth for the query outcome.
void MultiAgentHandler::recordInvocationFact(
    const std::string& query_log_id, const std::string& agent_id,
    const std::string& skill_name, const std::string& status,
    std::int64_t latency_ms) {
    if (!invocation_repository_) {
        return;
    }
    try {
        common::AgentInvocationRecord record;
        record.id = "invocation-" + QueryHelpers::generateRequestId();
        record.owner_id = AuthInterceptor::currentUserId();
        record.query_log_id = query_log_id;
        record.agent_id = agent_id.empty() ? "default" : agent_id;
        record.skill_name = skill_name;
        record.status = status;
        record.latency_ms = latency_ms;
        if (!invocation_repository_->recordInvocation(record)) {
            LOG_WARN("agent_invocations write skipped for query " + query_log_id);
        }
    } catch (const std::exception& error) {
        LOG_WARN(std::string("agent_invocations write failed for query ") +
                 query_log_id + ": " + error.what());
    } catch (...) {
        LOG_WARN("agent_invocations write failed for query " + query_log_id);
    }
}

bool MultiAgentHandler::initializeOrchestrator(
    const std::string& api_key,
    const std::string& model,
    const std::string& api_url,
    common::RedisClient* redis_client,
    const common::RpcConfig& rpc_config,
    std::unique_ptr<orchestrator::AgentRouter>& out_router,
    std::unique_ptr<orchestrator::TaskPlanner>& out_planner,
    std::unique_ptr<orchestrator::TaskExecutor>& out_executor,
    std::unique_ptr<orchestrator::ResultAggregator>& out_aggregator) {

    try {
        // AgentRouter: skill-based routing
        out_router = std::make_unique<orchestrator::AgentRouter>();
        out_router->initialize(orchestrator::RoutingStrategy::SKILL_MATCH);

        // Wire LLM client into AgentRouter for Tier 0 intent classification
        auto router_llm = std::make_unique<LLMClient>(api_key, model, api_url);
        out_router->setLLMClient(std::move(router_llm));

        // Wire Redis client for feedback-driven routing
        if (redis_client) {
            out_router->setRedisClient(redis_client);
        }

        // TaskPlanner: decides single vs multi-agent, decomposes into DAG
        orchestrator::TaskPlannerConfig planner_config;
        planner_config.api_key = api_key;
        planner_config.model = model;
        planner_config.api_url = api_url;
        out_planner = std::make_unique<orchestrator::TaskPlanner>(planner_config);

        // TaskExecutor: DAG execution engine
        orchestrator::ExecutorConfig exec_config;
        exec_config.subtask_timeout_seconds = rpc_config.timeout_seconds;
        exec_config.global_timeout_seconds = rpc_config.timeout_seconds * 2;
        out_executor = std::make_unique<orchestrator::TaskExecutor>(*out_router, exec_config);

        // ResultAggregator: merges subtask results
        orchestrator::AggregatorConfig agg_config;
        agg_config.api_key = api_key;
        agg_config.model = model;
        agg_config.api_url = api_url;
        agg_config.default_strategy = "llm_synthesize";
        out_aggregator = std::make_unique<orchestrator::ResultAggregator>(agg_config);

        return true;

    } catch (const std::exception& e) {
        LOG_ERROR(std::string("Orchestrator init failed: ") + e.what());
        return false;
    }
}

grpc::Status MultiAgentHandler::handleQuery(
    grpc::ServerContext* context,
    const agent_communication::AIQueryRequest* request,
    agent_communication::AIQueryResponse* response,
    const std::string& request_id) {

    // P24 C2: request-level cancellation token — the planner and the
    // aggregator read it so a cancelled request's LLM work self-aborts.
    auto cancel_token = InFlightAbortRegistry::instance().tokenFor(request_id);

    // Propagate gRPC deadline to A2A call timeouts
    auto gpr_deadline = context->deadline();
    int effective_timeout_seconds = rpc_config_->timeout_seconds;
    if (gpr_deadline != std::chrono::system_clock::time_point::max()) {
        auto now = std::chrono::system_clock::now();
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            gpr_deadline - now);
        if (remaining.count() > 0 && remaining.count() < effective_timeout_seconds) {
            effective_timeout_seconds = static_cast<int>(remaining.count());
        }
    }

    auto start_time = std::chrono::steady_clock::now();
    std::string question = request->question();
    const int llm_timeout_seconds =
        planningTimeoutFor(context, effective_timeout_seconds);

    // Step 1: Plan — decide single vs multi-agent. The P10 fast path may
    // skip the planning LLM entirely for high-confidence single intents;
    // A3: a nearly-exhausted deadline budget skips the doomed 20s planning
    // call and falls straight to the single-agent baseline.
    orchestrator::ExecutionPlan plan;
    bool fast_path_active = tryBuildFastPathPlan(question, plan);
    if (!fast_path_active) {
        if (llm_timeout_seconds <= 0) {
            LOG_WARN("Remaining deadline budget too small to plan, falling back to single agent: " + request_id);
            plan.is_single_agent = true;
        } else {
            try {
                plan = task_planner_->plan(
                    question, prunedSkillsForPlanning(agent_router_, question, agent_router_->getAllSkillDescriptions()),
                    llm_timeout_seconds, cancel_token.get());
            } catch (const std::exception& e) {
                LOG_ERROR("Planning failed for sync query: " + request_id + " - " + e.what());
                plan.is_single_agent = true;
            }
        }
    }

    // Pre-resolve agents for all subtasks
    task_planner_->resolveAgents(plan, *agent_router_);

    // B1: plan-only runs NEVER execute — including single-agent plans and
    // fast-path hits (P1-3). Deliver the plan JSON as the answer payload;
    // the top level finalizes the run as "planned" and execution happens via
    // a follow-up ExecutePlan call after user confirmation.
    if (request->plan_only()) {
        nlohmann::json plan_only_json;
        plan_only_json["original_query"] = plan.original_query;
        plan_only_json["single_agent"] = plan.is_single_agent;
        plan_only_json["tasks"] = nlohmann::json::array();
        if (plan.is_single_agent) {
            nlohmann::json tj;
            tj["id"] = "t1";
            tj["description"] = plan.original_query;
            tj["skill"] = plan.single_agent_skill;
            tj["depends_on"] = nlohmann::json::array();
            tj["agent_id"] = plan.single_agent_id;
            tj["agent_name"] = plan.single_agent_name;
            plan_only_json["tasks"].push_back(std::move(tj));
        } else {
            for (const auto& t : plan.tasks) {
                nlohmann::json tj;
                tj["id"] = t.id;
                tj["description"] = t.description;
                tj["skill"] = t.required_skill;
                tj["depends_on"] = t.depends_on;
                tj["agent_id"] = t.preferred_agent_id;
                tj["agent_name"] = t.preferred_agent_name;
                plan_only_json["tasks"].push_back(std::move(tj));
            }
        }
        response->set_answer(plan_only_json.dump());
        response->set_request_id(request_id);
        response->set_task_id(request_id);
        auto* st = response->mutable_status();
        st->set_code(0);
        st->set_message("plan-only");
        update_status_(request_id, "planned", "", "", "");
        return grpc::Status::OK;
    }

    // Single-agent fast path. Metrics/invocation facts are recorded ONLY at
    // the final terminal state: a failed fast-path attempt is an internal
    // probe that gets retried below, and recording it would double-count
    // the request (two entries for one query).
    if (plan.is_single_agent) {
        bool success = executeSingleAgentSync(plan, *request, response);
        if (success) {
            update_status_(request_id, "completed",
                          plan.single_agent_id, plan.single_agent_name, "");
        }
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time);
        response->set_processing_time_ms(duration.count());
        if (success || !fast_path_active) {
            // Final state: record exactly once.
            record_metrics_("Query", duration.count(), success);
            recordInvocationFact(request_id, plan.single_agent_id,
                                 plan.single_agent_skill,
                                 success ? "success" : "failed", duration.count());
            // Live health metrics for the 30s evaluation loop (P14a).
            if (!plan.single_agent_id.empty()) {
                agent_rpc::registry::ServiceRegistry::recordAgentCall(
                    plan.single_agent_id, success, duration.count());
            }
            return success ? grpc::Status::OK
                           : grpc::Status(grpc::StatusCode::INTERNAL,
                                          QueryHelpers::sanitizeErrorMessage(response->status().message()));
        }

        // P10 fallback: the fast-path execution failed (agent error or
        // unreachable). Retry exactly once through full planning. No metrics
        // were recorded for the probe attempt — the retry result below is
        // the single entry for this request.
        LOG_WARN("P10 single-intent fast path failed, retrying with full planning: " + request_id);
        plan = orchestrator::ExecutionPlan{};
        try {
            plan = task_planner_->plan(question, prunedSkillsForPlanning(agent_router_, question, agent_router_->getAllSkillDescriptions()),
                                       planningTimeoutFor(context, rpc_config_->timeout_seconds),
                                       cancel_token.get());
        } catch (const std::exception& e) {
            LOG_ERROR("Planning failed on fast-path retry: " + request_id + " - " + e.what());
            plan.is_single_agent = true;
        }
        task_planner_->resolveAgents(plan, *agent_router_);
        if (plan.is_single_agent) {
            success = executeSingleAgentSync(plan, *request, response);
            if (success) {
                update_status_(request_id, "completed",
                              plan.single_agent_id, plan.single_agent_name, "");
            }
            end_time = std::chrono::steady_clock::now();
            duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                end_time - start_time);
            response->set_processing_time_ms(duration.count());
            record_metrics_("Query", duration.count(), success);
            recordInvocationFact(request_id, plan.single_agent_id,
                                 plan.single_agent_skill,
                                 success ? "success" : "failed", duration.count());
            // Live health metrics for the 30s evaluation loop (P14a).
            if (!plan.single_agent_id.empty()) {
                agent_rpc::registry::ServiceRegistry::recordAgentCall(
                    plan.single_agent_id, success, duration.count());
            }
            return success ? grpc::Status::OK
                           : grpc::Status(grpc::StatusCode::INTERNAL,
                                          QueryHelpers::sanitizeErrorMessage(response->status().message()));
        }
        // Retry produced a multi-agent plan: fall through below.
    }

    // Multi-agent path
    LOG_INFO("Multi-agent plan: " + std::to_string(plan.tasks.size()) + " subtasks");
    update_status_(request_id, "working", "", "", "");

    auto call_agent = buildCallAgent(request, effective_timeout_seconds);
    // P20: a timed-out subtask aborts its in-flight A2A call (scoped to this
    // request — concurrent requests sharing the agent URL are not touched).
    auto on_cancel = [request_id](const std::string& agent_url) {
        auto& registry = InFlightAbortRegistry::instance();
        // P20/P24: local abort of the in-flight transfer first.
        registry.cancelInFlight(agent_url, request_id);
        // P24 C1②: best-effort protocol-level tasks/cancel so the remote
        // agent can stop work it already started. Detached: the collector
        // thread must not block on the round trip; the fire-and-forget
        // thread is bounded by the 3s HTTP timeout.
        const std::string task_id = registry.inFlightTaskId(agent_url, request_id);
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
    // Client-disconnect propagation: the executor checks this at every layer
    // boundary and stops launching new work when the RPC is cancelled.
    auto cancelled_probe = [context]() { return context->IsCancelled(); };

    try {
        auto results = task_executor_->execute(plan, call_agent, nullptr, on_cancel,
                                               cancelled_probe);
        auto aggregated = result_aggregator_->aggregate(plan, results,
                                    cancel_token.get());

        // One invocation fact per executed subtask (owner from auth context).
        for (const auto& entry : results) {
            const auto& result = entry.second;
            std::string agent_id;
            std::string skill_name;
            for (const auto& task : plan.tasks) {
                if (task.id == entry.first) {
                    agent_id = task.preferred_agent_id;
                    skill_name = task.required_skill;
                    break;
                }
            }
            // Prefer the actually executed agent (routing fallback may pick
            // a different agent than the pre-resolved preference). Results
            // that never reached an agent (fabricated timeout/cancel/resolve
            // failures) are skipped: they carry no signal about the agent's
            // health and would poison the 30s evaluation loop (R4).
            if (!result.executed) {
                continue;
            }
            std::string actual_agent_id =
                result.agent_id.empty() ? agent_id : result.agent_id;
            if (!actual_agent_id.empty()) {
                agent_rpc::registry::ServiceRegistry::recordAgentCall(
                    actual_agent_id, result.success, result.duration_ms);
            }
            recordInvocationFact(request_id, agent_id, skill_name,
                                 result.success ? "success" : "failed",
                                 result.duration_ms);
        }

        response->set_request_id(request_id);
        response->set_task_id(request_id);
        response->set_answer(aggregated.final_answer);
        response->set_context_id(request->context_id());

        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time);
        response->set_processing_time_ms(duration.count());

        auto* status = response->mutable_status();
        status->set_code(0);
        status->set_message("OK");

        update_status_(request_id, "completed", "", "multi-agent", "");
        record_metrics_("Query", duration.count(), true);

        LOG_INFO("Multi-agent query completed in " +
             std::to_string(duration.count()) + "ms (" +
             std::to_string(plan.tasks.size()) + " subtasks)");

        return grpc::Status::OK;

    } catch (const std::exception& e) {
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time);
        LOG_ERROR("Multi-agent query failed: " + request_id + " - " + e.what());
        update_status_(request_id, "failed", "", "", e.what());
        record_metrics_("Query", duration.count(), false);
        recordInvocationFact(request_id, "multi-agent", "", "failed",
                             duration.count());
        return grpc::Status(grpc::StatusCode::INTERNAL,
                           QueryHelpers::sanitizeErrorMessage(
                               std::string("Multi-agent orchestration failed: ") + e.what()));
    }
}

grpc::Status MultiAgentHandler::handleQueryStream(
    grpc::ServerContext* context,
    const agent_communication::AIQueryRequest* request,
    grpc::ServerWriter<agent_communication::AIStreamEvent>* writer,
    const std::string& request_id) {

    // P24 C2: request-level cancellation token (see handleQuery).
    auto cancel_token = InFlightAbortRegistry::instance().tokenFor(request_id);

    tls_stream_result = StreamResultSlot{};
    auto start_time = std::chrono::steady_clock::now();
    std::string question = request->question();
    std::string context_id = request->context_id();

    // Send "thinking" status event
    {
        agent_communication::AIStreamEvent thinking_event;
        thinking_event.set_event_type("status");
        thinking_event.set_content("thinking");
        thinking_event.set_task_state("planning");
        thinking_event.set_context_id(context_id);
        writer->Write(thinking_event);
    }

    // Step 1: Plan. The P10 fast path may skip the planning LLM entirely
    // for high-confidence single intents. A3: the planning call honors the
    // remaining deadline budget and is skipped entirely when too little
    // budget remains (straight to the single-agent baseline).
    orchestrator::ExecutionPlan plan;
    bool fast_path_active = tryBuildFastPathPlan(question, plan);
    if (!fast_path_active) {
        const int llm_timeout_seconds =
            planningTimeoutFor(context, rpc_config_->timeout_seconds);
        if (llm_timeout_seconds <= 0) {
            LOG_WARN("Remaining deadline budget too small to plan, falling back to single agent: " + request_id);
            plan.is_single_agent = true;
        } else {
            try {
                plan = task_planner_->plan(question, prunedSkillsForPlanning(agent_router_, question, agent_router_->getAllSkillDescriptions()),
                                           llm_timeout_seconds, cancel_token.get());
            } catch (const std::exception& e) {
                LOG_ERROR("Planning failed for query: " + request_id + " - " + e.what());
                plan.is_single_agent = true;
            }
        }
    }

    task_planner_->resolveAgents(plan, *agent_router_);

    // B1: plan-only runs NEVER execute — including single-agent plans and
    // fast-path hits (P1-3). Deliver the plan event plus the confirmation
    // marker and stop; execution happens via a follow-up ExecutePlan call
    // after user confirmation.
    if (request->plan_only()) {
        nlohmann::json plan_only_json;
        plan_only_json["original_query"] = plan.original_query;
        plan_only_json["tasks"] = nlohmann::json::array();
        if (plan.is_single_agent) {
            nlohmann::json tj;
            tj["id"] = "t1";
            tj["description"] = plan.original_query;
            tj["skill"] = plan.single_agent_skill;
            tj["depends_on"] = nlohmann::json::array();
            tj["agent_id"] = plan.single_agent_id;
            tj["agent_name"] = plan.single_agent_name;
            plan_only_json["tasks"].push_back(std::move(tj));
        } else {
            for (const auto& t : plan.tasks) {
                nlohmann::json tj;
                tj["id"] = t.id;
                tj["description"] = t.description;
                tj["skill"] = t.required_skill;
                tj["depends_on"] = t.depends_on;
                tj["agent_id"] = t.preferred_agent_id;
                tj["agent_name"] = t.preferred_agent_name;
                plan_only_json["tasks"].push_back(std::move(tj));
            }
        }
        agent_communication::AIStreamEvent plan_event;
        plan_event.set_event_type("plan");
        plan_event.set_content(plan_only_json.dump());
        plan_event.set_context_id(context_id);
        writer->Write(plan_event);
        agent_communication::AIStreamEvent confirm_event;
        confirm_event.set_event_type("status");
        confirm_event.set_content("awaiting_confirmation");
        confirm_event.set_context_id(context_id);
        writer->Write(confirm_event);
        update_status_(request_id, "planned", "", "", "");
        return grpc::Status::OK;
    }

    // Single-agent fast path. No terminal event is emitted anywhere in this
    // branch; AIQueryServiceImpl owns the single terminal emission.
    if (plan.is_single_agent) {
        auto outcome = executeSingleAgentStream(plan, context, request, writer,
                                                request_id, start_time,
                                                fast_path_active);
        if (outcome == SingleStreamOutcome::Success) {
            return grpc::Status::OK;
        }
        if (outcome == SingleStreamOutcome::Cancelled) {
            return grpc::Status(grpc::StatusCode::CANCELLED, "Request cancelled");
        }
        // Retry is only safe when NOTHING reached the client: the adapter
        // may have delivered status events before reporting an error, and
        // replaying those would duplicate events and re-run the agent call
        // (repeated downstream side effects).
        if (!fast_path_active || tls_stream_result.any_written) {
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                QueryHelpers::sanitizeErrorMessage(tls_stream_result.error));
        }

        // P10 fallback: the fast-path execution failed BEFORE any event
        // reached the client, so retrying is safe. Retry exactly once
        // through full planning. The probe attempt recorded no metrics —
        // the retry below is the single entry for this request.
        LOG_WARN("P10 single-intent fast path failed, retrying with full planning: " + request_id);
        tls_stream_result = StreamResultSlot{};
        plan = orchestrator::ExecutionPlan{};
        try {
            plan = task_planner_->plan(question, prunedSkillsForPlanning(agent_router_, question, agent_router_->getAllSkillDescriptions()),
                                       planningTimeoutFor(context, rpc_config_->timeout_seconds),
                                       cancel_token.get());
        } catch (const std::exception& e) {
            LOG_ERROR("Planning failed on fast-path retry: " + request_id + " - " + e.what());
            plan.is_single_agent = true;
        }
        task_planner_->resolveAgents(plan, *agent_router_);
        if (plan.is_single_agent) {
            outcome = executeSingleAgentStream(plan, context, request, writer,
                                               request_id, start_time);
            if (outcome == SingleStreamOutcome::Success) {
                return grpc::Status::OK;
            }
            if (outcome == SingleStreamOutcome::Cancelled) {
                return grpc::Status(grpc::StatusCode::CANCELLED, "Request cancelled");
            }
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                QueryHelpers::sanitizeErrorMessage(tls_stream_result.error));
        }
        // Retry produced a multi-agent plan: fall through below.
    }

    // Emit plan event
    nlohmann::json plan_json;
    plan_json["original_query"] = plan.original_query;
    plan_json["tasks"] = nlohmann::json::array();
    for (const auto& t : plan.tasks) {
        nlohmann::json tj;
        tj["id"] = t.id;
        tj["description"] = t.description;
        tj["skill"] = t.required_skill;
        tj["depends_on"] = t.depends_on;
        if (!t.preferred_agent_id.empty()) {
            tj["agent_id"] = t.preferred_agent_id;
            tj["agent_name"] = t.preferred_agent_name;
        }
        // P12(a): candidate provenance — confidence is a real cosine
        // similarity only when confidence_source == "embedding"; "ranking"
        // values are placeholders and must be labelled as such in the UI.
        tj["candidates"] = nlohmann::json::array();
        for (const auto& cand : t.candidate_agents) {
            nlohmann::json cj;
            cj["agent_id"] = cand.agent_id;
            cj["agent_name"] = cand.agent_name;
            cj["confidence"] = cand.confidence;
            cj["confidence_source"] = cand.confidence_source;
            tj["candidates"].push_back(std::move(cj));
        }
        plan_json["tasks"].push_back(tj);
    }

    agent_communication::AIStreamEvent plan_event;
    plan_event.set_event_type("plan");
    plan_event.set_content(plan_json.dump());
    plan_event.set_context_id(context_id);
    writer->Write(plan_event);

    update_status_(request_id, "working", "", "", "");

    // Propagate gRPC deadline to A2A call timeouts (same contraction logic
    // as the sync path; the gateway now sets a real deadline from the
    // request's timeout_seconds, so this activates for streaming too).
    auto gpr_deadline = context->deadline();
    int effective_timeout_seconds = rpc_config_->timeout_seconds;
    if (gpr_deadline != std::chrono::system_clock::time_point::max()) {
        auto now = std::chrono::system_clock::now();
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            gpr_deadline - now);
        if (remaining.count() > 0 && remaining.count() < effective_timeout_seconds) {
            effective_timeout_seconds = static_cast<int>(remaining.count());
        }
    }

    auto call_agent = buildCallAgent(request, effective_timeout_seconds);
    // P20: a timed-out subtask aborts its in-flight A2A call (scoped to this
    // request — concurrent requests sharing the agent URL are not touched).
    auto on_cancel = [request_id](const std::string& agent_url) {
        auto& registry = InFlightAbortRegistry::instance();
        // P20/P24: local abort of the in-flight transfer first.
        registry.cancelInFlight(agent_url, request_id);
        // P24 C1②: best-effort protocol-level tasks/cancel so the remote
        // agent can stop work it already started. Detached: the collector
        // thread must not block on the round trip; the fire-and-forget
        // thread is bounded by the 3s HTTP timeout.
        const std::string task_id = registry.inFlightTaskId(agent_url, request_id);
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
    // Client-disconnect propagation into the DAG (R3): the executor stops
    // launching new layers once the RPC context is cancelled.
    auto cancelled_probe = [context]() { return context->IsCancelled(); };

    try {
        orchestrator::ProgressCallback progress_cb =
            [writer, &context_id](const orchestrator::SubTaskEvent& event) {
                agent_communication::AIStreamEvent stream_event;
                stream_event.set_context_id(context_id);
                if (event.type == orchestrator::SubTaskEventType::START) {
                    stream_event.set_event_type("subtask_start");
                    stream_event.set_task_state(event.subtask_id);
                    stream_event.set_content(event.detail);
                } else if (event.type == orchestrator::SubTaskEventType::COMPLETE) {
                    stream_event.set_event_type("subtask_complete");
                    stream_event.set_task_state(event.subtask_id);
                    stream_event.set_content(event.detail);
                } else if (event.type == orchestrator::SubTaskEventType::FAILED) {
                    stream_event.set_event_type("subtask_complete");
                    stream_event.set_task_state(event.subtask_id);
                    stream_event.set_content("FAILED: " + event.detail);
                }
                writer->Write(stream_event);
            };

        auto results = task_executor_->execute(plan, call_agent, progress_cb,
                                               on_cancel, cancelled_probe);
        auto aggregated = result_aggregator_->aggregate(plan, results,
                                    cancel_token.get());

        // P13(d)/A2: surface non-fatal warnings (dropped subtasks) as status
        // events — the final_answer itself also carries the notice.
        for (const auto& warning : aggregated.warnings) {
            agent_communication::AIStreamEvent warn_event;
            warn_event.set_event_type("status");
            warn_event.set_content(warning);
            warn_event.set_context_id(context_id);
            writer->Write(warn_event);
        }

        // Client disconnected mid-DAG: surface CANCELLED instead of a
        // completed (and billed) run — same contract as the sync path.
        // P24 C2: mark the request-level token (write_cb already wrote it
        // when events flowed; this covers the no-event window).
        if (context->IsCancelled()) {
            auto duration_cancel = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time);
            InFlightAbortRegistry::instance().cancelInFlightByRequest(request_id);
            update_status_(request_id, "cancelled", "", "", "");
            record_metrics_("QueryStream", duration_cancel.count(), false);
            tls_stream_result.answer = "";
            return grpc::Status(grpc::StatusCode::CANCELLED, "Request cancelled");
        }

        // One invocation fact per executed subtask (owner from auth context).
        for (const auto& entry : results) {
            const auto& result = entry.second;
            std::string agent_id;
            std::string skill_name;
            for (const auto& task : plan.tasks) {
                if (task.id == entry.first) {
                    agent_id = task.preferred_agent_id;
                    skill_name = task.required_skill;
                    break;
                }
            }
            // R4: skip fabricated (never-executed) results — no health
            // signal, no invocation fact.
            if (!result.executed) {
                continue;
            }
            // Prefer the actually executed agent (routing fallback may pick
            // a different agent than the pre-resolved preference).
            std::string actual_agent_id =
                result.agent_id.empty() ? agent_id : result.agent_id;
            if (!actual_agent_id.empty()) {
                agent_rpc::registry::ServiceRegistry::recordAgentCall(
                    actual_agent_id, result.success, result.duration_ms);
            }
            recordInvocationFact(request_id, agent_id, skill_name,
                                 result.success ? "success" : "failed",
                                 result.duration_ms);
        }

        // Hand the acting agent back to the service layer: the LAST
        // executed subtask's agent is the one the conversation ends with,
        // so the streaming agent-switch pipeline (last_agent + cross-agent
        // summary) sees the real agent identity. plan.tasks keeps the DAG
        // order, so iterating it yields the execution order.
        for (const auto& task : plan.tasks) {
            if (results.count(task.id) != 0) {
                tls_stream_result.agent_id = task.preferred_agent_id;
                tls_stream_result.agent_name = task.preferred_agent_name;
            }
        }

        agent_communication::AIStreamEvent answer_event;
        answer_event.set_event_type("partial");
        answer_event.set_content(aggregated.final_answer);
        answer_event.set_context_id(context_id);
        writer->Write(answer_event);

        // The accumulated answer is handed to the service layer, which emits
        // the single terminal event after this call returns.
        tls_stream_result.answer = aggregated.final_answer;

        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time);
        update_status_(request_id, "completed", "", "multi-agent", "");
        record_metrics_("QueryStream", duration.count(), true);

        LOG_INFO("Multi-agent stream completed in " +
                 std::to_string(duration.count()) + "ms (" +
                 std::to_string(plan.tasks.size()) + " subtasks)");

        return grpc::Status::OK;

    } catch (const std::exception& e) {
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time);
        LOG_ERROR("Multi-agent stream failed: " + request_id + " - " + e.what());
        update_status_(request_id, "failed", "", "", e.what());
        record_metrics_("QueryStream", duration.count(), false);
        recordInvocationFact(request_id, "multi-agent", "", "failed",
                             duration.count());

        // Hand the failure to the service layer; it owns the terminal event.
        tls_stream_result.error =
            std::string("Multi-agent orchestration failed: ") + e.what();

        return grpc::Status(grpc::StatusCode::INTERNAL,
                           std::string("Multi-agent orchestration failed: ") + e.what());
    }
}

MultiAgentHandler::SingleStreamOutcome MultiAgentHandler::executeSingleAgentStream(
    const orchestrator::ExecutionPlan& plan,
    grpc::ServerContext* context,
    const agent_communication::AIQueryRequest* request,
    grpc::ServerWriter<agent_communication::AIStreamEvent>* writer,
    const std::string& request_id,
    std::chrono::steady_clock::time_point start_time,
    bool fast_path_probe) {
    // The relay below filters terminal events coming from the A2A adapter,
    // records partial content, and checks both context->IsCancelled() and
    // the writer result. No terminal event is emitted here; the top-level
    // AIQueryServiceImpl owns the single terminal emission.
    //
    // fast_path_probe marks the FIRST attempt of a fast-path run: its
    // failure may be retried once by the caller, so no metrics/invocation
    // facts are recorded for it (recording both attempts would double-count
    // one request). A cancellation is always terminal and is recorded.
    bool cancelled = false;
    bool write_failed = false;
    std::string lower_error;

    auto write_cb = [context, writer, &cancelled, &write_failed,
                     &lower_error, request_id](const agent_communication::AIStreamEvent& event) {
        if (event.event_type() == "complete") {
            return;  // filtered: terminal belongs to the service layer
        }
        if (event.event_type() == "error") {
            if (lower_error.empty()) {
                lower_error = event.content().empty()
                    ? "Agent reported an error" : event.content();
            }
            return;  // filtered: terminal belongs to the service layer
        }
        if (context->IsCancelled()) {
            cancelled = true;
            // P24 C0: client disconnected — abort the SSE transfer itself
            // instead of only stopping event consumption.
            InFlightAbortRegistry::instance().cancelInFlightByRequest(request_id);
            return;
        }
        if (event.event_type() == "partial") {
            tls_stream_result.answer += event.content();
        }
        if (!writer->Write(event)) {
            write_failed = true;
            // P24 C0: the response stream is gone — abort the SSE transfer.
            InFlightAbortRegistry::instance().cancelInFlightByRequest(request_id);
        } else {
            // Any delivered event (status included) makes the stream
            // irreversible: the caller must not replay the run.
            tls_stream_result.any_written = true;
        }
    };

    std::string agent_url;
    if (!plan.single_agent_id.empty()) {
        auto agent = agent_router_->getAgent(plan.single_agent_id);
        if (agent.has_value() && agent->is_healthy) {
            agent_url = agent->url;
        }
    }

    // P24 C0: register the in-flight call so cancellation detected inside
    // write_cb aborts the SSE transfer via the shared registry.
    const std::string target_url = !agent_url.empty()
        ? agent_url : a2a_adapter_->getConfig().orchestrator_url;
    InFlightRegistration in_flight(target_url, request_id);

    try {
        if (!agent_url.empty()) {
            LOG_INFO("Single-agent stream: routing to " + plan.single_agent_skill +
                     " via " + agent_url);
            a2a_adapter_->processQueryStreamingDirect(*request, write_cb,
                                                      agent_url, in_flight.flag());
        } else {
            LOG_INFO("Single-agent stream: no pre-resolved agent, using adapter routing");
            a2a_adapter_->processQueryStreaming(*request, write_cb, in_flight.flag());
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Single-agent streaming failed: " + request_id + " - " + e.what());
        tls_stream_result.error = std::string("Agent communication failed: ") + e.what();

        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time);
        // P24 C1①: a transport exception raised by our own disconnect
        // abort is a cancellation, not an agent failure — reporting it as
        // "failed" would pollute health metrics and mislead the client.
        if (cancelled || context->IsCancelled()) {
            update_status_(request_id, "cancelled", "", "", "");
            recordInvocationFact(request_id, plan.single_agent_id,
                                 plan.single_agent_skill, "cancelled",
                                 duration.count());
            return SingleStreamOutcome::Cancelled;
        }
        // Probe failures may be retried by the caller — do not publish a
        // "failed" task status that the retry would flip back to "completed".
        if (!fast_path_probe) {
            update_status_(request_id, "failed", "", "", e.what());
        }
        if (!fast_path_probe) {
            record_metrics_("QueryStream", duration.count(), false);
            recordInvocationFact(request_id, plan.single_agent_id,
                                 plan.single_agent_skill, "failed",
                                 duration.count());
            if (!plan.single_agent_id.empty()) {
                agent_rpc::registry::ServiceRegistry::recordAgentCall(
                    plan.single_agent_id, false, duration.count());
            }
        }
        return SingleStreamOutcome::Failed;
    }

    if (cancelled) {
        update_status_(request_id, "cancelled", "", "", "");
        recordInvocationFact(request_id, plan.single_agent_id,
                             plan.single_agent_skill, "cancelled",
                             std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start_time).count());
        return SingleStreamOutcome::Cancelled;
    }
    if (!lower_error.empty()) {
        tls_stream_result.error = lower_error;
        if (!fast_path_probe) {
            update_status_(request_id, "failed", "", "", lower_error);
            auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            recordInvocationFact(request_id, plan.single_agent_id,
                                 plan.single_agent_skill, "failed", duration_ms);
            if (!plan.single_agent_id.empty()) {
                agent_rpc::registry::ServiceRegistry::recordAgentCall(
                    plan.single_agent_id, false, duration_ms);
            }
        }
        return SingleStreamOutcome::Failed;
    }
    if (write_failed) {
        tls_stream_result.error = "Failed to write stream event";
        if (!fast_path_probe) {
            update_status_(request_id, "failed", "", "", "Failed to write stream event");
            auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            recordInvocationFact(request_id, plan.single_agent_id,
                                 plan.single_agent_skill, "failed", duration_ms);
            if (!plan.single_agent_id.empty()) {
                agent_rpc::registry::ServiceRegistry::recordAgentCall(
                    plan.single_agent_id, false, duration_ms);
            }
        }
        return SingleStreamOutcome::Failed;
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);
    update_status_(request_id, "completed",
                  plan.single_agent_id, plan.single_agent_name, "");
    record_metrics_("QueryStream", duration.count(), true);
    recordInvocationFact(request_id, plan.single_agent_id,
                         plan.single_agent_skill, "success",
                         duration.count());
    if (!plan.single_agent_id.empty()) {
        agent_rpc::registry::ServiceRegistry::recordAgentCall(
            plan.single_agent_id, true, duration.count());
    }
    // Hand the acting agent back so the service layer can run the
    // agent-switch memory pipeline on the streaming path.
    tls_stream_result.agent_id = plan.single_agent_id;
    tls_stream_result.agent_name = plan.single_agent_name;
    return SingleStreamOutcome::Success;
}

std::function<std::string(const std::string&, const std::string&)>
MultiAgentHandler::buildCallAgent(const agent_communication::AIQueryRequest* request,
                                  int effective_timeout_seconds) {
    std::string memory_ctx = QueryHelpers::buildMemoryContext(request);
    const std::string owner_request_id = request ? request->request_id() : "";

    return [this, memory_ctx, effective_timeout_seconds, owner_request_id](
               const std::string& agent_url,
               const std::string& prompt) -> std::string {
        std::string enriched_prompt = prompt;
        if (!memory_ctx.empty()) {
            enriched_prompt = memory_ctx + "\n" + prompt;
        }

        // P21 L1: the DAG delegation path was the naked path — validate the
        // agent URL with the same real-parse checks as the direct paths.
        std::string url_err;
        if (!agent_rpc::a2a_adapter::validateAgentUrl(agent_url, url_err)) {
            throw std::runtime_error("Agent URL rejected: " + url_err);
        }

        // P20/P24: register an abort flag for this in-flight call so a
        // timed-out subtask (or a client disconnect) can interrupt the
        // blocking HTTP transfer. RAII erases it on every exit path
        // (success, exception).
        InFlightRegistration in_flight(agent_url, owner_request_id);

        a2a::A2AClient client(agent_url);
        client.set_abort_flag(in_flight.flag().get());
        // P20: the remaining request budget tightens the HTTP timeout
        // instead of the fixed rpc_config value.
        client.set_timeout(std::max(1L, static_cast<long>(std::min(
            effective_timeout_seconds, rpc_config_->timeout_seconds))));

        // P21 L2 (strict mode): resolve the host, reject blacklisted
        // addresses, and pin the validated IPs to the connection so the
        // resolution and the connect share one result (anti-rebinding).
        std::string host;
        std::string port_str;
        if (agent_rpc::a2a_adapter::ssrfStrictModeEnabled() &&
            agent_rpc::a2a_adapter::splitAgentUrlHostPort(
                agent_url, host, port_str)) {
            std::vector<std::string> ips;
            std::string host_err;
            if (!agent_rpc::a2a_adapter::validateResolvedHost(
                    host, ips, host_err)) {
                throw std::runtime_error("Agent host rejected: " + host_err);
            }
            // Scheme detection must match L1's case-insensitive parse:
            // any case of https pins port 443.
            const bool https =
                std::equal(agent_url.begin(),
                           agent_url.begin() + std::min<size_t>(8, agent_url.size()),
                           "https://", [](char a, char b) {
                               return std::tolower(static_cast<unsigned char>(a)) == b;
                           });
            const std::string pin_port =
                port_str.empty() ? (https ? "443" : "80") : port_str;
            std::vector<std::string> resolve_entries;
            resolve_entries.reserve(ips.size());
            for (const auto& ip : ips) {
                resolve_entries.push_back(host + ":" + pin_port + ":" + ip);
            }
            client.set_resolve_entries(resolve_entries);
        }

        a2a::AgentMessage msg = a2a::AgentMessage::create()
            .with_role(a2a::MessageRole::User)
            .with_text(enriched_prompt);

            auto params = a2a::MessageSendParams::create().with_message(msg);
            auto a2a_response = client.send_message(params);
            if (a2a_response.is_task()) {
                // P24 C1②: remember the task id on the live in-flight entry
                // so a later timeout/disconnect can send a best-effort
                // protocol-level tasks/cancel (entry cleanup is RAII).
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
}

} // namespace server
} // namespace agent_rpc
