/**
 * @file ai_query_service.cpp
 * @brief AI Query Service — core query methods (Query, QueryStream, GetQueryStatus, GetAgentMetrics)
 *
 * Requirements: 2.1, 2.2, 2.5
 *
 * Durable pipeline: every Query/QueryStream executes the same six
 * ordered steps:
 *   1. Resolve the owner exclusively from AuthInterceptor::currentUserId();
 *      the request body user_id is ignored. No rows are created without auth.
 *   2. Resolve request_id/context_id and ensure the owner's conversation.
 *   3. Create query_logs + traces rows with status "running" (JSON-bound
 *      route/plan parameters; user text is never concatenated into SQL).
 *   4. Reserve the estimated token budget in PostgreSQL. Rejections persist
 *      the "rejected" terminal state before RESOURCE_EXHAUSTED is returned.
 *   5. Assemble SystemContext from PostgreSQL (messages + memory summary);
 *      Redis MemoryService is only an acceleration cache.
 *   6. finalizeDurableQuery() persists the terminal state exactly once per
 *      run (messages, query log, trace, token_usage_ledger estimate).
 *
 * Delegated modules:
 *   - orchestration_service_impl.cpp  — ExecutePlan, ReplayQuery, ExportConversation
 *   - multi_agent_handler.cpp         — multi-agent sync/stream query paths
 *   - query_helpers.cpp               — task status, metrics, agent-switch, UUID
 */

#include "agent_rpc/server/ai_query_service.h"
#include "agent_rpc/server/auth_interceptor.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/env_loader.h"
#include "agent_rpc/a2a_adapter/error_mapper.h"
#include "agent_rpc/common/trace_context.h"
#include "agent_rpc/orchestrator/export_service.h"
#include "agent_rpc/orchestrator/replay_service.h"
#include "agent_rpc/common/cost_tracker.h"
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>

namespace agent_rpc {
namespace server {

AIQueryServiceImpl::AIQueryServiceImpl()
    : a2a_adapter_(std::make_unique<a2a_adapter::A2AAdapter>()) {
}

AIQueryServiceImpl::~AIQueryServiceImpl() {
    shutdown();
}

bool AIQueryServiceImpl::initialize(
    const common::RpcConfig& rpc_config,
    const a2a_adapter::A2AConfig& a2a_config,
    common::RedisClient* redis,
    common::PostgresStore& store,
    common::QueryDomainRepository& domain,
    common::PostgresBudgetRepository& budget) {

    if (initialized_) {
        return true;
    }

    rpc_config_ = rpc_config;
    redis_client_ = redis;
    store_ = &store;
    domain_repo_ = &domain;
    budget_repo_ = &budget;
    budget_limits_ = budgetLimitsFromEnvironment();

    // Initialize MemoryService (Redis-backed cache only; PostgreSQL is the
    // source of truth for conversation context).
    memory_service_ = std::make_unique<common::MemoryService>(
        std::shared_ptr<common::RedisClient>(redis, [](common::RedisClient*){}));

    // Initialize A2A adapter
    if (!a2a_adapter_->initialize(a2a_config)) {
        LOG_ERROR("Failed to initialize A2A adapter");
        return false;
    }

    // Wire Redis client to adapter (required for activity feed and autonomy headers)
    if (redis) {
        a2a_adapter_->setRedisClient(
            std::shared_ptr<common::RedisClient>(redis, [](common::RedisClient*){}));
    }

    // Circuit breaker for the A2A backend
    circuit_breaker_ = common::CircuitBreakerManager::getInstance()
        .getCircuitBreaker("a2a_backend");

    // Initialize multi-agent orchestrator if LLM_API_KEY is set
    const char* api_key_env = std::getenv("LLM_API_KEY");
    if (api_key_env && api_key_env[0] != '\0') {
        std::string api_key(api_key_env);
        std::string model = common::envOrDefault("LLM_MODEL", "deepseek-v4-pro");
        std::string api_url = common::envOrDefault("LLM_API_URL", "https://api.deepseek.com/v1/chat/completions");

        memory_llm_client_ = std::make_unique<LLMClient>(api_key, model, api_url);

        if (MultiAgentHandler::initializeOrchestrator(
                api_key, model, api_url, redis, rpc_config_,
                agent_router_, task_planner_, task_executor_, result_aggregator_)) {

            orchestrator_enabled_ = true;

            multi_agent_handler_ = std::make_unique<MultiAgentHandler>(
                task_planner_.get(), agent_router_.get(),
                task_executor_.get(), result_aggregator_.get(),
                a2a_adapter_.get(), &rpc_config_);
            multi_agent_handler_->setCallbacks(
                [this](const std::string& tid, const std::string& st,
                       const std::string& aid, const std::string& an, const std::string& err) {
                    helpers_.updateTaskStatus(tid, st, aid, an, err);
                },
                [](const std::string& m, int64_t d, bool s) {
                    QueryHelpers::recordMetrics(m, d, s);
                });

            orchestration_impl_ = std::make_unique<OrchestrationServiceImpl>(
                task_planner_.get(), task_executor_.get(),
                agent_router_.get(), memory_service_.get(), &rpc_config_);

            LOG_INFO("Multi-agent orchestrator enabled (LLM: " + model + ")");

            // P7: optional embedding routing tier (NEXUSAI_EMBEDDING_ROUTER,
            // default off). Any failure degrades silently — the router keeps
            // the 3-tier pipeline (Embedding(high) -> LLM -> Keyword) and
            // startup is never blocked or failed by this wiring.
            if (agent_router_ &&
                common::envOrDefault("NEXUSAI_EMBEDDING_ROUTER", "0") == "1") {
                orchestrator::EmbeddingRouterConfig embedding_config;
                embedding_config.enabled = true;
                embedding_config.api_key = api_key;  // LLM_API_KEY fallback
                if (agent_router_->enableEmbedding(embedding_config)) {
                    LOG_INFO("Embedding router enabled (NEXUSAI_EMBEDDING_ROUTER=1)");
                } else {
#ifdef AGENT_RPC_ENABLE_MCP
                    LOG_WARN("embedding router disabled: embedding service unavailable or invalid config, fallback to 3-tier pipeline");
#else
                    LOG_WARN("embedding router disabled: built without AGENT_RPC_ENABLE_MCP, fallback to 3-tier pipeline");
#endif
                }
            }
        } else {
            LOG_WARN("Multi-agent orchestrator initialization failed, falling back to single-agent mode");
        }
    }

    initialized_ = true;
    LOG_INFO("AIQueryService initialized successfully");
    return true;
}

void AIQueryServiceImpl::shutdown() {
    if (!initialized_) {
        return;
    }
    if (a2a_adapter_) {
        a2a_adapter_->shutdown();
    }
    initialized_ = false;
    LOG_INFO("AIQueryService shutdown");
}

bool AIQueryServiceImpl::isAvailable() const {
    return initialized_ && a2a_adapter_ && a2a_adapter_->isAvailable();
}

// Local helper — sanitize CURL errors
static std::string sanitizeErrorMessage(const std::string& msg) {
    return QueryHelpers::sanitizeErrorMessage(msg);
}

// Durable pipeline helpers (steps 2-6)
// Stable deterministic estimate: V012 reservations are estimated tokens, not
// provider-measured usage. The same value is recorded in token_usage_ledger
// with estimated=true so accounting never pretends to be exact.
std::int64_t AIQueryServiceImpl::estimateTokens(const std::string& question) {
    return 64 + static_cast<std::int64_t>(question.size()) / 4;
}

// Estimated-token budget quotas. Defaults (documented here and in
// .env.example):
//   NEXUSAI_BUDGET_GLOBAL_TOKENS        default 0 (unlimited)
//   NEXUSAI_BUDGET_USER_DAILY_TOKENS    default 200000
//   NEXUSAI_BUDGET_USER_MONTHLY_TOKENS  default 4000000
//   NEXUSAI_BUDGET_SESSION_TOKENS       default 100000
// A value of zero leaves the corresponding bucket unlimited.
common::BudgetLimits AIQueryServiceImpl::budgetLimitsFromEnvironment() {
    auto read = [](const char* name, std::int64_t fallback) -> std::int64_t {
        const std::string raw = common::envOrDefault(name, std::to_string(fallback));
        try {
            return std::stoll(raw);
        } catch (...) {
            return fallback;
        }
    };
    common::BudgetLimits limits;
    limits.global = read("NEXUSAI_BUDGET_GLOBAL_TOKENS", 0);
    limits.user_daily = read("NEXUSAI_BUDGET_USER_DAILY_TOKENS", 200000);
    limits.user_monthly = read("NEXUSAI_BUDGET_USER_MONTHLY_TOKENS", 4000000);
    limits.session = read("NEXUSAI_BUDGET_SESSION_TOKENS", 100000);
    return limits;
}

bool AIQueryServiceImpl::beginDurableRows(DurableQueryRun& run, const std::string& route,
                                          const std::string& plan_json) {
    if (!domain_repo_) {
        return false;
    }

    // Step 2: confirm/create the owner's conversation. Cross-owner conflicts
    // and database failures both refuse the request before any query rows.
    const std::string title = run.question.substr(0, std::min<std::size_t>(run.question.size(), 64));
    if (!domain_repo_->ensureConversation(run.owner_id, run.conversation_id, title)) {
        LOG_ERROR("ensureConversation refused for owner " + run.owner_id +
                  " conversation " + run.conversation_id);
        return false;
    }

    // Step 3: running query_log + trace rows. The query_log id equals the
    // request_id so retries and budget reservations share one idempotency key.
    run.trace_row_id = "trace-" + run.request_id;

    nlohmann::json route_json;
    route_json["route"] = route;

    common::QueryLogRecord log;
    log.id = run.request_id;
    log.owner_id = run.owner_id;
    log.conversation_id = run.conversation_id;
    log.request_text = run.question;
    log.route_decision = route_json.dump();
    log.execution_plan = plan_json;
    log.model = run.model;
    log.status = "running";
    if (domain_repo_->createQueryLog(log)) {
        run.first_attempt = true;
    } else if (!domain_repo_->getQueryLogById(run.owner_id, run.request_id).has_value()) {
        LOG_ERROR("Failed to create query log for request " + run.request_id);
        return false;
    }

    nlohmann::json trace_start;
    trace_start["phase"] = "running";
    trace_start["request_id"] = run.request_id;

    common::TraceRecord trace;
    trace.id = run.trace_row_id;
    trace.owner_id = run.owner_id;
    trace.query_log_id = run.request_id;
    trace.trace_payload = trace_start.dump();
    trace.status = "running";
    if (!domain_repo_->createTrace(trace) &&
        !domain_repo_->getTraceById(run.owner_id, run.trace_row_id).has_value()) {
        LOG_ERROR("Failed to create trace for request " + run.request_id);
        return false;
    }
    return true;
}

grpc::Status AIQueryServiceImpl::reserveBudgetOrReject(DurableQueryRun& run) {
    if (!budget_repo_ || !domain_repo_) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Budget repository unavailable");
    }

    // Step 4: estimated-token reservation keyed by request_id (idempotent on
    // same-owner retries; cross-owner reuse is refused).
    // Semantic note: sandbox and compare traffic (context_id
    // with a "sandbox-" or "compare-" prefix) intentionally counts against
    // the same PostgreSQL budget. There is NO exemption branch here: the old
    // Redis micro-dollar sandbox exemption was removed on purpose because the
    // PG budget is the source of truth and the sandbox/compare paths
    // reuse this exact pipeline.
    run.estimated_tokens = estimateTokens(run.question);
    auto result = budget_repo_->reserve(run.owner_id, run.conversation_id,
                                        run.request_id, run.estimated_tokens,
                                        budget_limits_);
    if (result.accepted) {
        return grpc::Status::OK;
    }

    // Persist the rejected terminal state before answering.
    const std::string reason = result.reason.empty() ? "Token budget exhausted" : result.reason;

    common::QueryLogRecord log;
    log.id = run.request_id;
    log.owner_id = run.owner_id;
    log.response_text = reason;
    log.model = run.model;
    log.status = "rejected";
    domain_repo_->updateQueryLog(log);

    nlohmann::json payload;
    payload["status"] = "rejected";
    payload["reason"] = reason;
    payload["estimated_tokens"] = run.estimated_tokens;

    common::TraceRecord trace;
    trace.id = run.trace_row_id;
    trace.owner_id = run.owner_id;
    trace.query_log_id = run.request_id;
    trace.trace_payload = payload.dump();
    trace.status = "rejected";
    domain_repo_->updateTrace(trace);

    // Terminal state already persisted; finalize must not run again.
    run.finalized.store(true);
    helpers_.updateTaskStatus(run.request_id, "failed", "", "", reason);
    LOG_WARN("Budget reservation refused for " + run.owner_id + " request " +
             run.request_id + ": " + reason);
    return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, reason);
}

// Forward declaration of the cache-only guard defined further down: the
// memory-recall step below relies on its silent-degradation contract.
template <typename Fn>
static void runCacheOnly(Fn&& operation, const std::string& what);

void AIQueryServiceImpl::buildSystemContextFromPg(
    const std::string& owner_id, const std::string& conversation_id,
    agent_communication::SystemContext* system_context,
    bool sandbox_request) {
    if (!system_context) {
        return;
    }
    system_context->set_user_id(owner_id);
    if (!domain_repo_) {
        return;
    }
    // Step 5: durable PostgreSQL records are the source of truth; Redis is
    // only an acceleration cache for the memory service.
    auto conversation = domain_repo_->getConversationById(owner_id, conversation_id);
    if (conversation && !conversation->memory_summary.empty()) {
        system_context->set_user_memory(conversation->memory_summary);
    }
    const auto messages = domain_repo_->listMessages(owner_id, conversation_id);
    constexpr std::size_t kMaxHistory = 20;
    const std::size_t start = messages.size() > kMaxHistory ? messages.size() - kMaxHistory : 0;
    std::string history;
    for (std::size_t index = start; index < messages.size(); ++index) {
        history += messages[index].role + ": " + messages[index].content + "\n";
    }
    system_context->set_conversation_history(history);

    // Batch-1 memory loop: after the PostgreSQL context is assembled, recall
    // the Redis-accelerated tiers (Tier-2 long-term memory hints and the
    // cross-agent switch summary). Redis stays cache-only here: the
    // NEXUSAI_MEMORY_HINTS_RECALL switch (default on) and the live connection
    // state gate the reads, and any failure degrades silently.
    //
    // Sandbox requests skip long-term memory reads, symmetric to the
    // write-side !request->sandbox() guard in Query(): the sandbox flag lives
    // on the request object, so the caller forwards it — this function stays
    // request-shape-free by design.
    const bool recall_enabled =
        common::envOrDefault("NEXUSAI_MEMORY_HINTS_RECALL", "1") != "0";
    if (!recall_enabled || sandbox_request || !memory_service_) {
        return;
    }
    // Hot-path protection: short-circuit before touching Redis when the
    // connection is down — the client's lazy reconnect can block for ~1s and
    // must never stall the query path.
    if (!redis_client_ || !redis_client_->isConnected()) {
        return;
    }
    runCacheOnly([&] {
        // Tier-2 long-term memory: merge with a non-empty PG memory_summary
        // instead of overwriting it.
        const std::string recalled = memory_service_->getUserMemory(owner_id);
        if (!recalled.empty()) {
            std::string merged = system_context->user_memory();
            if (!merged.empty()) {
                merged += "\n";
            }
            merged += recalled;
            system_context->set_user_memory(merged);
        }
        // Tier-3 cross-agent summary recall.
        const std::string summary =
            memory_service_->getCrossAgentSummary(conversation_id);
        if (!summary.empty()) {
            system_context->set_cross_agent_summary(summary);
        }
    }, "memory recall");
}

void AIQueryServiceImpl::finalizeDurableQuery(DurableQueryRun& run, const std::string& status,
                                              const std::string& response_text,
                                              const std::string& error_message) {
    // Step 6: exactly one terminal persistence per run, on any path.
    bool expected = false;
    if (!run.finalized.compare_exchange_strong(expected, true)) {
        return;
    }
    if (!domain_repo_) {
        return;
    }

    // Conversation messages are appended at most once per request_id. The
    // message ids are deterministic ("msg-user-" / "msg-assistant-" +
    // request_id) and the repository deduplicates on them, so every run that
    // reaches finalize attempts the write: the FIRST successful run persists
    // the history, later retries are discarded without consuming a sequence
    // number. This keeps "rejected then retried successfully" requests
    // recorded while retries still never duplicate history.
    domain_repo_->appendMessageAutoSequence("msg-user-" + run.request_id, run.owner_id,
                                            run.conversation_id, "user", run.question);
    if (!response_text.empty()) {
        domain_repo_->appendMessageAutoSequence("msg-assistant-" + run.request_id,
                                                run.owner_id, run.conversation_id,
                                                "assistant", response_text);
    }

    // Query log terminal update (pure owner-scoped UPDATE).
    common::QueryLogRecord log;
    log.id = run.request_id;
    log.owner_id = run.owner_id;
    log.response_text = response_text.empty() ? error_message : response_text;
    log.model = run.model;
    log.status = status;
    if (!domain_repo_->updateQueryLog(log)) {
        LOG_WARN("finalize: query log update missed for request " + run.request_id);
    }

    // Trace terminal update with collected spans.
    nlohmann::json payload;
    payload["status"] = status;
    payload["request_id"] = run.request_id;
    payload["estimated_tokens"] = run.estimated_tokens;
    if (!error_message.empty()) {
        payload["error"] = error_message;
    }
    if (auto* trace_ctx = common::TraceContext::current()) {
        nlohmann::json spans = nlohmann::json::array();
        for (const auto& span : trace_ctx->completedSpans()) {
            nlohmann::json entry;
            entry["name"] = span.name;
            entry["component"] = span.component;
            entry["duration_ms"] = span.duration_ms;
            entry["status"] = span.status;
            spans.push_back(entry);
        }
        payload["spans"] = spans;
    }

    common::TraceRecord trace;
    trace.id = run.trace_row_id;
    trace.owner_id = run.owner_id;
    trace.query_log_id = run.request_id;
    trace.trace_payload = payload.dump();
    trace.status = status;
    if (!domain_repo_->updateTrace(trace)) {
        LOG_WARN("finalize: trace update missed for request " + run.request_id);
    }

    // Token ledger: one estimate-only entry per request_id, never per retry.
    // The entry id is the deduplication key: the repository reports the
    // conflict instead of throwing, so retries may call this unconditionally
    // (a request rejected first and retried successfully is billed exactly
    // once). There is no provider settlement yet (estimated=true).
    common::TokenUsageLedgerRecord usage;
    usage.id = "usage-" + run.request_id;
    usage.owner_id = run.owner_id;
    usage.query_log_id = run.request_id;
    usage.model = run.model;
    usage.prompt_tokens = run.estimated_tokens;
    usage.completion_tokens = response_text.empty()
        ? 0
        : static_cast<std::int64_t>(response_text.size()) / 4;
    usage.estimated = true;
    usage.cost_usd = "0";
    if (!domain_repo_->appendTokenUsageLedger(usage)) {
        // A false return here means the usage-<request_id> row
        // already exists — the idempotent duplicate was skipped on purpose,
        // not a missed write.
        LOG_INFO("finalize: token ledger duplicate skipped for request " + run.request_id);
    }
}

void AIQueryServiceImpl::abortDurableRun(DurableQueryRun& run, const std::string& reason) {
    if (run.request_id.empty()) {
        return;
    }
    try {
        finalizeDurableQuery(run, "failed", "",
                             "Pipeline aborted: " + reason);
    } catch (const std::exception& nested) {
        LOG_ERROR(std::string("finalize during pipeline abort failed: ") + nested.what());
    } catch (...) {
        LOG_ERROR("finalize during pipeline abort failed with unknown error");
    }
}

// Cache-only guard: Redis-backed bookkeeping that runs alongside (or after)
// the durable terminal persistence must never flip an already-finalized
// request into INTERNAL/UNAVAILABLE. Failures are logged and swallowed.
template <typename Fn>
static void runCacheOnly(Fn&& operation, const std::string& what) {
    try {
        operation();
    } catch (const std::exception& error) {
        LOG_WARN(std::string("cache-only step failed (") + what + "): " + error.what());
    } catch (...) {
        LOG_WARN(std::string("cache-only step failed (") + what + "): unknown error");
    }
}

// agent_invocations producer (final wrap-up wiring)
void AIQueryServiceImpl::setInvocationRepository(
    common::AgentRuntimeRepository* repository) {
    invocation_repository_ = repository;
    if (multi_agent_handler_) {
        multi_agent_handler_->setInvocationRepository(repository);
    }
}

// Owner-scoped observability fact. agent_invocations is derived metrics
// data, never the source of truth for a query outcome: any write failure is
// logged and swallowed so it cannot flip a successful query into an error.
void AIQueryServiceImpl::recordInvocationFact(
    const std::string& owner_id, const std::string& query_log_id,
    const std::string& agent_id, const std::string& skill_name,
    const std::string& status, std::int64_t latency_ms) {
    if (!invocation_repository_) {
        return;
    }
    try {
        common::AgentInvocationRecord record;
        record.id = "invocation-" + QueryHelpers::generateRequestId();
        record.owner_id = owner_id;
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

// Agent-switch memory pipeline, shared by Query() and QueryStream(). The
// taking-over agent's name/duties come from the registry router when
// available; agent_name is the fallback. Callers wrap this in runCacheOnly,
// and only invoke it for real acting agents on success (never for sandbox
// runs — the sandbox guard lives at the call sites).
void AIQueryServiceImpl::writeAgentSwitchMemory(
    const std::string& owner_id, const std::string& context_id,
    const std::string& agent_id, const std::string& agent_name) {
    std::string target_agent_name = agent_name;
    std::string target_agent_duties;
    if (agent_router_) {
        if (auto target = agent_router_->getAgent(agent_id)) {
            if (target_agent_name.empty()) {
                target_agent_name = target->name;
            }
            target_agent_duties = target->description;
            for (const auto& skill : target->skills) {
                if (!target_agent_duties.empty()) {
                    target_agent_duties += ", ";
                }
                target_agent_duties += skill;
                const auto desc = target->skill_descriptions.find(skill);
                if (desc != target->skill_descriptions.end() && !desc->second.empty()) {
                    target_agent_duties += "(" + desc->second + ")";
                }
            }
        }
    }
    helpers_.handleAgentSwitch(memory_service_.get(), memory_llm_client_.get(),
                               domain_repo_, owner_id, context_id, agent_id,
                               target_agent_name, target_agent_duties);
}

grpc::Status AIQueryServiceImpl::Query(
    grpc::ServerContext* context,
    const agent_communication::AIQueryRequest* request,
    agent_communication::AIQueryResponse* response) {

    if (!isAvailable()) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                           "AI Query Service not available");
    }
    if (!request || !response) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                           "Invalid request or response");
    }

    // Step 1: the owner comes exclusively from the authenticated session.
    // The user_id carried in the request body is always ignored.
    std::string owner_id = AuthInterceptor::currentUserId();
    if (!AuthInterceptor::isAuthenticated() || owner_id.empty()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                           "Valid authentication token required");
    }

    // Crash guard: any PG/Redis fault thrown by the durable pipeline below is
    // mapped to a gRPC error and the already-created rows are finalized as
    // "failed"; nothing may escape into the gRPC handler (std::terminate).
    DurableQueryRun run;
    try {
    auto start_time = std::chrono::steady_clock::now();

    // Step 2: stable identifiers.
    std::string request_id = request->request_id();
    if (request_id.empty()) {
        request_id = QueryHelpers::generateRequestId();
    }
    std::string context_id = request->context_id();
    if (context_id.empty()) {
        context_id = "ctx-" + request_id;
    }

    LOG_INFO("Processing AI query: " + request_id);

    run.owner_id = owner_id;
    run.conversation_id = context_id;
    run.request_id = request_id;
    run.question = request->question();
    run.model = common::envOrDefault("LLM_MODEL", "deepseek-v4-pro");

    const std::string route = orchestrator_enabled_ ? "multi-agent" : "single-agent-a2a";
    nlohmann::json plan_json;
    plan_json["mode"] = "sync";
    plan_json["request_id"] = request_id;
    if (!beginDurableRows(run, route, plan_json.dump())) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                           "Failed to persist query start");
    }

    // Step 4: PostgreSQL budget reservation (rejected rows persisted inside).
    auto budget_status = reserveBudgetOrReject(run);
    if (!budget_status.ok()) {
        return budget_status;
    }

    // Step 5: enrich the request; set_user_id is unconditional.
    agent_communication::AIQueryRequest enriched_req = *request;
    enriched_req.set_user_id(owner_id);
    enriched_req.set_request_id(request_id);
    enriched_req.set_context_id(context_id);
    buildSystemContextFromPg(owner_id, context_id,
                             enriched_req.mutable_system_context(),
                             request->sandbox());

    common::TraceContext::init(owner_id, context_id);

    if (context->IsCancelled()) {
        finalizeDurableQuery(run, "cancelled", "", "Request cancelled");
        helpers_.updateTaskStatus(request_id, "cancelled");
        return grpc::Status(grpc::StatusCode::CANCELLED, "Request cancelled");
    }

    helpers_.updateTaskStatus(request_id, "working");

    bool success = false;
    std::string response_text;
    std::string error_message;

    if (orchestrator_enabled_) {
        // Multi-agent orchestrator path
        auto status = multi_agent_handler_->handleQuery(
            context, &enriched_req, response, request_id);
        success = status.ok();
        response_text = response->answer();
        error_message = status.error_message();
        response->set_request_id(request_id);
        response->set_task_id(request_id);
    } else {
        if (circuit_breaker_ && !circuit_breaker_->isRequestAllowed()) {
            LOG_WARN("A2A backend circuit breaker open, rejecting query: " + request_id);
            auto* status = response->mutable_status();
            status->set_code(-1);
            status->set_message("A2A backend temporarily unavailable (circuit breaker open)");
            finalizeDurableQuery(run, "failed", "", "Circuit breaker open");
            helpers_.updateTaskStatus(request_id, "failed", "", "", "Circuit breaker open");
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "A2A backend circuit breaker open");
        }

        // Propagate gRPC deadline to A2A HTTP timeout
        if (context->deadline() != std::chrono::system_clock::time_point::max()) {
            auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                context->deadline() - std::chrono::system_clock::now());
            long timeout_sec = std::max(1L, static_cast<long>(remaining.count()));
            a2a_adapter_->setRequestTimeout(timeout_sec);
        }

        // Process query via A2A adapter
        // Process query via A2A adapter
        common::TraceContext::current()->startSpan("process_query", "server");
        success = a2a_adapter_->processQuery(enriched_req, response);
        common::TraceContext::current()->endSpan();

        if (circuit_breaker_) {
            if (success) circuit_breaker_->recordSuccess();
            else circuit_breaker_->recordFailure();
        }

        response->set_request_id(request_id);
        response->set_task_id(request_id);
        response_text = response->answer();
        error_message = response->status().message();
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);

    QueryHelpers::recordMetrics("Query", duration.count(), success);

    // agent_invocations producer: the single-agent A2A path records one
    // owner-scoped fact per query here; the multi-agent orchestrator path
    // records per-call facts inside MultiAgentHandler.
    if (!orchestrator_enabled_) {
        recordInvocationFact(run.owner_id, request_id, response->agent_id(),
                             "", success ? "success" : "failed",
                             duration.count());
    }

    // Memory cache (Redis only; PostgreSQL remains the source of truth).
    // Cache-only: a Redis fault here must not convert a successful query
    // into an error response. Sandbox executions (SandboxQuery /
    // CompareAgents / deferred intervention runs) never touch long-term
    // memory: the sandbox flag guards the memory-hints / agent-switch path.
    if (success && memory_service_ && !request->sandbox()) {
        runCacheOnly([&] {
            memory_service_->updateUserMemoryFromHints(
                owner_id, {response->memory_hints().begin(), response->memory_hints().end()});
            // The summary is specialized for the taking-over assistant; the
            // response agent_name is the fallback when the router has no
            // registry entry.
            const std::string real_agent_id =
                response->agent_id().empty() ? "default" : response->agent_id();
            writeAgentSwitchMemory(owner_id, context_id, real_agent_id,
                                   response->agent_name());
        }, "query memory cache");
    }

    // Step 6: exactly-once terminal persistence.
    finalizeDurableQuery(run, success ? "completed" : "failed",
                         response_text, error_message);

    if (success) {
        helpers_.updateTaskStatus(request_id, "completed",
                         response->agent_id(), response->agent_name());
        auto* tc = common::TraceContext::current();
        if (tc) {
            // Estimate-only accounting (no provider usage passthrough yet):
            // prompt = 64 message-skeleton tokens + ~4 bytes per token;
            // completion = response bytes / 4 (same coarse estimate the
            // finalize ledger uses).
            const int est_prompt =
                static_cast<int>(estimateTokens(run.question));
            const int est_completion =
                static_cast<int>(response_text.size() / 4);
            common::CostTracker::instance().recordLLMCall(
                tc->traceId(), owner_id, context_id,
                response->agent_id(), "server_query",
                est_prompt, est_completion, "unknown", duration.count());
        }
        LOG_INFO("AI query completed: " + request_id +
                " in " + std::to_string(duration.count()) + "ms");
    } else {
        helpers_.updateTaskStatus(request_id, "failed", "", "", error_message);
        LOG_ERROR("AI query failed: " + request_id + " - " + error_message);
    }

    if (success) {
        return grpc::Status::OK;
    }
    grpc::StatusCode grpc_code = a2a_adapter::ErrorMapper::mapIntToGrpcStatus(
        response->status().code());
    return grpc::Status(grpc_code, sanitizeErrorMessage(
        error_message.empty() ? response->status().message() : error_message));

    } catch (const std::exception& error) {
        abortDurableRun(run, error.what());
        const bool persistence_fault = common::isPostgresError(error);
        // The client-facing status message is fixed, sanitized text.
        // pqxx::sql_error::what() embeds the failing SQL statement, so raw
        // exception detail stays in the server log only and never rides the
        // response.
        LOG_ERROR(std::string("Query pipeline crashed: ") + error.what());
        return grpc::Status(
            persistence_fault ? grpc::StatusCode::UNAVAILABLE
                              : grpc::StatusCode::INTERNAL,
            persistence_fault ? "Query unavailable: persistence layer error"
                              : "Query failed unexpectedly");
    }
}

grpc::Status AIQueryServiceImpl::QueryStream(
    grpc::ServerContext* context,
    const agent_communication::AIQueryRequest* request,
    grpc::ServerWriter<agent_communication::AIStreamEvent>* writer) {

    if (!isAvailable()) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                           "AI Query Service not available");
    }
    if (!request || !writer) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                           "Invalid request or writer");
    }

    // Step 1: owner from the authenticated session only.
    std::string owner_id = AuthInterceptor::currentUserId();
    if (!AuthInterceptor::isAuthenticated() || owner_id.empty()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                           "Valid authentication token required");
    }

    // Crash guard: PG/Redis faults become gRPC errors with a "failed"
    // terminal row; they must never reach the gRPC handler as exceptions.
    DurableQueryRun run;
    try {
    auto start_time = std::chrono::steady_clock::now();

    // Step 2: stable identifiers.
    std::string request_id = request->request_id();
    if (request_id.empty()) {
        request_id = QueryHelpers::generateRequestId();
    }
    std::string context_id = request->context_id();
    if (context_id.empty()) {
        context_id = "ctx-" + request_id;
    }

    LOG_INFO("Processing streaming AI query: " + request_id);

    run.owner_id = owner_id;
    run.conversation_id = context_id;
    run.request_id = request_id;
    run.question = request->question();
    run.model = common::envOrDefault("LLM_MODEL", "deepseek-v4-pro");

    const std::string route = orchestrator_enabled_ ? "multi-agent" : "single-agent-a2a";
    nlohmann::json plan_json;
    plan_json["mode"] = "stream";
    plan_json["request_id"] = request_id;
    if (!beginDurableRows(run, route, plan_json.dump())) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                           "Failed to persist query start");
    }

    // Step 4: budget reservation (rejected terminal persisted inside).
    auto budget_status = reserveBudgetOrReject(run);
    if (!budget_status.ok()) {
        return budget_status;
    }

    // Step 5: enrich; set_user_id is unconditional.
    agent_communication::AIQueryRequest enriched_req = *request;
    enriched_req.set_user_id(owner_id);
    enriched_req.set_request_id(request_id);
    enriched_req.set_context_id(context_id);
    buildSystemContextFromPg(owner_id, context_id,
                             enriched_req.mutable_system_context(),
                             request->sandbox());

    common::TraceContext::init(owner_id, context_id);
    helpers_.updateTaskStatus(request_id, "working");

    // This service is the single emitter of terminal stream events. Lower
    // layers (A2A adapter, MultiAgentHandler) only produce non-terminal
    // events; their terminal events are filtered by the relays below.
    std::atomic<bool> terminal_emitted{false};
    auto emitTerminal = [&terminal_emitted, writer, &run](
            const std::string& event_type, const std::string& content) {
        bool expected = false;
        if (!terminal_emitted.compare_exchange_strong(expected, true)) {
            return;
        }
        agent_communication::AIStreamEvent terminal;
        terminal.set_event_type(event_type);
        terminal.set_content(content);
        terminal.set_context_id(run.conversation_id);
        if (event_type == "complete") {
            if (auto* tc = common::TraceContext::current()) {
                terminal.set_trace_summary(tc->traceSummary());
            }
        }
        writer->Write(terminal);
    };

    // Multi-agent orchestrator path
    if (orchestrator_enabled_) {
        auto status = multi_agent_handler_->handleQueryStream(
            context, &enriched_req, writer, request_id);
        std::string answer = takeMultiAgentStreamedAnswer();
        std::string lower_error = takeMultiAgentStreamError();
        std::string stream_agent_id = takeMultiAgentStreamedAgentId();
        std::string stream_agent_name = takeMultiAgentStreamedAgentName();

        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time);
        QueryHelpers::recordMetrics("QueryStream", duration.count(), status.ok());

        if (status.ok()) {
            // Streaming-path memory wiring: the handler hands back the real
            // acting agent, so the agent-switch pipeline (last_agent write +
            // async cross-agent summary) runs here exactly as on the sync
            // Query path. Memory hints have no streaming channel
            // (AIStreamEvent carries none), so updateUserMemoryFromHints is
            // sync-path-only. Sandbox runs skip long-term memory, mirroring
            // the sync-path !request->sandbox() guard.
            if (memory_service_ && !request->sandbox() && !stream_agent_id.empty()) {
                runCacheOnly([&] {
                    writeAgentSwitchMemory(owner_id, context_id,
                                           stream_agent_id, stream_agent_name);
                }, "stream query memory cache");
            }
            emitTerminal("complete", "");
            finalizeDurableQuery(run, "completed", answer, "");
            helpers_.updateTaskStatus(request_id, "completed");
            return grpc::Status::OK;
        }
        if (status.error_code() == grpc::StatusCode::CANCELLED) {
            finalizeDurableQuery(run, "cancelled", answer, "Request cancelled");
            helpers_.updateTaskStatus(request_id, "cancelled");
            return status;
        }
        const std::string message = lower_error.empty() ? status.error_message() : lower_error;
        emitTerminal("error", sanitizeErrorMessage(message));
        finalizeDurableQuery(run, "failed", answer, message);
        helpers_.updateTaskStatus(request_id, "failed", "", "", message);
        return status;
    }

    // Circuit breaker check
    // Circuit breaker check
    if (circuit_breaker_ && !circuit_breaker_->isRequestAllowed()) {
        LOG_WARN("A2A backend circuit breaker open, rejecting streaming query: " + request_id);
        finalizeDurableQuery(run, "failed", "", "Circuit breaker open");
        helpers_.updateTaskStatus(request_id, "failed", "", "", "Circuit breaker open");
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "A2A backend circuit breaker open");
    }

    bool cancelled = false;
    bool write_failed = false;
    std::string lower_error;
    std::string streamed_content;

    // Propagate gRPC deadline to A2A HTTP timeout
    if (context->deadline() != std::chrono::system_clock::time_point::max()) {
        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            context->deadline() - std::chrono::system_clock::now());
        long timeout_sec = std::max(1L, static_cast<long>(remaining.count()));
        a2a_adapter_->setRequestTimeout(timeout_sec);
    }

    common::TraceContext::current()->startSpan("process_query_stream", "server");
    a2a_adapter_->processQueryStreaming(enriched_req,
        [&context, writer, &cancelled, &write_failed, &lower_error,
         &streamed_content](const agent_communication::AIStreamEvent& event) {

            // Relay filter: lower-layer terminal events are dropped; the
            // service emits the single terminal event after the run ends.
            if (event.event_type() == "complete") {
                return;
            }
            if (event.event_type() == "error") {
                if (lower_error.empty()) {
                    lower_error = event.content().empty()
                        ? "Agent reported an error" : event.content();
                }
                return;
            }

            if (context->IsCancelled()) {
                cancelled = true;
                return;
            }

            if (event.event_type() == "partial") {
                streamed_content += event.content();
            }

            if (!writer->Write(event)) {
                write_failed = true;
            }
        });
    common::TraceContext::current()->endSpan();

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);

    const bool success = !cancelled && lower_error.empty() && !write_failed;
    QueryHelpers::recordMetrics("QueryStream", duration.count(), success);

    if (circuit_breaker_) {
        if (success) circuit_breaker_->recordSuccess();
        else circuit_breaker_->recordFailure();
    }

    if (cancelled) {
        // Client/gRPC cancellation: persist the cancelled terminal state.
        finalizeDurableQuery(run, "cancelled", streamed_content, "Request cancelled");
        helpers_.updateTaskStatus(request_id, "cancelled");
        recordInvocationFact(run.owner_id, request_id, "", "", "cancelled",
                             duration.count());
        a2a_adapter_->cancelTask(request_id);
        return grpc::Status(grpc::StatusCode::CANCELLED, "Request cancelled");
    }

    if (!lower_error.empty()) {
        emitTerminal("error", sanitizeErrorMessage(lower_error));
        finalizeDurableQuery(run, "failed", streamed_content, lower_error);
        helpers_.updateTaskStatus(request_id, "failed", "", "", lower_error);
        recordInvocationFact(run.owner_id, request_id, "", "", "failed",
                             duration.count());
        LOG_ERROR("Streaming AI query failed: " + request_id + " - " + lower_error);
        return grpc::Status(grpc::StatusCode::INTERNAL, sanitizeErrorMessage(lower_error));
    }

    if (write_failed) {
        finalizeDurableQuery(run, "failed", streamed_content,
                             "Failed to write stream event");
        helpers_.updateTaskStatus(request_id, "failed", "", "",
                                  "Failed to write stream event");
        recordInvocationFact(run.owner_id, request_id, "", "", "failed",
                             duration.count());
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to write stream event");
    }

    // Memory cache: last_agent is recorded with the real agent id at the end
    // of handleAgentSwitch (non-streaming Query path). The streaming paths
    // cannot reliably surface an agent identity — the A2A adapter returns no
    // agent id and the multi-agent stream only exposes the accumulated answer
    // through a thread-local slot — so no fake "default" placeholder is
    // written here anymore.

    // Step 6: single terminal event + exactly-once persistence.
    emitTerminal("complete", "");
    finalizeDurableQuery(run, "completed", streamed_content, "");
    helpers_.updateTaskStatus(request_id, "completed");
    recordInvocationFact(run.owner_id, request_id, "", "", "success",
                         duration.count());
    auto* tc = common::TraceContext::current();
    // Estimate-only accounting (no provider usage passthrough yet): prompt =
    // 64 message-skeleton tokens + ~4 bytes per token; completion =
    // accumulated streamed bytes / 4.
    const int stream_est_prompt =
        static_cast<int>(estimateTokens(run.question));
    const int stream_est_completion =
        static_cast<int>(streamed_content.size() / 4);
    common::CostTracker::instance().recordLLMCall(
        tc ? tc->traceId() : "", owner_id, context_id, "", "server_stream",
        stream_est_prompt, stream_est_completion, "unknown", duration.count());
    LOG_INFO("Streaming AI query completed: " + request_id +
            " in " + std::to_string(duration.count()) + "ms");

    return grpc::Status::OK;

    } catch (const std::exception& error) {
        abortDurableRun(run, error.what());
        const bool persistence_fault = common::isPostgresError(error);
        // The client-facing status message is fixed, sanitized text.
        // pqxx::sql_error::what() embeds the failing SQL statement, so raw
        // exception detail stays in the server log only and never rides the
        // response.
        LOG_ERROR(std::string("Query pipeline crashed: ") + error.what());
        return grpc::Status(
            persistence_fault ? grpc::StatusCode::UNAVAILABLE
                              : grpc::StatusCode::INTERNAL,
            persistence_fault ? "Query unavailable: persistence layer error"
                              : "Query failed unexpectedly");
    }
}

grpc::Status AIQueryServiceImpl::GetQueryStatus(
    grpc::ServerContext* context,
    const agent_communication::QueryStatusRequest* request,
    agent_communication::QueryStatusResponse* response) {

    if (!isAvailable()) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                           "AI Query Service not available");
    }
    if (!request || !response) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                           "Invalid request or response");
    }
    if (!AuthInterceptor::isAuthenticated()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                           "Valid authentication token required");
    }
    if (context->IsCancelled()) {
        return grpc::Status(grpc::StatusCode::CANCELLED, "Request cancelled");
    }
    if (request->task_id().empty() && request->context_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                           "task_id or context_id is required");
    }

    // Durable status lookup: PostgreSQL query_logs is the source of
    // truth (the old in-memory task cache was process-local and returned
    // success/unknown placeholders). Owner always comes from the
    // authenticated context; missing or foreign rows are NOT_FOUND.
    const std::string owner_id = AuthInterceptor::currentUserId();
    if (owner_id.empty()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                           "Authenticated owner context required");
    }
    if (!domain_repo_) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                           "Durable query store not initialized");
    }

    LOG_INFO("Getting query status for task: " + request->task_id());

    try {
        std::optional<common::QueryLogRecord> query_log;
        if (!request->task_id().empty()) {
            query_log = domain_repo_->getQueryLogById(owner_id, request->task_id());
        } else {
            query_log = domain_repo_->getLatestQueryLogByConversation(
                owner_id, request->context_id());
        }
        if (!query_log.has_value()) {
            return grpc::Status(
                grpc::StatusCode::NOT_FOUND,
                "No query record exists for the given task or context "
                "(or it belongs to another owner)");
        }

        auto* status = response->mutable_status();
        status->set_code(0);
        status->set_message("OK");
        response->set_task_state(query_log->status);

        // Conversation history straight from the durable message store, in
        // sequence order (timestamp left unset: created_at is server-side).
        for (const auto& message :
             domain_repo_->listMessages(owner_id, query_log->conversation_id)) {
            auto* hist = response->add_history();
            hist->set_message_id(message.id);
            hist->set_role(message.role);
            hist->set_content(message.content);
        }
        return grpc::Status::OK;
    } catch (const std::exception& error) {
        LOG_ERROR(std::string("GetQueryStatus failed: ") + error.what());
        if (common::isPostgresError(error)) {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                               "Query status store is unavailable");
        }
        return grpc::Status(grpc::StatusCode::INTERNAL,
                           "Failed to read query status");
    }
}

grpc::Status AIQueryServiceImpl::GetAgentMetrics(
    grpc::ServerContext* context,
    const agent_communication::GetAgentMetricsRequest* request,
    agent_communication::GetAgentMetricsResponse* response) {

    (void)context;

    if (!request || !response) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                           "Invalid request or response");
    }
    if (!AuthInterceptor::isAuthenticated()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                           "Valid authentication token required");
    }

    const std::string& agent_id = request->agent_id();
    if (agent_id.empty()) {
        auto* status = response->mutable_status();
        status->set_code(-1);
        status->set_message("agent_id is required");
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "agent_id is required");
    }

    LOG_INFO("GetAgentMetrics for agent: " + agent_id);

    std::string redis_key = "agent_metrics:" + agent_id;
    std::map<std::string, std::string> metrics_data;

    if (!redis_client_ || !redis_client_->hgetall(redis_key, metrics_data)) {
        LOG_WARN("No metrics found for agent: " + agent_id);
        auto* status = response->mutable_status();
        status->set_code(0);
        status->set_message("No metrics available for this agent");
        auto* metrics = response->mutable_metrics();
        metrics->set_agent_id(agent_id);
        return grpc::Status::OK;
    }

    auto* metrics = response->mutable_metrics();
    metrics->set_agent_id(agent_id);

    auto get_double = [&](const std::string& field) -> double {
        auto it = metrics_data.find(field);
        if (it != metrics_data.end() && !it->second.empty()) {
            try { return std::stod(it->second); } catch (...) {}
        }
        return 0.0;
    };
    auto get_int = [&](const std::string& field) -> int32_t {
        auto it = metrics_data.find(field);
        if (it != metrics_data.end() && !it->second.empty()) {
            try { return std::stoi(it->second); } catch (...) {}
        }
        return 0;
    };

    metrics->set_success_rate(get_double("success_rate"));
    metrics->set_avg_latency_ms(get_double("avg_latency_ms"));
    metrics->set_p95_latency_ms(get_double("p95_latency_ms"));
    metrics->set_total_requests(get_int("total_requests"));
    metrics->set_approval_rate(get_double("approval_rate"));

    auto* status = response->mutable_status();
    status->set_code(0);
    status->set_message("OK");
    return grpc::Status::OK;
}

// OrchestrationService delegation
grpc::Status AIQueryServiceImpl::ExecutePlan(
    grpc::ServerContext* context,
    const agent_communication::ExecutePlanRequest* request,
    agent_communication::ExecutePlanResponse* response) {

    if (!isAvailable()) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "AI Query Service not available");
    }
    if (!request || !response) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid request or response");
    }
    if (!orchestration_impl_) {
        auto* status = response->mutable_status();
        status->set_code(-1);
        status->set_message("Multi-agent orchestrator is not enabled");
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                           "Multi-agent orchestrator is not enabled");
    }
    return orchestration_impl_->executePlan(context, request, response);
}

grpc::Status AIQueryServiceImpl::ReplayQuery(
    grpc::ServerContext* context,
    const agent_communication::ReplayQueryRequest* request,
    agent_communication::ReplayQueryResponse* response) {

    if (!request || !response) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid request or response");
    }
    if (orchestration_impl_) {
        return orchestration_impl_->replayQuery(context, request, response);
    }
    // Replay is durable (PostgreSQL + pipeline) and works even when
    // the multi-agent orchestrator is disabled; owner comes from the
    // authenticated session.
    return orchestrator::ReplayService::handleReplayRequest(
        AuthInterceptor::currentUserId(), request, response);
}

grpc::Status AIQueryServiceImpl::ExportConversation(
    grpc::ServerContext* context,
    const agent_communication::ExportConversationRequest* request,
    agent_communication::ExportConversationResponse* response) {

    if (!request || !response) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid request or response");
    }
    if (orchestration_impl_) {
        return orchestration_impl_->exportConversation(context, request, response);
    }
    // Export reads PostgreSQL conversation messages directly and does
    // not depend on the multi-agent orchestrator.
    return orchestrator::ExportService::handleExportRequest(
        AuthInterceptor::currentUserId(), request, response);
}

} // namespace server
} // namespace agent_rpc
