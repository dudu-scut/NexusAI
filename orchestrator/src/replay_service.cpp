#include "agent_rpc/orchestrator/replay_service.h"

#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/query_domain_repository.h"

#include "orchestration.pb.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>

namespace agent_rpc {
namespace orchestrator {

namespace {

// Generates a high-entropy identifier for the replayed request. Replay must
// never reuse the original request id: the durable pipeline treats the
// request id as the idempotency key of the original run. Every word is
// drawn directly from the OS entropy source (no PRNG state).
std::string generateReplayRequestId() {
    std::random_device entropy;
    std::ostringstream id;
    id << "replay-" << std::hex << std::setfill('0');
    for (int i = 0; i < 4; ++i) {
        const std::uint64_t low = entropy();
        const std::uint64_t high = entropy();
        const std::uint64_t word = (high << 32) | (low & 0xFFFFFFFFull);
        id << std::setw(16) << word;
    }
    return id.str();
}

// Extracts the stored route label from the query log's route_decision JSON
// (persisted by the durable pipeline as {"route": "..."}). Falls back to the
// raw string when the payload is not the expected shape.
std::string routeLabelFrom(const std::string& route_decision) {
    const auto parsed = nlohmann::json::parse(route_decision, nullptr, false);
    if (!parsed.is_discarded() && parsed.is_object() && parsed.contains("route") &&
        parsed["route"].is_string()) {
        return parsed["route"].get<std::string>();
    }
    return route_decision;
}

} // namespace

ReplayService& ReplayService::instance() {
    static ReplayService service;
    return service;
}

void ReplayService::configure(common::QueryDomainRepository* domain_repository,
                              RouteProvider route_provider,
                              PipelineExecutor executor) {
    std::lock_guard<std::mutex> lock(mutex_);
    domain_repository_ = domain_repository;
    route_provider_ = std::move(route_provider);
    executor_ = std::move(executor);
}

namespace {

grpc::Status handleReplayRequestImpl(
    common::QueryDomainRepository* repository,
    const ReplayService::RouteProvider& route_provider,
    const ReplayService::PipelineExecutor& executor,
    const std::string& owner_id,
    const agent_communication::ReplayQueryRequest* request,
    agent_communication::ReplayQueryResponse* response) {

    if (owner_id.empty()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "Valid authentication token required");
    }
    if (request->trace_id().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "trace_id is required");
    }

    const std::string& mode = request->mode();
    if (mode != "exact" && mode != "route") {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "mode must be 'exact' or 'route'");
    }

    const std::string& trace_id = request->trace_id();

    // PostgreSQL is the source of truth. The lookup is scoped by the
    // authenticated owner, so a foreign or unknown trace is NOT_FOUND —
    // never an existence leak.
    // deep-review proto-R3: traces are stored as "trace-<request_id>" but
    // clients may send the bare id (GetTraceDetail tolerates both); accept
    // both spellings here too so cross-RPC behavior is symmetric.
    auto trace = repository->getTraceById(owner_id, trace_id);
    if (!trace.has_value()) {
        trace = repository->getTraceById(owner_id, "trace-" + trace_id);
    }
    if (!trace.has_value()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "Trace not found: " + trace_id);
    }
    const auto query_log = repository->getQueryLogById(owner_id, trace->query_log_id);
    if (!query_log.has_value()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "Query log not found for trace: " + trace_id);
    }

    auto* status = response->mutable_status();
    status->set_code(0);
    status->set_message("OK");

    if (mode == "route") {
        // Route-only replay: compare the stored route decision against the
        // route the pipeline would pick now. Nothing is executed and nothing
        // is persisted.
        if (!route_provider) {
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                "Route provider is not configured");
        }
        const std::string original_route = routeLabelFrom(query_log->route_decision);
        const std::string replayed_route = route_provider();

        response->set_original(original_route);
        response->set_replayed(replayed_route);
        // Route mode performs no execution: the new-record fields stay empty.
        LOG_INFO("ReplayQuery route comparison for " + trace_id + ": " +
                 original_route + " -> " + replayed_route);
        return grpc::Status::OK;
    }

    // mode == "exact": re-execute through the durable pipeline under a new
    // request id. The original query_log/trace rows are never touched.
    if (!executor) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "Replay executor is not configured");
    }

    const std::string new_request_id = generateReplayRequestId();
    std::string answer;
    std::string error;
    if (!executor(new_request_id, query_log->conversation_id,
                  query_log->request_text, answer, error)) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "Replay execution failed: " + error);
    }

    const std::string new_trace_id = "trace-" + new_request_id;

    // Link the new trace back to the original one inside its payload so the
    // association is queryable (JSONB containment on traces.trace_payload).
    const auto new_trace = repository->getTraceById(owner_id, new_trace_id);
    if (!new_trace.has_value()) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "Replay pipeline did not persist trace " + new_trace_id);
    }
    auto payload = nlohmann::json::parse(new_trace->trace_payload, nullptr, false);
    if (payload.is_discarded() || !payload.is_object()) {
        payload = nlohmann::json::object();
    }
    payload["replayed_from_trace"] = trace_id;
    payload["replayed_from_request"] = query_log->id;
    // The new trace has already finalized, so the terminal-guarded
    // updateTrace would drop this linkage — merge it payload-only.
    if (!repository->updateTracePayload(owner_id, new_trace_id, payload.dump())) {
        LOG_WARN("Replay trace linkage failed for " + new_trace_id);
    }

    response->set_original(query_log->response_text);
    response->set_replayed(answer);
    response->set_new_trace_id(new_trace_id);
    response->set_new_request_id(new_request_id);

    LOG_INFO("ReplayQuery exact completed: " + trace_id + " -> " + new_trace_id);
    return grpc::Status::OK;
}

} // namespace

grpc::Status ReplayService::handleReplayRequest(
    const std::string& owner_id,
    const agent_communication::ReplayQueryRequest* request,
    agent_communication::ReplayQueryResponse* response) {

    if (!request || !response) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Invalid request or response");
    }
    // Snapshot the injected dependencies under the service lock.
    auto& service = instance();
    common::QueryDomainRepository* repository;
    RouteProvider route_provider;
    PipelineExecutor executor;
    {
        std::lock_guard<std::mutex> lock(service.mutex_);
        repository = service.domain_repository_;
        route_provider = service.route_provider_;
        executor = service.executor_;
    }
    if (!repository) {
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "Replay service is not configured");
    }
    // Top-level guard: PostgreSQL faults must surface as UNAVAILABLE (the
    // Query pipeline convention), never escape the gRPC handler.
    try {
        return handleReplayRequestImpl(repository, route_provider, executor,
                                       owner_id, request, response);
    } catch (const std::exception& error) {
        const bool persistence_fault = common::isPostgresError(error);
        LOG_ERROR(std::string("ReplayQuery failed: ") + error.what());
        // The client-facing status message is a fixed, sanitized
        // text. Internal exception detail stays in the server log only and
        // never rides the response (no cross-tenant information leak).
        return grpc::Status(
            persistence_fault ? grpc::StatusCode::UNAVAILABLE
                              : grpc::StatusCode::INTERNAL,
            persistence_fault ? "Replay unavailable: persistence layer error"
                              : "Replay failed unexpectedly");
    }
}

} // namespace orchestrator
} // namespace agent_rpc
