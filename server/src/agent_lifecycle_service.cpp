#include "agent_rpc/server/agent_lifecycle_service.h"
#include "agent_rpc/server/auth_interceptor.h"
#include "agent_rpc/common/agent_runtime_repository.h"
#include "agent_rpc/common/query_domain_repository.h"
#include "agent_rpc/common/logger.h"
#include <json/json.h>

#include <pqxx/pqxx>

#include <cstdint>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace agent_rpc { namespace server {
namespace {

// Locally generated primary keys keep the service free of database sequences
// while staying collision-safe (same scheme as the durable-domain repository).
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

// Feedback dimensions form a closed key space. agent_id and skill_name are
// bounded identifiers from the known charset — unknown/arbitrary keys are
// refused before any write so the feedback/quality tables never accumulate
// garbage dimensions. (deep-review R12: empty values are refused too — a
// row with agent_id='' would aggregate into an owner/agent combination no
// consumer ever reads, silently dropping the feedback.)
bool validFeedbackKey(const std::string& value) {
    constexpr std::size_t kMaxLength = 128;
    if (value.empty()) {
        return false;
    }
    if (value.size() > kMaxLength) {
        return false;
    }
    for (const char character : value) {
        const bool allowed = (character >= 'a' && character <= 'z') ||
                             (character >= 'A' && character <= 'Z') ||
                             (character >= '0' && character <= '9') ||
                             character == '-' || character == '_' ||
                             character == '.' || character == ':';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

}  // namespace

AgentLifecycleServiceImpl::AgentLifecycleServiceImpl(common::RedisClient* redis) : redis_(redis) {}

void AgentLifecycleServiceImpl::setAgentRuntimeRepository(
    common::AgentRuntimeRepository* repository) {
    runtime_repository_ = repository;
}

void AgentLifecycleServiceImpl::setQueryDomainRepository(
    common::QueryDomainRepository* repository) {
    query_repository_ = repository;
}

void AgentLifecycleServiceImpl::setExecutor(PipelineExecutor executor) {
    executor_ = std::move(executor);
}

grpc::Status AgentLifecycleServiceImpl::SubmitFeedback(
    grpc::ServerContext* ctx, const agent_communication::SubmitFeedbackRequest* request,
    agent_communication::SubmitFeedbackResponse* response) {
    if (!AuthInterceptor::isAuthenticated())
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Authentication required");

    // The feedback owner is always the authenticated user; any client-supplied
    // identity is ignored (owner only ever comes from the auth context).
    const std::string owner = AuthInterceptor::currentUserId();

    if (request->rating() < 1 || request->rating() > 5) {
        response->mutable_status()->set_code(1);
        response->mutable_status()->set_message("rating must be between 1 and 5");
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "rating must be between 1 and 5");
    }

    // Closed key space — refuse oversized or arbitrary keys.
    if (!validFeedbackKey(request->agent_id()) || !validFeedbackKey(request->skill_name())) {
        response->mutable_status()->set_code(1);
        response->mutable_status()->set_message(
            "agent_id/skill_name must match [A-Za-z0-9._:-]{1,128}");
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "agent_id/skill_name must match [A-Za-z0-9._:-]{1,128}");
    }

    if (runtime_repository_ == nullptr || query_repository_ == nullptr) {
        response->mutable_status()->set_code(1);
        response->mutable_status()->set_message("Feedback persistence not available");
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "Feedback persistence not available");
    }

    // Feedback must reference a trace that belongs to the caller. Traces are
    // stored as "trace-<request_id>" but clients may send the bare id, so try
    // both spellings. A missing or foreign trace is NOT_FOUND — never leak
    // another owner's trace existence via a different error.
    std::optional<common::TraceRecord> trace =
        query_repository_->getTraceById(owner, request->trace_id());
    if (!trace.has_value()) {
        trace = query_repository_->getTraceById(owner, "trace-" + request->trace_id());
    }
    if (!trace.has_value()) {
        response->mutable_status()->set_code(1);
        response->mutable_status()->set_message("Trace not found");
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Trace not found for current user");
    }

    common::RuntimeFeedbackRecord feedback;
    feedback.owner_id = owner;
    feedback.query_log_id = trace->query_log_id;
    feedback.trace_id = trace->id;
    feedback.agent_id = request->agent_id();
    feedback.skill_name = request->skill_name();
    feedback.rating = request->rating();
    feedback.comment = request->comment();

    // Persist with a unique_violation safety net: on the astronomically rare
    // primary-key collision, retry once with a fresh id. Any database error
    // is mapped to INTERNAL here — exceptions must never escape the handler.
    bool inserted = false;
    for (int attempt = 0; attempt < 2 && !inserted; ++attempt) {
        feedback.id = generateRowId("feedback");
        try {
            inserted = runtime_repository_->insertFeedback(feedback);
        } catch (const pqxx::unique_violation& error) {
            LOG_WARN(std::string{"SubmitFeedback: primary-key collision, retrying with a new id: "} +
                     error.what());
        } catch (const std::exception& error) {
            LOG_ERROR(std::string{"SubmitFeedback: feedback insert failed: "} + error.what());
            response->mutable_status()->set_code(1);
            response->mutable_status()->set_message("Failed to persist feedback");
            return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to persist feedback");
        }
    }
    if (!inserted) {
        response->mutable_status()->set_code(1);
        response->mutable_status()->set_message("Failed to persist feedback");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to persist feedback");
    }

    // Recompute the owner-scoped route quality row immediately so routing
    // picks up the new rating without waiting for the periodic aggregator.
    // Aggregation failure is non-fatal: the raw feedback row is already
    // durable and the aggregator will catch up.
    if (!runtime_repository_->aggregateRouteQuality(owner, request->agent_id(),
                                                    request->skill_name())) {
        LOG_WARN("SubmitFeedback: route quality aggregation failed for owner=" + owner +
                 " agent=" + request->agent_id() + " skill=" + request->skill_name());
    }

    response->mutable_status()->set_code(0);
    response->mutable_status()->set_message("OK");
    LOG_INFO("SubmitFeedback: owner=" + owner + " agent=" + request->agent_id() +
             " skill=" + request->skill_name() + " rating=" + std::to_string(request->rating()));
    return grpc::Status::OK;
}

grpc::Status AgentLifecycleServiceImpl::GetAgentCompare(
    grpc::ServerContext* ctx, const agent_communication::GetAgentCompareRequest* /*request*/,
    agent_communication::GetAgentCompareResponse* response) {
    (void)ctx;
    if (!AuthInterceptor::isAuthenticated())
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Authentication required");
    const std::string owner = AuthInterceptor::currentUserId();
    if (query_repository_ == nullptr) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "Compare persistence not available");
    }

    // Real summary of the owner's persisted compare_runs rows — the
    // empty list is the true empty state, never fabricated metrics.
    try {
        const auto runs = query_repository_->listCompareRunsByOwner(owner);
        for (const auto& run : runs) {
            auto* summary = response->add_runs();
            summary->set_run_id(run.id);
            summary->set_request_text(run.request_text);
            summary->set_status(run.status);
            summary->set_results_json(run.results);
            summary->set_created_at(run.created_at);
        }
        response->mutable_status()->set_code(0);
        response->mutable_status()->set_message("OK");
        return grpc::Status::OK;
    } catch (const std::exception& error) {
        LOG_ERROR(std::string{"GetAgentCompare failed: "} + error.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to load compare runs");
    }
}

grpc::Status AgentLifecycleServiceImpl::SetAutonomyLevel(
    grpc::ServerContext* ctx, const agent_communication::SetAutonomyLevelRequest* request,
    agent_communication::SetAutonomyLevelResponse* response) {
    (void)ctx;
    if (!AuthInterceptor::isAuthenticated())
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Authentication required");
    // Owner comes exclusively from the auth context; any identity in
    // the request body is ignored on purpose.
    const std::string owner = AuthInterceptor::currentUserId();

    if (request->agent_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "agent_id is required");
    }
    if (request->level() < 1 || request->level() > 4) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "autonomy level must be between 1 and 4");
    }
    if (query_repository_ == nullptr) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "Autonomy persistence not available");
    }

    // PostgreSQL autonomy_settings(owner, agent) is the source of truth; the
    // upsert updates the same row in place on repeated sets.
    try {
        if (!query_repository_->upsertAutonomySetting(owner, request->agent_id(),
                                                      request->level())) {
            return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to persist autonomy level");
        }
    } catch (const std::exception& error) {
        LOG_ERROR(std::string{"SetAutonomyLevel failed: "} + error.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to persist autonomy level");
    }

    response->mutable_status()->set_code(0);
    response->mutable_status()->set_message("OK");
    LOG_INFO("SetAutonomyLevel: owner=" + owner + " agent=" + request->agent_id() +
             " level=" + std::to_string(request->level()));
    return grpc::Status::OK;
}

grpc::Status AgentLifecycleServiceImpl::UndoAction(
    grpc::ServerContext* ctx, const agent_communication::UndoActionRequest* request,
    agent_communication::UndoActionResponse* response) {
    (void)ctx;
    if (!AuthInterceptor::isAuthenticated())
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Authentication required");
    const std::string owner = AuthInterceptor::currentUserId();

    if (request->action_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "action_id is required");
    }
    if (query_repository_ == nullptr) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "Undo persistence not available");
    }

    try {
        // Owner-scoped read first: unknown/foreign ids are NOT_FOUND (no
        // existence leak), already-undone rows are a real conflict, expired
        // rows are refused before any inverse payload runs.
        const auto action = query_repository_->getUndoActionById(owner, request->action_id());
        if (!action.has_value()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                "Undo action not found for current user");
        }
        if (!action->undone_at.empty()) {
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "Action already undone");
        }
        if (action->expired) {
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                "Undo action has expired");
        }

        // Validate the inverse payload BEFORE consuming the action: a
        // malformed/unknown payload must never burn undone_at.
        Json::Value payload;
        Json::CharReaderBuilder payload_reader_builder;
        std::string payload_errors;
        std::istringstream payload_stream{action->action_payload};
        if (!Json::parseFromStream(payload_reader_builder, payload_stream, &payload,
                                   &payload_errors) ||
            !payload.isMember("operation")) {
            return grpc::Status(grpc::StatusCode::INTERNAL, "Malformed undo payload");
        }
        const std::string operation = payload["operation"].asString();
        if (operation != "restore_intervention") {
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                "Unknown undo operation: " + operation);
        }
        const std::string intervention_id = payload["intervention_id"].asString();
        if (intervention_id.empty()) {
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                "Undo payload misses intervention_id");
        }

        // Atomic: the undone_at CAS and the inverse restore commit or roll
        // back in ONE transaction, so a failed inverse never consumes the
        // action and the owner can retry. Currently the only registered
        // inverse is restore_intervention (written by InterventionResponse).
        const auto outcome = query_repository_->undoRestoreIntervention(
            owner, request->action_id(), intervention_id);
        if (outcome == common::UndoOutcome::kNotFound) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                "Undo action not found for current user");
        }
        if (outcome == common::UndoOutcome::kAlreadyUndone) {
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS, "Action already undone");
        }
        if (outcome == common::UndoOutcome::kInverseFailed) {
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                "Inverse operation failed; undo not consumed, retryable");
        }

        response->set_success(true);
        response->set_message("OK");
        response->mutable_status()->set_code(0);
        response->mutable_status()->set_message("OK");
        LOG_INFO("UndoAction: owner=" + owner + " action=" + request->action_id() +
                 " operation=" + operation);
        return grpc::Status::OK;
    } catch (const std::exception& error) {
        LOG_ERROR(std::string{"UndoAction failed: "} + error.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, "Undo failed");
    }
}

grpc::Status AgentLifecycleServiceImpl::CompareAgents(
    grpc::ServerContext* ctx, const agent_communication::CompareAgentsRequest* request,
    agent_communication::CompareAgentsResponse* response) {
    if (!AuthInterceptor::isAuthenticated())
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Authentication required");
    const std::string owner = AuthInterceptor::currentUserId();

    // Validation: 1..3 distinct agents and a non-empty question.
    if (request->question().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "question is required");
    }
    if (request->agent_ids_size() < 1 || request->agent_ids_size() > 3) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "compare requires between 1 and 3 agents");
    }
    std::set<std::string> unique_agents;
    for (const auto& agent_id : request->agent_ids()) {
        if (agent_id.empty() || !unique_agents.insert(agent_id).second) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "agent_ids must be non-empty and unique");
        }
    }
    if (runtime_repository_ == nullptr || query_repository_ == nullptr || !executor_) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "Compare execution not available");
    }
    // Overall cancellation: a client-cancelled call never starts new work.
    if (ctx != nullptr && ctx->IsCancelled()) {
        return grpc::Status(grpc::StatusCode::CANCELLED, "Compare cancelled before start");
    }

    struct AgentOutcome {
        std::string agent_id;
        std::string request_id;
        std::string status = "failed";  // completed | failed | cancelled
        std::string answer;
        std::string error;
        bool executed = false;
    };

    common::CompareRunRecord run;
    bool run_created = false;
    try {
        const std::string run_id = generateRowId("cmp");
        run.id = run_id;
        run.owner_id = owner;
        run.query_log_id = run_id;
        run.request_text = request->question();
        run.status = "running";
        if (!query_repository_->createCompareRun(run)) {
            return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to persist compare run");
        }
        run_created = true;

        // Worker threads never see the AuthInterceptor, so propagate the
        // already-validated owner context snapshot before spawning them.
        const AuthInterceptor::AuthContext auth_snapshot = AuthInterceptor::currentAuth();

        std::vector<AgentOutcome> outcomes(static_cast<std::size_t>(request->agent_ids_size()));
        std::vector<std::thread> workers;
        workers.reserve(outcomes.size());
        for (std::size_t index = 0; index < outcomes.size(); ++index) {
            AgentOutcome& outcome = outcomes[index];
            outcome.agent_id = request->agent_ids(static_cast<int>(index));

            // Health gate: only agents registered as healthy may execute;
            // unhealthy/unknown agents fail independently without touching
            // the pipeline (their failure stays visible per agent).
            std::optional<common::AgentRegistryRecord> registry;
            try {
                registry = runtime_repository_->getAgent(outcome.agent_id);
            } catch (const std::exception& error) {
                LOG_WARN(std::string{"CompareAgents registry lookup failed: "} + error.what());
            }
            if (!registry.has_value() || registry->health_status != "healthy") {
                outcome.error = !registry.has_value()
                                    ? "agent is not registered"
                                    : "agent is not healthy (status=" + registry->health_status + ")";
                continue;
            }

            workers.emplace_back([this, &outcome, &auth_snapshot, &run_id,
                                  question = request->question()] {
                AuthInterceptor::propagateAuth(auth_snapshot);
                outcome.request_id = generateRowId("req");
                const std::string context_id =
                    "compare-" + run_id + "-" + outcome.agent_id;
                outcome.executed = true;
                if (executor_(outcome.request_id, context_id, question, outcome.answer,
                              outcome.error)) {
                    outcome.status = "completed";
                }
            });
        }
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        // Aggregate: a single failed agent never masks the whole run.
        int completed = 0;
        int failed = 0;
        for (const auto& outcome : outcomes) {
            if (outcome.status == "completed") {
                ++completed;
            } else {
                ++failed;
            }
        }
        std::string run_status;
        if (ctx != nullptr && ctx->IsCancelled()) {
            run_status = "cancelled";
        } else if (failed == 0) {
            run_status = "completed";
        } else if (completed == 0) {
            run_status = "failed";
        } else {
            run_status = "partial";
        }

        Json::Value results_json(Json::arrayValue);
        for (const auto& outcome : outcomes) {
            auto* entry = response->add_results();
            entry->set_agent_id(outcome.agent_id);
            entry->set_status(outcome.status);
            entry->set_answer(outcome.status == "completed" ? outcome.answer : "");
            entry->set_error(outcome.status == "completed" ? "" : outcome.error);
            entry->set_request_id(outcome.request_id);
            if (!outcome.request_id.empty()) {
                entry->set_trace_id("trace-" + outcome.request_id);
            }

            Json::Value item;
            item["agent_id"] = outcome.agent_id;
            item["status"] = outcome.status;
            item["answer"] = entry->answer();
            item["error"] = entry->error();
            item["request_id"] = outcome.request_id;
            results_json.append(item);
        }
        Json::StreamWriterBuilder json_builder;
        json_builder["indentation"] = "";
        const std::string results_text = Json::writeString(json_builder, results_json);

        run.results = results_text;
        run.status = run_status;
        if (!query_repository_->updateCompareRun(run)) {
            // Finalization failed: make one best-effort attempt to persist a
            // terminal state so the row never stays "running" forever; a
            // failed attempt is logged, not thrown.
            common::CompareRunRecord failed_run = run;
            failed_run.status = "failed";
            if (!query_repository_->updateCompareRun(failed_run)) {
                LOG_WARN("CompareAgents: could not persist terminal state for run " +
                         run_id);
            }
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                "Failed to finalize compare run");
        }

        response->set_run_id(run_id);
        response->set_run_status(run_status);
        response->mutable_status()->set_code(0);
        response->mutable_status()->set_message("OK");
        LOG_INFO("CompareAgents: owner=" + owner + " run=" + run_id +
                 " status=" + run_status +
                 " completed=" + std::to_string(completed) +
                 " failed=" + std::to_string(failed));
        return grpc::Status::OK;
    } catch (const std::exception& error) {
        LOG_ERROR(std::string{"CompareAgents failed: "} + error.what());
        // The run row must never stay "running" forever: best-effort terminal
        // write, failures only logged.
        if (run_created) {
            try {
                run.status = "failed";
                if (!query_repository_->updateCompareRun(run)) {
                    LOG_WARN("CompareAgents: could not persist terminal state for run " +
                             run.id);
                }
            } catch (const std::exception& finalize_error) {
                LOG_WARN(std::string{"CompareAgents terminal-state write failed: "} +
                         finalize_error.what());
            }
        }
        return grpc::Status(grpc::StatusCode::INTERNAL, "Compare execution failed");
    }
}

}} // namespaces
