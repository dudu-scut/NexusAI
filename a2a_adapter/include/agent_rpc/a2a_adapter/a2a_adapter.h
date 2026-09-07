/**
 * @file a2a_adapter.h
 * @brief Main A2A adapter class
 */

#pragma once

#include "a2a_config.h"
#include "request_adapter.h"
#include "response_adapter.h"
#include "error_mapper.h"

#include <a2a/client/a2a_client.hpp>
#include <memory>
#include <functional>
#include <atomic>

// Forward declarations
namespace agent_communication {
class AIQueryRequest;
class AIQueryResponse;
class AIStreamEvent;
}
namespace agent_rpc { namespace common { class RedisClient; } }

namespace agent_rpc {
namespace a2a_adapter {

/**
 * @brief Main adapter class bridging RPC and A2A protocol
 */
class A2AAdapter {
public:
    A2AAdapter();
    ~A2AAdapter();
    
    /**
     * @brief Initialize the adapter with configuration
     * @param config The A2A configuration
     * @return true if initialization successful
     */
    bool initialize(const A2AConfig& config);
    
    /**
     * @brief Shutdown the adapter
     */
    void shutdown();
    
    /**
     * @brief Process a synchronous AI query
     * @param request The RPC request
     * @param response The RPC response to populate
     * @param abort_flag Optional P24 abort flag (from InFlightAbortRegistry);
     *                   when set, the HTTP transfer is interruptible via the
     *                   libcurl progress callback
     * @return true if query successful
     */
    bool processQuery(
        const agent_communication::AIQueryRequest& request,
        agent_communication::AIQueryResponse* response,
        std::shared_ptr<std::atomic<bool>> abort_flag = nullptr);

    /**
     * @brief Process an asynchronous AI query
     * @param request The RPC request
     * @param callback Callback to invoke with response
     */
    void processQueryAsync(
        const agent_communication::AIQueryRequest& request,
        std::function<void(const agent_communication::AIQueryResponse&)> callback);

    /**
     * @brief Process a streaming AI query
     * @param request The RPC request
     * @param callback Callback to invoke for each stream event
     * @param abort_flag Optional P24 abort flag (see processQuery)
     */
    void processQueryStreaming(
        const agent_communication::AIQueryRequest& request,
        std::function<void(const agent_communication::AIStreamEvent&)> callback,
        std::shared_ptr<std::atomic<bool>> abort_flag = nullptr);

    /**
     * @brief Process a sync query using a pre-resolved agent URL (bypasses routing)
     * @param request The RPC request
     * @param response The RPC response to populate
     * @param agent_url Pre-resolved agent URL
     * @param abort_flag Optional P24 abort flag (see processQuery)
     * @return true if query successful
     */
    bool processQueryDirect(
        const agent_communication::AIQueryRequest& request,
        agent_communication::AIQueryResponse* response,
        const std::string& agent_url,
        std::shared_ptr<std::atomic<bool>> abort_flag = nullptr);

    /**
     * @brief Process a streaming query using a pre-resolved agent URL (bypasses routing)
     * @param request The RPC request
     * @param callback Callback to invoke for each stream event
     * @param agent_url Pre-resolved agent URL
     * @param abort_flag Optional P24 abort flag (see processQuery)
     */
    void processQueryStreamingDirect(
        const agent_communication::AIQueryRequest& request,
        std::function<void(const agent_communication::AIStreamEvent&)> callback,
        const std::string& agent_url,
        std::shared_ptr<std::atomic<bool>> abort_flag = nullptr);

    /**
     * @brief Check if the adapter is available
     * @return true if adapter is initialized and ready
     */
    bool isAvailable() const;

    /**
     * @brief Cancel an in-flight task
     * @param task_id Task identifier to cancel
     * @return true if cancellation request was sent successfully
     */
    bool cancelTask(const std::string& task_id);

    /**
     * @brief Set per-request timeout
     *
     * Call before processQuery() to override the default timeout
     * for the next request. Useful for propagating gRPC deadlines.
     *
     * @param seconds Timeout in seconds
     */
    /**
     * @brief Set per-request timeout override (seconds).
     *
     * deep-review ad-R1: stored in a thread-local slot, NOT a shared member
     * — a shared atomic written by every request thread and read by a
     * different one would leak one request's gRPC deadline into another
     * request's curl timeout (deadline crosstalk under concurrency). The
     * caller sets the value on ITS OWN thread right before invoking the
     * adapter, and the adapter reads it on the same thread.
     */
    void setRequestTimeout(long seconds);

    /**
     * @brief Get the current configuration
     * @return Current A2A configuration
     */
    const A2AConfig& getConfig() const { return config_; }
    
    /**
     * @brief Get the request adapter
     */
    RequestAdapter& getRequestAdapter() { return *request_adapter_; }
    
    /**
     * @brief Get the response adapter
     */
    ResponseAdapter& getResponseAdapter() { return *response_adapter_; }

    /**
     * @brief Attach a shared Redis client (trace activity streams).
     *
     * The former autonomy-level lookups (Redis key autonomy:<user>:<agent>)
     * were removed with PR-E: autonomy levels now live in PostgreSQL
     * (autonomy_settings) and are enforced by the durable services, not by
     * this adapter.
     */
    void setRedisClient(std::shared_ptr<common::RedisClient> redis);

    /**
     * @brief Check whether user intervention is needed before
     *        executing a high-impact operation.
     *
     * Evaluates the action type against configured thresholds:
     *   - write operations:            intervene above kInterventionTokenThresholdWrite
     *   - high-token-cost operations:  intervene above kInterventionTokenThresholdHighCost
     *
     * @param action_type       "write", "high_cost_llm", "default"
     * @param estimated_tokens  Estimated token cost of the operation
     * @param confidence        Model confidence (0.0–1.0); lower → more likely to intervene
     * @return true if the operation should pause for user confirmation
     */
    bool shouldIntervene(const std::string& action_type,
                         long estimated_tokens = 0,
                         double confidence = 1.0) const;

private:
    // deep-review ad-R1: the per-request timeout override lives in a
    // thread-local slot (see setRequestTimeout) so concurrent gRPC handlers
    // cannot leak one request's gRPC deadline into another request's curl
    // timeout. Config.defaultTimeout() below stays the fallback for calls
    // that never set an override on this thread.
    static thread_local long request_timeout_seconds_;
    std::unique_ptr<RequestAdapter> request_adapter_;
    std::unique_ptr<ResponseAdapter> response_adapter_;
    A2AConfig config_;
    std::atomic<bool> initialized_{false};
    std::shared_ptr<common::RedisClient> redis_;
};

} // namespace a2a_adapter
} // namespace agent_rpc
