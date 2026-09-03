/**
 * @file a2a_adapter.cpp
 * @brief Implementation of main A2A adapter
 */

#include "agent_rpc/a2a_adapter/a2a_adapter.h"
#include "agent_rpc/a2a_adapter/error_mapper.h"
#include "agent_rpc/a2a_adapter/url_validation.h"
#include "agent_rpc/common/circuit_breaker.h"
#include "agent_rpc/common/trace_context.h"
#include "agent_rpc/common/redis_client.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/registry/service_registry.h"
#include "ai_query.pb.h"
#include <a2a/core/exception.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>

namespace agent_rpc {
namespace a2a_adapter {

using json = nlohmann::json;

A2AAdapter::A2AAdapter()
    : request_adapter_(std::make_unique<RequestAdapter>())
    , response_adapter_(std::make_unique<ResponseAdapter>()) {
}

A2AAdapter::~A2AAdapter() {
    shutdown();
}

bool A2AAdapter::initialize(const A2AConfig& config) {
    if (initialized_) {
        return true;
    }
    
    // Validate and store configuration
    config_ = config;
    if (!config_.validate()) {
        LOG_WARN("A2A configuration had invalid values, defaults were applied");
    }
    
    // Create A2A client
    try {
        a2a_client_ = std::make_unique<a2a::A2AClient>(config_.orchestrator_url);
        a2a_client_->set_timeout(config_.request_timeout_seconds);
        initialized_ = true;
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to create A2A client for orchestrator at " + config_.orchestrator_url + ": " + e.what());
        return false;
    }
}

void A2AAdapter::shutdown() {
    if (!initialized_) {
        return;
    }
    
    a2a_client_.reset();
    initialized_ = false;
}

bool A2AAdapter::processQuery(
    const agent_communication::AIQueryRequest& request,
    agent_communication::AIQueryResponse* response) {
    
    if (!response) {
        return false;
    }
    
    if (!initialized_) {
        auto* status = response->mutable_status();
        status->set_code(-1);
        status->set_message("A2A adapter not initialized");
        return false;
    }
    
    if (!a2a_client_) {
        auto* status = response->mutable_status();
        status->set_code(-1);
        status->set_message("A2A client not available");
        return false;
    }
    
    auto start_time = std::chrono::steady_clock::now();

    // Circuit breaker: check if orchestrator is healthy before attempting call
    auto cb = common::CircuitBreakerManager::getInstance().getCircuitBreaker("a2a_orchestrator");

    try {
        // Convert RPC request to A2A format
        a2a::MessageSendParams params = request_adapter_->convertToA2A(request);

        // Check circuit breaker before making the call
        if (!cb->isRequestAllowed()) {
            auto* status = response->mutable_status();
            status->set_code(static_cast<int>(grpc::StatusCode::UNAVAILABLE));
            status->set_message("Circuit breaker is OPEN — orchestrator is unavailable");
            return false;
        }

        // Inject trace headers into A2A HTTP call
        auto* trace = agent_rpc::common::TraceContext::current();
        if (trace) {
            trace->startSpan("agent_call", "a2a_adapter");

            // Delegation depth limit check
            constexpr int MAX_DEPTH = 5;
            // Use depth() counter as primary. Safety net: count agent_call spans
            // in completedSpans() as fallback when depth counter is unset (0).
            int depth = trace->depth();
            if (depth == 0) {
                int span_count = 0;
                for (const auto& s : trace->completedSpans()) {
                    if (s.name.rfind("agent_call", 0) == 0) {
                        span_count++;
                    }
                }
                depth = span_count;
            }
            if (depth >= MAX_DEPTH) {
                auto* status = response->mutable_status();
                status->set_code(static_cast<int>(grpc::StatusCode::FAILED_PRECONDITION));
                status->set_message("Delegation depth exceeded (max " +
                                     std::to_string(MAX_DEPTH) + ")");
                trace->endSpan();
                return false;
            }
            trace->incrementDepth();

            a2a_client_->add_header("x-trace-id", trace->traceId());
            a2a_client_->add_header("x-delegation-depth", std::to_string(depth + 1));
        }

        // Autonomy-level header removed: the old Redis key
        // autonomy:<user>:<agent> was replaced by PostgreSQL autonomy_settings;
        // this header had no consumer anywhere and its owner came from the
        // request body. To restore it, read via QueryDomainRepository::
        // getAutonomySetting(owner, agent_id) with the authenticated owner.

        // Send message via A2A client with retry for transient network errors
        int max_retries = config_.max_retries > 0 ? config_.max_retries : 1;
        int retry_delay = config_.retry_delay_ms > 0 ? config_.retry_delay_ms : 1000;
        std::string last_error;

        for (int attempt = 0; attempt < max_retries; ++attempt) {
            try {
                a2a::A2AResponse a2a_response = a2a_client_->send_message(params);

                // Convert A2A response to RPC format FIRST — if this throws,
                // we haven't corrupted trace/CB state yet. Only on success do
                // we finalize the trace, clear headers, and record success.
                response_adapter_->convertFromA2A(a2a_response, request.request_id(), "", response);

                // Record agent call result (only after successful conversion)
                if (trace) {
                    trace->endSpan();
                    a2a_client_->clear_headers();
                }

                // Record success
                cb->recordSuccess();

                // Calculate processing time
                auto end_time = std::chrono::steady_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    end_time - start_time);
                response->set_processing_time_ms(duration.count());

                // Record agent call for health dashboard metrics
                agent_rpc::registry::ServiceRegistry::recordAgentCall(
                    "orchestrator", true, static_cast<double>(duration.count()));

                // Success if we got any valid response (Task or Message)
                return true;

            } catch (const a2a::A2AException& e) {
                // Protocol errors are not transient — do not retry.
                // Re-throw so the outer A2AException handler applies the
                // correct ErrorMapper::mapToGrpcStatus for the error code.
                LOG_ERROR("A2A protocol error calling orchestrator: " + std::string(e.what()));
                throw;
            } catch (const std::exception& e) {
                last_error = e.what();
                if (attempt < max_retries - 1) {
                    LOG_WARN("A2A call attempt " + std::to_string(attempt + 1) + "/" +
                             std::to_string(max_retries) + " failed: " + last_error +
                             " — retrying in " + std::to_string(retry_delay) + "ms");
                    std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay));
                }
            }
        }

        // All retries exhausted, fall through to error handling below
        LOG_ERROR("A2A call failed after " + std::to_string(max_retries) +
                  " attempt(s): " + last_error);
        throw std::runtime_error(last_error);

    } catch (const a2a::A2AException& e) {
        LOG_ERROR("A2A protocol error calling orchestrator: " + std::string(e.what()));
        // Record failure to circuit breaker
        cb->recordFailure();
        // Record failure for health dashboard
        auto fail_end = std::chrono::steady_clock::now();
        auto fail_dur = std::chrono::duration_cast<std::chrono::milliseconds>(fail_end - start_time);
        agent_rpc::registry::ServiceRegistry::recordAgentCall(
            "orchestrator", false, static_cast<double>(fail_dur.count()));
        // Handle A2A protocol errors via ErrorMapper
        auto* status = response->mutable_status();
        grpc::StatusCode grpc_code = ErrorMapper::mapToGrpcStatus(
            static_cast<a2a::ErrorCode>(e.error_code()));
        status->set_code(static_cast<int>(grpc_code));
        std::string error_msg = e.what();
        if (error_msg.empty()) {
            error_msg = ErrorMapper::getErrorDescription(
                static_cast<a2a::ErrorCode>(e.error_code()));
        }
        status->set_message(error_msg);
        return false;
    } catch (const std::exception& e) {
        LOG_ERROR("Network/general error calling orchestrator: " + std::string(e.what()));
        // Record failure to circuit breaker
        cb->recordFailure();
        // Record failure for health dashboard
        auto fail_end2 = std::chrono::steady_clock::now();
        auto fail_dur2 = std::chrono::duration_cast<std::chrono::milliseconds>(fail_end2 - start_time);
        agent_rpc::registry::ServiceRegistry::recordAgentCall(
            "orchestrator", false, static_cast<double>(fail_dur2.count()));
        // Handle network and general errors via ErrorMapper
        auto* status = response->mutable_status();
        grpc::StatusCode grpc_code = ErrorMapper::mapNetworkException(e);
        status->set_code(static_cast<int>(grpc_code));
        std::string error_msg = e.what();
        if (error_msg.empty()) {
            error_msg = "Unknown error occurred while processing query";
        }
        status->set_message(error_msg);
        return false;
    }
}

void A2AAdapter::processQueryAsync(
    const agent_communication::AIQueryRequest& request,
    std::function<void(const agent_communication::AIQueryResponse&)> callback) {
    
    if (!initialized_ || !callback) {
        return;
    }
    
    // True asynchronous A2A submission (submit now, poll/fetch the
    // result later) is outside the local delivery boundary. This entry point
    // stays a synchronous fallback so callers never receive a fake task id or
    // an untracked background job; QueryStream is the supported streaming
    // path.
    agent_communication::AIQueryResponse response;
    processQuery(request, &response);
    callback(response);
}

void A2AAdapter::processQueryStreaming(
    const agent_communication::AIQueryRequest& request,
    std::function<void(const agent_communication::AIStreamEvent&)> callback) {
    
    if (!initialized_ || !callback || !config_.enable_streaming) {
        return;
    }
    
    // Circuit breaker: check if orchestrator is healthy before streaming call
    auto streaming_cb = common::CircuitBreakerManager::getInstance().getCircuitBreaker("a2a_orchestrator");
    if (!streaming_cb->isRequestAllowed()) {
        agent_communication::AIStreamEvent cb_event;
        response_adapter_->buildStreamEvent(
            "Circuit breaker is OPEN — orchestrator is unavailable",
            request.context_id(), "error", &cb_event);
        callback(cb_event);
        // Note: Do NOT call endSpan() here — no startSpan() was called
        // in this function yet. The caller manages its own span.
        return;
    }

    try {
        // Convert RPC request to A2A format
        a2a::MessageSendParams params = request_adapter_->convertToA2A(request);
        std::string context_id = params.context_id().value_or("");

        // Use streaming API
        // Note: http_client splits on double newlines, so each callback
        // receives a complete SSE event
        // Inject trace headers into A2A HTTP streaming call
        auto* trace = agent_rpc::common::TraceContext::current();
        std::string trace_id;
        if (trace) {
            trace->startSpan("agent_call_streaming", "a2a_adapter");

            // Delegation depth limit check for streaming
            constexpr int MAX_DEPTH = 5;
            int depth = trace->depth();
            if (depth >= MAX_DEPTH) {
                agent_communication::AIStreamEvent depth_event;
                response_adapter_->buildStreamEvent(
                    "Delegation depth exceeded (max " + std::to_string(MAX_DEPTH) + ")",
                    request.context_id(), "error", &depth_event);
                callback(depth_event);
                trace->endSpan();
                return;
            }
            trace->incrementDepth();

            trace_id = trace->traceId();
            a2a_client_->add_header("x-trace-id", trace_id);
            a2a_client_->add_header("x-delegation-depth", std::to_string(depth + 1));
        }

        // Autonomy-level header removed (see processQuery).

        a2a_client_->send_message_streaming(params,
            [this, &callback, &context_id, trace_id](const std::string& event_line) {
                // Skip empty lines
                if (event_line.empty() || event_line == "\n" || event_line == "\r\n") {
                    return;
                }
                
                // Parse SSE format: "data: {...}\n" or "data: {...}"
                std::string event_data = event_line;
                
                // Strip trailing newlines
                while (!event_data.empty() && 
                       (event_data.back() == '\n' || event_data.back() == '\r')) {
                    event_data.pop_back();
                }
                
                // Extract the content after "data: "
                const std::string data_prefix = "data: ";
                if (event_data.find(data_prefix) == 0) {
                    event_data = event_data.substr(data_prefix.length());
                }
                
                // Skip empty data
                if (event_data.empty()) {
                    return;
                }
                
                // Parse JSON; catch all JSON exceptions (including UTF-8 errors)
                json j;
                try {
                    j = json::parse(event_data);
                } catch (const json::exception& e) {
                    // JSON parse failed (including UTF-8 errors); skip this event
                    return;
                }
                
                try {
                    // Check for errors
                    if (j.contains("error")) {
                        agent_communication::AIStreamEvent event;
                        std::string error_msg = j["error"].value("message", "Unknown error");
                        response_adapter_->buildStreamEvent(
                            error_msg, context_id, "error", &event);
                        callback(event);
                        return;
                    }
                    
                    // Check for a result
                    if (j.contains("result")) {
                        auto& result = j["result"];
                        std::string type = result.value("type", "");
                        
                        if (type == "chunk") {
                            // Streaming content chunk
                            std::string content = result.value("content", "");
                            agent_communication::AIStreamEvent event;
                            response_adapter_->buildStreamEvent(
                                content, context_id, "partial", &event);
                            callback(event);
                        } else if (type == "stream_start") {
                            // Stream start event
                            agent_communication::AIStreamEvent event;
                            response_adapter_->buildStreamEvent(
                                "", context_id, "status", &event);
                            event.set_task_state("processing");
                            callback(event);
                        } else if (type == "stream_end") {
                            // Stream end event — do not send "complete" here; the outer layer handles it
                        } else if (type == "intent") {
                            // Intent classification event
                            agent_communication::AIStreamEvent event;
                            std::string intent = result.value("intent", "");
                            response_adapter_->buildStreamEvent(
                                "Intent: " + intent, context_id, "status", &event);
                            callback(event);
                        } else if (type == "status") {
                            // Standard A2A status event
                            if (result.contains("status")) {
                                auto& status_obj = result["status"];
                                std::string state = status_obj.value("state", "");

                                // Write activity feed record
                                if (!trace_id.empty() && redis_) {
                                    try {
                                        nlohmann::json activity;
                                        activity["t"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::system_clock::now().time_since_epoch()).count();
                                        activity["status"] = state;
                                        activity["desc"] = status_obj.value("status_description",
                                            state == "working" ? "processing" : "completed");
                                        std::string act_key = "activity_feed:" + trace_id;
                                        redis_->rpush(act_key, activity.dump());
                                        redis_->expire(act_key, 3600);
                                        redis_->ltrim(act_key, -50, -1);
                                    } catch (...) {
                                        // Swallow — activity feed is non-critical
                                    }
                                }

                                if (state == "working") {
                                    agent_communication::AIStreamEvent event;
                                    response_adapter_->buildStreamEvent(
                                        "", context_id, "status", &event);
                                    event.set_task_state("processing");
                                    callback(event);
                                } else if (state == "completed") {
                                    // Extract text content from the completion message
                                    if (status_obj.contains("message")) {
                                        auto& message = status_obj["message"];
                                        if (message.contains("parts")) {
                                            std::string content;
                                            for (auto& part : message["parts"]) {
                                                if (part.value("type", "") == "text" || part.value("kind", "") == "text") {
                                                    content += part.value("text", "");
                                                }
                                            }
                                            if (!content.empty()) {
                                                agent_communication::AIStreamEvent event;
                                                response_adapter_->buildStreamEvent(
                                                    content, context_id, "partial", &event);
                                                callback(event);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    // Error while processing the result; skip
                }
            });

        // End streaming trace span
        if (trace) {
            trace->endSpan();
            a2a_client_->clear_headers();
        }

        // Record streaming success to circuit breaker
        streaming_cb->recordSuccess();

        // Send completion event
        agent_communication::AIStreamEvent complete_event;
        response_adapter_->buildStreamEvent(
            "", context_id, "complete", &complete_event);
        callback(complete_event);

    } catch (const std::exception& e) {
        // Record streaming failure to circuit breaker
        streaming_cb->recordFailure();

        // Send error event
        agent_communication::AIStreamEvent error_event;
        response_adapter_->buildStreamEvent(
            e.what(), request.context_id(), "error", &error_event);
        callback(error_event);
    }
}

void A2AAdapter::setRedisClient(std::shared_ptr<common::RedisClient> redis) {
    redis_ = std::move(redis);
}

bool A2AAdapter::cancelTask(const std::string& task_id) {
    if (!initialized_ || !a2a_client_ || task_id.empty()) {
        return false;
    }

    try {
        a2a_client_->cancel_task(task_id);
        return true;
    } catch (...) {
        return false;
    }
}

void A2AAdapter::setRequestTimeout(long seconds) {
    if (a2a_client_ && seconds > 0) {
        a2a_client_->set_timeout(seconds);
    }
}

bool A2AAdapter::isAvailable() const {
    return initialized_;
}

bool A2AAdapter::processQueryDirect(
    const agent_communication::AIQueryRequest& request,
    agent_communication::AIQueryResponse* response,
    const std::string& agent_url) {

    if (!response || !initialized_) {
        if (response) {
            auto* status = response->mutable_status();
            status->set_code(-1);
            status->set_message("A2A adapter not initialized");
        }
        return false;
    }

    // P21 L1: validate agent URL to prevent SSRF. Real parse + strict
    // http/https whitelist + userinfo/ambiguity-byte rejection (L2/L3 host
    // and port layers activate under NEXUSAI_SSRF_STRICT=1).
    std::string url_err;
    if (!validateAgentUrl(agent_url, url_err)) {
        if (response) {
            auto* status = response->mutable_status();
            status->set_code(static_cast<int>(grpc::StatusCode::INVALID_ARGUMENT));
            status->set_message(url_err);
        }
        return false;
    }

    auto start_time = std::chrono::steady_clock::now();

    // Circuit breaker: check if the target agent is healthy
    auto direct_cb = common::CircuitBreakerManager::getInstance().getCircuitBreaker("direct_agent:" + agent_url);

    try {
        a2a::MessageSendParams params = request_adapter_->convertToA2A(request);

        // Check circuit breaker before making the direct call
        if (!direct_cb->isRequestAllowed()) {
            if (response) {
                auto* status = response->mutable_status();
                status->set_code(static_cast<int>(grpc::StatusCode::UNAVAILABLE));
                status->set_message("Circuit breaker is OPEN — agent " + agent_url + " is unavailable");
            }
            return false;
        }

        a2a::A2AClient client(agent_url);
        client.set_timeout(config_.request_timeout_seconds);

        // P21 L2 (strict mode): resolve the host, reject blacklisted
        // addresses, and pin the validated IPs to the connection
        // (anti-rebinding).
        std::string host;
        std::string port_str;
        if (ssrfStrictModeEnabled() &&
            splitAgentUrlHostPort(agent_url, host, port_str)) {
            std::vector<std::string> ips;
            std::string host_err;
            if (!validateResolvedHost(host, ips, host_err)) {
                if (response) {
                    auto* status = response->mutable_status();
                    status->set_code(static_cast<int>(grpc::StatusCode::INVALID_ARGUMENT));
                    status->set_message(host_err);
                }
                return false;
            }
            const bool https = agent_url.compare(0, 8, "https://") == 0;
            const std::string pin_port =
                port_str.empty() ? (https ? "443" : "80") : port_str;
            std::vector<std::string> resolve_entries;
            resolve_entries.reserve(ips.size());
            for (const auto& ip : ips) {
                resolve_entries.push_back(host + ":" + pin_port + ":" + ip);
            }
            client.set_resolve_entries(resolve_entries);
        }

        // Inject trace headers into direct A2A HTTP call
        auto* trace = agent_rpc::common::TraceContext::current();
        if (trace) {
            trace->startSpan("agent_call_direct", "a2a_adapter");

            // Delegation depth limit check for direct calls
            constexpr int MAX_DEPTH = 5;
            int depth = trace->depth();
            if (depth >= MAX_DEPTH) {
                if (response) {
                    auto* status = response->mutable_status();
                    status->set_code(static_cast<int>(grpc::StatusCode::FAILED_PRECONDITION));
                    status->set_message("Delegation depth exceeded (max " +
                                         std::to_string(MAX_DEPTH) + ")");
                }
                trace->endSpan();
                return false;
            }
            trace->incrementDepth();

            client.add_header("x-trace-id", trace->traceId());
            client.add_header("x-delegation-depth", std::to_string(depth + 1));
        }

        // Autonomy-level header removed (see processQuery).

        a2a::A2AResponse a2a_response = client.send_message(params);

        // End trace span
        if (trace) {
            trace->endSpan();
        }

        response_adapter_->convertFromA2A(a2a_response, request.request_id(), "", response);

        // Record direct call success to circuit breaker
        direct_cb->recordSuccess();

        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            end_time - start_time);
        response->set_processing_time_ms(duration.count());
        return true;

    } catch (const a2a::A2AException& e) {
        // Record failure to circuit breaker
        direct_cb->recordFailure();
        auto* status = response->mutable_status();
        grpc::StatusCode grpc_code = ErrorMapper::mapToGrpcStatus(
            static_cast<a2a::ErrorCode>(e.error_code()));
        status->set_code(static_cast<int>(grpc_code));
        std::string error_msg = e.what();
        if (error_msg.empty()) {
            error_msg = ErrorMapper::getErrorDescription(
                static_cast<a2a::ErrorCode>(e.error_code()));
        }
        status->set_message(error_msg);
        return false;
    } catch (const std::exception& e) {
        // Record failure to circuit breaker
        direct_cb->recordFailure();
        auto* status = response->mutable_status();
        grpc::StatusCode grpc_code = ErrorMapper::mapNetworkException(e);
        status->set_code(static_cast<int>(grpc_code));
        status->set_message(e.what());
        return false;
    }
}

void A2AAdapter::processQueryStreamingDirect(
    const agent_communication::AIQueryRequest& request,
    std::function<void(const agent_communication::AIStreamEvent&)> callback,
    const std::string& agent_url) {

    if (!initialized_ || !callback) {
        return;
    }

    // P21 L1: validate agent URL (same SSRF protection as processQueryDirect)
    std::string url_err;
    if (!validateAgentUrl(agent_url, url_err)) {
        agent_communication::AIStreamEvent cb_event;
        response_adapter_->buildStreamEvent(
            "Invalid agent URL — " + url_err,
            request.context_id(), "error", &cb_event);
        callback(cb_event);
        return;
    }

    // P21 L2 (strict mode): reject blacklisted hosts on the streaming
    // direct path as well. The streaming HTTP path has no RESOLVE pin
    // channel, so this is validate-only (known limitation).
    std::string host;
    std::string port_str;
    if (ssrfStrictModeEnabled() &&
        splitAgentUrlHostPort(agent_url, host, port_str)) {
        std::vector<std::string> ips;
        std::string host_err;
        if (!validateResolvedHost(host, ips, host_err)) {
            agent_communication::AIStreamEvent cb_event;
            response_adapter_->buildStreamEvent(
                "Invalid agent URL — " + host_err,
                request.context_id(), "error", &cb_event);
            callback(cb_event);
            return;
        }
    }

    // Circuit breaker: check if the target agent is healthy for streaming direct
    auto streaming_direct_cb = common::CircuitBreakerManager::getInstance().getCircuitBreaker("streaming_direct:" + agent_url);
    if (!streaming_direct_cb->isRequestAllowed()) {
        agent_communication::AIStreamEvent cb_event;
        response_adapter_->buildStreamEvent(
            "Circuit breaker is OPEN — agent " + agent_url + " is unavailable for streaming",
            request.context_id(), "error", &cb_event);
        callback(cb_event);
        return;
    }

    try {
        a2a::MessageSendParams params = request_adapter_->convertToA2A(request);
        std::string context_id = params.context_id().value_or("");

        a2a::A2AClient client(agent_url);
        client.set_timeout(config_.request_timeout_seconds);

        // Inject trace headers into direct A2A HTTP streaming call
        auto* trace = agent_rpc::common::TraceContext::current();
        std::string trace_id;
        if (trace) {
            trace->startSpan("agent_call_streaming_direct", "a2a_adapter");

            // Delegation depth limit check for streaming direct
            constexpr int MAX_DEPTH = 5;
            int depth = trace->depth();
            if (depth >= MAX_DEPTH) {
                agent_communication::AIStreamEvent depth_event;
                response_adapter_->buildStreamEvent(
                    "Delegation depth exceeded (max " + std::to_string(MAX_DEPTH) + ")",
                    request.context_id(), "error", &depth_event);
                callback(depth_event);
                trace->endSpan();
                return;
            }
            trace->incrementDepth();

            trace_id = trace->traceId();
            client.add_header("x-trace-id", trace_id);
            client.add_header("x-delegation-depth", std::to_string(depth + 1));
        }

        // Autonomy-level header removed (see processQuery).

        client.send_message_streaming(params,
            [this, &callback, &context_id, trace_id](const std::string& event_line) {
                if (event_line.empty() || event_line == "\n" || event_line == "\r\n") {
                    return;
                }

                std::string event_data = event_line;
                while (!event_data.empty() &&
                       (event_data.back() == '\n' || event_data.back() == '\r')) {
                    event_data.pop_back();
                }

                const std::string data_prefix = "data: ";
                if (event_data.find(data_prefix) == 0) {
                    event_data = event_data.substr(data_prefix.length());
                }

                if (event_data.empty()) return;

                json j;
                try {
                    j = json::parse(event_data);
                } catch (const json::exception&) {
                    return;
                }

                try {
                    if (j.contains("error")) {
                        agent_communication::AIStreamEvent event;
                        std::string error_msg = j["error"].value("message", "Unknown error");
                        response_adapter_->buildStreamEvent(
                            error_msg, context_id, "error", &event);
                        callback(event);
                        return;
                    }

                    if (j.contains("result")) {
                        auto& result = j["result"];
                        std::string type = result.value("type", "");

                        if (type == "chunk") {
                            std::string content = result.value("content", "");
                            agent_communication::AIStreamEvent event;
                            response_adapter_->buildStreamEvent(
                                content, context_id, "partial", &event);
                            callback(event);
                        } else if (type == "stream_start") {
                            agent_communication::AIStreamEvent event;
                            response_adapter_->buildStreamEvent(
                                "", context_id, "status", &event);
                            event.set_task_state("processing");
                            callback(event);
                        } else if (type == "stream_end") {
                            // Completion handled by outer scope
                        } else if (type == "intent") {
                            agent_communication::AIStreamEvent event;
                            std::string intent = result.value("intent", "");
                            response_adapter_->buildStreamEvent(
                                "Intent: " + intent, context_id, "status", &event);
                            callback(event);
                        } else if (type == "status") {
                            // Standard A2A status event
                            if (result.contains("status")) {
                                auto& status_obj = result["status"];
                                std::string state = status_obj.value("state", "");

                                // Write activity feed record
                                if (!trace_id.empty() && redis_) {
                                    try {
                                        nlohmann::json activity;
                                        activity["t"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::system_clock::now().time_since_epoch()).count();
                                        activity["status"] = state;
                                        activity["desc"] = status_obj.value("status_description",
                                            state == "working" ? "processing" : "completed");
                                        std::string act_key = "activity_feed:" + trace_id;
                                        redis_->rpush(act_key, activity.dump());
                                        redis_->expire(act_key, 3600);
                                        redis_->ltrim(act_key, -50, -1);
                                    } catch (...) {
                                        // Swallow — activity feed is non-critical
                                    }
                                }

                                if (state == "working") {
                                    agent_communication::AIStreamEvent event;
                                    response_adapter_->buildStreamEvent(
                                        "", context_id, "status", &event);
                                    event.set_task_state("processing");
                                    callback(event);
                                } else if (state == "completed") {
                                    // Extract text content from the completion message
                                    if (status_obj.contains("message")) {
                                        auto& message = status_obj["message"];
                                        if (message.contains("parts")) {
                                            std::string content;
                                            for (auto& part : message["parts"]) {
                                                if (part.value("type", "") == "text" || part.value("kind", "") == "text") {
                                                    content += part.value("text", "");
                                                }
                                            }
                                            if (!content.empty()) {
                                                agent_communication::AIStreamEvent event;
                                                response_adapter_->buildStreamEvent(
                                                    content, context_id, "partial", &event);
                                                callback(event);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                } catch (const std::exception&) {
                    // Skip malformed events
                }
            });

        // End direct streaming trace span
        if (trace) {
            trace->endSpan();
        }

        // Record streaming direct success to circuit breaker
        streaming_direct_cb->recordSuccess();

        agent_communication::AIStreamEvent complete_event;
        response_adapter_->buildStreamEvent(
            "", context_id, "complete", &complete_event);
        callback(complete_event);

    } catch (const std::exception& e) {
        // Record streaming direct failure to circuit breaker
        streaming_direct_cb->recordFailure();

        agent_communication::AIStreamEvent error_event;
        response_adapter_->buildStreamEvent(
            e.what(), request.context_id(), "error", &error_event);
        callback(error_event);
    }
}

// Intervention detection

bool A2AAdapter::shouldIntervene(const std::string& action_type,
                                  long estimated_tokens,
                                  double confidence) const {
    // Thresholds for intervention
    constexpr long kHighCostThreshold = 8000;     // tokens
    constexpr long kWriteThreshold    = 4000;     // tokens (writes are riskier)
    constexpr double kLowConfidence   = 0.6;      // below this, intervene more readily

    // Determine effective token threshold based on action type
    long threshold = 0;
    if (action_type == "write") {
        threshold = kWriteThreshold;
    } else if (action_type == "high_cost_llm") {
        threshold = kHighCostThreshold;
    } else {
        // Default: no intervention for low-risk actions
        return false;
    }

    // Intervene if estimated cost exceeds threshold
    if (estimated_tokens >= threshold) {
        return true;
    }

    // Intervene if confidence is too low for a high-impact action
    if (action_type == "write" && confidence < kLowConfidence) {
        return true;
    }

    return false;
}

} // namespace a2a_adapter
} // namespace agent_rpc
