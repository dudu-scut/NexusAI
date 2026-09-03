/**
 * @file observability_service.cpp
 * @brief ObservabilityService implementation — GetTraceDetail & GetCostReport
 *
 * Data flow:
 *   - Trace payloads (including collected spans) live in PostgreSQL
 *     `traces.trace_payload` written by AIQueryServiceImpl's durable pipeline.
 *   - Daily costs are aggregated from `token_usage_ledger`.
 *   - The owner is ALWAYS the authenticated user; client-supplied ids are
 *     ignored. Redis is not consulted for observability reads anymore.
 */

#include "agent_rpc/server/observability_service.h"
#include "agent_rpc/server/auth_interceptor.h"
#include "agent_rpc/common/agent_runtime_repository.h"
#include "agent_rpc/common/query_domain_repository.h"
#include "agent_rpc/common/logger.h"

#include <nlohmann/json.hpp>

#include <ctime>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace agent_rpc {
namespace server {

ObservabilityServiceImpl::ObservabilityServiceImpl(common::RedisClient* redis_client)
    : redis_client_(redis_client) {
}

void ObservabilityServiceImpl::setAgentRuntimeRepository(
    common::AgentRuntimeRepository* repository) {
    runtime_repository_ = repository;
}

void ObservabilityServiceImpl::setQueryDomainRepository(
    common::QueryDomainRepository* repository) {
    query_repository_ = repository;
}

namespace {

// Parse "YYYY-MM-DD" into a std::tm (zero-padded). Returns false on failure.
bool parseDate(const std::string& date_str, std::tm& tm) {
    if (date_str.size() != 10) return false;
    std::memset(&tm, 0, sizeof(tm));
    std::istringstream iss(date_str);
    iss >> std::get_time(&tm, "%Y-%m-%d");
    return !iss.fail();
}

// Format a std::tm back to "YYYY-MM-DD".
std::string formatDate(const std::tm& tm) {
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d");
    return oss.str();
}

// Advance tm by one day (normalises via mktime).
void advanceDay(std::tm& tm) {
    tm.tm_mday += 1;
    std::mktime(&tm);  // normalise
}

// Count days between two tm values (inclusive of start, exclusive of end).
int daysBetween(const std::tm& from, const std::tm& to) {
    std::time_t t_from = std::mktime(const_cast<std::tm*>(&from));
    std::time_t t_to   = std::mktime(const_cast<std::tm*>(&to));
    return static_cast<int>((t_to - t_from) / 86400);
}

} // anonymous namespace

grpc::Status ObservabilityServiceImpl::buildRedisFallbackTrace(
    const std::string& redis_key,
    const std::string& trace_id,
    const std::string& owner,
    agent_communication::GetTraceDetailResponse* response) {
    (void)owner;  // ownership was already enforced before the fallback
    // Batch-flush Redis spans carry: trace_id/span_id/name/component/
    // duration_ms/status as one JSON string per list element.
    std::vector<std::string> raw_spans;
    if (!redis_client_->lrange(redis_key, 0, -1, raw_spans) || raw_spans.empty()) {
        auto* status = response->mutable_status();
        status->set_code(-1);
        status->set_message("Trace not found: " + trace_id);
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "Trace not found: " + trace_id);
    }

    std::ostringstream summary;
    bool first_span = true;
    for (const auto& raw : raw_spans) {
        try {
            const auto j = nlohmann::json::parse(raw);
            auto* span = response->add_spans();
            span->set_trace_id(trace_id);
            span->set_span_id(j.value("span_id", ""));
            span->set_parent_span_id("");
            std::string component = j.value("component", "");
            if (component.empty()) {
                component = j.value("name", "");
            }
            span->set_component(component);
            span->set_duration_ms(j.value("duration_ms", 0));
            span->set_status(j.value("status", "ok"));
            if (!first_span) summary << " \xe2\x86\x92 ";
            summary << span->component() << " " << span->duration_ms() << "ms";
            first_span = false;
        } catch (const nlohmann::json::exception& e) {
            LOG_WARN("Malformed Redis span for trace " + trace_id + ": " +
                     std::string(e.what()));
        }
    }

    response->set_trace_summary(summary.str());
    auto* status = response->mutable_status();
    status->set_code(0);
    status->set_message("OK");
    LOG_INFO("GetTraceDetail returned " +
             std::to_string(response->spans_size()) +
             " spans (Redis fallback) for trace: " + trace_id);
    return grpc::Status::OK;
}

grpc::Status ObservabilityServiceImpl::GetTraceDetail(
    grpc::ServerContext* context,
    const agent_communication::GetTraceDetailRequest* request,
    agent_communication::GetTraceDetailResponse* response) {

    (void)context;

    if (!request || !response) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Invalid request or response");
    }
    if (!AuthInterceptor::isAuthenticated()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "Valid authentication token required");
    }

    const std::string& trace_id = request->trace_id();
    if (trace_id.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "trace_id is required");
    }
    if (query_repository_ == nullptr) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                            "Trace storage not available");
    }

    // Owner comes exclusively from the auth context. Traces are stored as
    // "trace-<request_id>" but clients may send the bare id; try both.
    // Missing OR foreign traces both surface as NOT_FOUND so another user's
    // trace existence is never disclosed.
    const std::string owner = AuthInterceptor::currentUserId();
    std::optional<common::TraceRecord> trace =
        query_repository_->getTraceById(owner, trace_id);
    if (!trace.has_value()) {
        trace = query_repository_->getTraceById(owner, "trace-" + trace_id);
    }
    if (!trace.has_value()) {
        // P6 遗留收口：PG 无记录时回退读批量 flush 键（PG 主、Redis 兜底）。
        if (redis_client_ && redis_client_->isConnected()) {
            const std::string redis_key = "trace:spans:" + trace_id;
            if (redis_client_->exists(redis_key)) {
                return buildRedisFallbackTrace(redis_key, trace_id, owner,
                                               response);
            }
        }
        auto* status = response->mutable_status();
        status->set_code(-1);
        status->set_message("Trace not found: " + trace_id);
        return grpc::Status(grpc::StatusCode::NOT_FOUND,
                            "Trace not found: " + trace_id);
    }

    LOG_INFO("GetTraceDetail for trace: " + trace->id + " owner=" + owner);

    // trace_payload is a JSON object; the durable pipeline stores collected
    // spans under "spans" (fields: name/component/duration_ms/status).
    std::ostringstream summary;
    bool first_span = true;
    try {
        const auto payload = nlohmann::json::parse(trace->trace_payload);
        if (payload.contains("spans") && payload["spans"].is_array()) {
            int span_index = 0;
            for (const auto& j : payload["spans"]) {
                auto* span = response->add_spans();
                span->set_trace_id(trace->id);
                span->set_span_id(j.value("span_id", "span-" + std::to_string(span_index++)));
                span->set_parent_span_id(j.value("parent_span_id", ""));
                // Legacy Redis spans carried "component"; durable payload may
                // only carry "name" — fall back to it for readability.
                std::string component = j.value("component", "");
                if (component.empty()) {
                    component = j.value("name", "");
                }
                span->set_component(component);
                span->set_start_time(j.value("start_time", int64_t{0}));
                span->set_end_time(j.value("end_time", int64_t{0}));
                span->set_duration_ms(j.value("duration_ms", 0));
                span->set_status(j.value("status", "ok"));
                span->set_error_message(j.value("error_message", ""));
                span->set_metadata_json(j.value("metadata_json", ""));

                if (!first_span) summary << " \xe2\x86\x92 ";
                summary << span->component() << " " << span->duration_ms() << "ms";
                first_span = false;
            }
        }
    } catch (const nlohmann::json::exception& e) {
        LOG_WARN("Malformed trace payload for trace " + trace->id + ": " +
                 std::string(e.what()));
    }

    response->set_trace_summary(summary.str());

    auto* status = response->mutable_status();
    status->set_code(0);
    status->set_message("OK");

    LOG_INFO("GetTraceDetail returned " + std::to_string(response->spans_size()) +
             " spans for trace: " + trace->id);
    return grpc::Status::OK;
}

grpc::Status ObservabilityServiceImpl::GetCostReport(
    grpc::ServerContext* context,
    const agent_communication::GetCostReportRequest* request,
    agent_communication::GetCostReportResponse* response) {

    (void)context;

    if (!request || !response) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Invalid request or response");
    }
    if (!AuthInterceptor::isAuthenticated()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "Valid authentication token required");
    }

    // The report is always scoped to the authenticated owner; a user_id in
    // the request body is deliberately ignored (cross-owner cost reads are
    // forbidden by design).
    const std::string user_id = AuthInterceptor::currentUserId();

    // Parse date range
    std::tm tm_start{}, tm_end{};
    if (!parseDate(request->start_date(), tm_start) ||
        !parseDate(request->end_date(),   tm_end)) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Invalid date format; expected YYYY-MM-DD");
    }

    // Cap the range to 90 days to keep the aggregate scan bounded
    int range_days = daysBetween(tm_start, tm_end);
    if (range_days < 0) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "start_date must be before or equal to end_date");
    }
    if (range_days > 90) {
        // Clamp end_date to start_date + 90 days
        tm_end = tm_start;
        for (int i = 0; i < 90; ++i) advanceDay(tm_end);
        range_days = 90;
    }

    if (runtime_repository_ == nullptr) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                            "Cost storage not available");
    }

    LOG_INFO("GetCostReport for owner=" + user_id +
             " from=" + request->start_date() +
             " to=" + formatDate(tm_end) +
             " (" + std::to_string(range_days + 1) + " days)");

    double total_cost_usd = 0.0;
    try {
        for (const auto& day : runtime_repository_->dailyCostReport(
                 user_id, request->start_date(), formatDate(tm_end))) {
            auto* record = response->add_records();
            record->set_user_id(user_id);
            record->set_date(day.date);
            double cost_usd = 0.0;
            if (!day.cost_usd.empty()) {
                try {
                    cost_usd = std::stod(day.cost_usd);
                } catch (const std::exception&) {
                    cost_usd = 0.0;
                }
            }
            record->set_total_cost_usd(cost_usd);
            record->set_total_prompt_tokens(
                static_cast<int32_t>(day.prompt_tokens));
            record->set_total_completion_tokens(
                static_cast<int32_t>(day.completion_tokens));
            record->set_total_requests(static_cast<int32_t>(day.request_count));
            // estimated=true means the provider never reported real usage and
            // the numbers are local estimates — never present them as exact.
            record->set_estimated(day.estimated);
            total_cost_usd += cost_usd;
        }
    } catch (const std::exception& e) {
        LOG_WARN(std::string("GetCostReport aggregate failed: ") + e.what());
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "Failed to build cost report");
    }

    response->set_total_cost_usd(total_cost_usd);

    auto* status = response->mutable_status();
    status->set_code(0);
    status->set_message("OK");

    LOG_INFO("GetCostReport returned " + std::to_string(response->records_size()) +
             " records, total=$" + std::to_string(total_cost_usd));
    return grpc::Status::OK;
}

} // namespace server
} // namespace agent_rpc
