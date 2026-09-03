#pragma once

#include "../models/agent_task.hpp"
#include "../models/agent_message.hpp"
#include "../models/message_send_params.hpp"
#include "../models/a2a_response.hpp"
#include "../core/http_client.hpp"
#include <atomic>
#include <string>
#include <memory>
#include <functional>
#include <vector>

namespace a2a {

/**
 * @brief Main A2A Client for communicating with agents
 */
class A2AClient {
public:
    /**
     * @brief Construct client with agent base URL
     * @param base_url Base URL of the agent service
     */
    explicit A2AClient(const std::string& base_url);
    
    ~A2AClient();
    
    // Disable copy, enable move
    A2AClient(const A2AClient&) = delete;
    A2AClient& operator=(const A2AClient&) = delete;
    A2AClient(A2AClient&&) noexcept;
    A2AClient& operator=(A2AClient&&) noexcept;
    
    /**
     * @brief Send a non-streaming message request
     * @param params Message parameters
     * @return A2AResponse containing Task or Message
     * @throws A2AException on error
     */
    A2AResponse send_message(const MessageSendParams& params);
    
    /**
     * @brief Send a streaming message request
     * @param params Message parameters
     * @param callback Called for each event received (Task, Message, or status update)
     * @throws A2AException on error
     */
    void send_message_streaming(const MessageSendParams& params,
                               std::function<void(const std::string&)> callback);
    
    /**
     * @brief Get a task by ID
     * @param task_id Task identifier
     * @return AgentTask object
     * @throws A2AException if task not found
     */
    AgentTask get_task(const std::string& task_id);
    
    /**
     * @brief Cancel a task
     * @param task_id Task identifier
     * @return Updated AgentTask
     * @throws A2AException if task cannot be cancelled
     */
    AgentTask cancel_task(const std::string& task_id);
    
    /**
     * @brief Subscribe to task updates (streaming)
     * @param task_id Task identifier
     * @param callback Called for each event received
     * @throws A2AException on error
     */
    void subscribe_to_task(const std::string& task_id,
                          std::function<void(const std::string&)> callback);
    
    /**
     * @brief Set request timeout
     * @param seconds Timeout in seconds
     */
    void set_timeout(long seconds);

    /**
     * @brief Attach an abort flag checked during transfers (P20).
     * Forwarded to the underlying HttpClient; see HttpClient::set_abort_flag.
     * @param flag  Pointer to an external atomic abort flag (may be null)
     */
    void set_abort_flag(const std::atomic<bool>* flag);

    /**
     * @brief Pin host→IP mappings for the next transfers (P21 L2).
     * Forwarded to the underlying HttpClient; see
     * HttpClient::set_resolve_entries.
     * @param entries  "host:port:ip" strings, one per validated address
     */
    void set_resolve_entries(const std::vector<std::string>& entries);

    /**
     * @brief Add a custom HTTP header to all outgoing requests
     * @param key Header name
     * @param value Header value
     */
    void add_header(const std::string& key, const std::string& value);

    /**
     * @brief Clear all custom HTTP headers
     */
    void clear_headers();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace a2a
