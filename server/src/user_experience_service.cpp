/**
 * @file user_experience_service.cpp
 * @brief Real Sandbox + Intervention workflows on PostgreSQL.
 *
 * SandboxQuery executes through the already-initialized durable Query
 * pipeline (the only execution entry point) with the sandbox flag set, and
 * persists a sandbox_runs row next to the pipeline's query/trace/cost rows.
 * The owner's autonomy level gates execution: level <= 2 (or no setting at
 * all — conservative default) creates a pending intervention instead of
 * executing; PROCEED/MODIFY on that intervention later trigger the deferred
 * sandbox execution, so the decision has real causal effect.
 *
 * InterventionResponse is a single owner-scoped CAS on the interventions
 * row (pending -> decision); duplicates and foreign ids map to real
 * ALREADY_EXISTS / NOT_FOUND errors. Reversible decisions (PROCEED, MODIFY,
 * SKIP) additionally write an undo_actions row whose inverse payload
 * restores the intervention to pending.
 */
#include "agent_rpc/server/user_experience_service.h"
#include "agent_rpc/server/auth_interceptor.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/query_domain_repository.h"

#include <cstdint>
#include <random>
#include <string>

namespace agent_rpc {
namespace server {

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

bool isValidDecision(const std::string& decision) {
    return decision == "PROCEED" || decision == "MODIFY" || decision == "SKIP" ||
           decision == "ABORT";
}

// Runs one sandbox execution through the durable pipeline and records it in
// sandbox_runs. The pipeline owns budget/owner/trace/cost/finalize; this
// wrapper only persists the sandbox-specific bookkeeping row. Returns false
// (with error filled) when the pipeline or persistence refuses the run.
bool runSandboxExecution(const UserExperienceServiceImpl::PipelineExecutor& executor,
                         common::QueryDomainRepository* repository,
                         const std::string& owner_id, const std::string& question,
                         std::string& request_id, std::string& answer, std::string& error) {
    request_id = generateRowId("req");
    const std::string context_id = "sandbox-" + request_id;

    common::SandboxRunRecord run;
    run.id = generateRowId("sbrun");
    run.owner_id = owner_id;
    run.query_log_id = request_id;
    run.request_text = question;
    run.status = "running";
    if (!repository->createSandboxRun(run)) {
        error = "Failed to persist sandbox run";
        return false;
    }

    bool ok = false;
    try {
        ok = executor(request_id, context_id, question, answer, error);
    } catch (const std::exception& e) {
        // deep-review: an executor exception must not leave the run stuck in
        // "running" — the sandbox_runs row was already persisted above, so
        // finalize it as failed before the handler maps this to INTERNAL.
        ok = false;
        error = std::string("executor exception: ") + e.what();
        LOG_ERROR("Sandbox run executor threw: run=" + run.id +
                  " error=" + error);
    }
    run.status = ok ? "completed" : "failed";
    run.response_text = ok ? answer : error;
    if (!repository->updateSandboxRun(run)) {
        // The execution already happened; a failed finalization must not
        // masquerade as a failed run, but it must stay visible in the logs.
        LOG_WARN("Sandbox run finalization failed: run=" + run.id +
                 " request=" + request_id);
    }
    return ok;
}

}  // namespace

void UserExperienceServiceImpl::setQueryDomainRepository(
    common::QueryDomainRepository* repository) {
    query_repository_ = repository;
}

void UserExperienceServiceImpl::setExecutor(PipelineExecutor executor) {
    executor_ = std::move(executor);
}

grpc::Status UserExperienceServiceImpl::InterventionResponse(
    grpc::ServerContext* ctx,
    const agent_communication::InterventionResponseRequest* request,
    agent_communication::InterventionResponseResponse* response) {
    (void)ctx;
    if (!AuthInterceptor::isAuthenticated())
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Authentication required");
    // The owner is always the authenticated user; the request carries only the
    // intervention id selector and the decision.
    const std::string owner = AuthInterceptor::currentUserId();

    if (request->intervention_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "intervention_id is required");
    }
    if (!isValidDecision(request->decision())) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "decision must be one of PROCEED, MODIFY, SKIP, ABORT");
    }
    if (request->decision() == "MODIFY" && request->modification_text().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "MODIFY requires non-empty modification_text");
    }
    if (query_repository_ == nullptr) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "Intervention persistence not available");
    }

    try {
        // Single owner-scoped CAS: pending -> decision. Concurrent resolvers
        // and repeated responses lose the CAS and observe a real conflict.
        const auto outcome = query_repository_->resolveIntervention(
            owner, request->intervention_id(), request->decision(),
            request->modification_text());
        if (outcome == common::InterventionResolveOutcome::kNotFound) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                "Intervention not found for current user");
        }
        if (outcome == common::InterventionResolveOutcome::kAlreadyResolved) {
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS,
                                "Intervention already resolved");
        }
        response->set_new_state(request->decision());

        // Reversible decisions write an undo action whose inverse payload
        // restores the intervention to pending (executed by UndoAction).
        // ABORT is terminal by design and carries no undo record.
        if (request->decision() != "ABORT") {
            common::UndoActionRecord action;
            action.id = generateRowId("undo");
            action.owner_id = owner;
            action.resource_type = "intervention";
            action.resource_id = request->intervention_id();
            // Intervention ids are locally generated hex strings, so direct
            // interpolation into the JSON payload is injection-safe.
            action.action_payload =
                "{\"operation\":\"restore_intervention\",\"intervention_id\":\"" +
                request->intervention_id() + "\"}";
            action.version = 1;
            if (!query_repository_->createUndoAction(action)) {
                // deep-review: the resolve committed in its own transaction
                // above. Without its undo record the intervention would sit
                // resolved-but-never-executable with no way to recover, so
                // roll it back to pending (best-effort: only a row still
                // carrying this decision is restored). If the compensation
                // also fails the state stays visible for manual recovery.
                if (!query_repository_->restoreInterventionToPending(
                        owner, request->intervention_id(), request->decision())) {
                    LOG_ERROR("Intervention left resolved without undo record: " +
                              request->intervention_id());
                }
                return grpc::Status(grpc::StatusCode::INTERNAL,
                                    "Failed to persist undo action");
            }
            response->set_undo_action_id(action.id);
        }

        // PROCEED / MODIFY trigger the deferred sandbox execution that the
        // autonomy gate withheld. The intervention row carries the target
        // agent id in query_log_id until execution assigns a real one.
        if (request->decision() == "PROCEED" || request->decision() == "MODIFY") {
            if (!executor_) {
                return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                    "Sandbox executor not available");
            }
            const auto intervention = query_repository_->getInterventionById(
                owner, request->intervention_id());
            if (!intervention.has_value()) {
                return grpc::Status(grpc::StatusCode::INTERNAL,
                                    "Intervention vanished after resolution");
            }
            const std::string question =
                request->decision() == "MODIFY" ? request->modification_text()
                                                : intervention->original_request;
            std::string executed_request_id;
            std::string answer;
            std::string error;
            if (!runSandboxExecution(executor_, query_repository_, owner, question,
                                     executed_request_id, answer, error)) {
                return grpc::Status(grpc::StatusCode::INTERNAL,
                                    "Deferred sandbox execution failed: " + error);
            }
            response->set_executed_request_id(executed_request_id);
        }

        response->mutable_status()->set_code(0);
        response->mutable_status()->set_message("OK");
        LOG_INFO("InterventionResponse: owner=" + owner +
                 " intervention=" + request->intervention_id() +
                 " decision=" + request->decision());
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        LOG_ERROR(std::string{"InterventionResponse failed: "} + e.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, "Intervention resolution failed");
    }
}

grpc::Status UserExperienceServiceImpl::SandboxQuery(
    grpc::ServerContext* ctx,
    const agent_communication::SandboxQueryRequest* request,
    agent_communication::SandboxQueryResponse* response) {
    (void)ctx;
    if (!AuthInterceptor::isAuthenticated())
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Authentication required");
    const std::string owner = AuthInterceptor::currentUserId();

    if (request->query_text().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "query_text is required");
    }
    if (request->agent_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "agent_id is required");
    }
    if (query_repository_ == nullptr || !executor_) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "Sandbox execution not available");
    }

    try {
        // Autonomy gate: the owner's persisted level decides whether the
        // execution runs immediately (>= 3) or waits for an explicit
        // intervention decision (<= 2). No setting at all defaults to the
        // conservative confirmation path (level 1).
        int level = 1;
        const auto setting =
            query_repository_->getAutonomySetting(owner, request->agent_id());
        if (setting.has_value()) {
            level = setting->level;
        }
        if (level <= 2) {
            common::InterventionRecord intervention;
            intervention.id = generateRowId("intv");
            intervention.owner_id = owner;
            // Pending rows carry the target agent id in query_log_id; the
            // deferred execution reads it back when PROCEED/MODIFY arrives.
            intervention.query_log_id = request->agent_id();
            intervention.state = "pending";
            intervention.original_request = request->query_text();
            if (!query_repository_->createIntervention(intervention)) {
                return grpc::Status(grpc::StatusCode::INTERNAL,
                                    "Failed to persist intervention");
            }
            response->set_intervention_required(true);
            response->set_intervention_id(intervention.id);
            response->mutable_status()->set_code(0);
            response->mutable_status()->set_message("Confirmation required");
            LOG_INFO("SandboxQuery gated by autonomy level " + std::to_string(level) +
                     ": owner=" + owner + " agent=" + request->agent_id() +
                     " intervention=" + intervention.id);
            return grpc::Status::OK;
        }

        std::string request_id;
        std::string answer;
        std::string error;
        if (!runSandboxExecution(executor_, query_repository_, owner,
                                 request->query_text(), request_id, answer, error)) {
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                "Sandbox execution failed: " + error);
        }
        response->set_result(answer);
        response->set_request_id(request_id);
        response->mutable_status()->set_code(0);
        response->mutable_status()->set_message("OK");
        LOG_INFO("SandboxQuery completed: owner=" + owner +
                 " agent=" + request->agent_id() + " request=" + request_id);
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        LOG_ERROR(std::string{"SandboxQuery failed: "} + e.what());
        return grpc::Status(grpc::StatusCode::INTERNAL, "Sandbox execution failed");
    }
}

} // namespace server
} // namespace agent_rpc
