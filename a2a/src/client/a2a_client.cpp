#include <a2a/client/a2a_client.hpp>
#include <a2a/core/jsonrpc_request.hpp>
#include <a2a/core/jsonrpc_response.hpp>
#include <a2a/core/a2a_methods.hpp>
#include <a2a/core/exception.hpp>
#include <atomic>
#include <sstream>

namespace a2a {

// Helper to generate UUID (simplified, thread-safe)
static std::string generate_uuid() {
    static std::atomic<uint64_t> counter{0};
    std::ostringstream oss;
    oss << "req-" << ++counter << "-" << std::time(nullptr);
    return oss.str();
}

// PIMPL implementation
class A2AClient::Impl {
public:
    explicit Impl(const std::string& base_url)
        : base_url_(base_url)
        , http_client_() {
        
        if (!base_url_.empty() && base_url_.back() == '/') {
            base_url_.pop_back();
        }
    }
    
    std::string base_url_;
    HttpClient http_client_;
    
    // Helper to send JSON-RPC request
    JsonRpcResponse send_rpc_request(const std::string& method,
                                    const std::string& params_json) {
        // Create JSON-RPC request
        JsonRpcRequest request(generate_uuid(), method, params_json);
        std::string request_json = request.to_json();
        
        // Send HTTP POST
        auto http_response = http_client_.post(
            base_url_,
            request_json,
            "application/json"
        );
        
        // Check HTTP status
        if (!http_response.is_success()) {
            throw A2AException(
                "HTTP request failed: " + std::to_string(http_response.status_code),
                ErrorCode::InternalError
            );
        }
        
        // Parse JSON-RPC response
        JsonRpcResponse rpc_response = JsonRpcResponse::from_json(http_response.body);
        
        // Check for JSON-RPC error
        if (rpc_response.is_error()) {
            const auto& error = *rpc_response.error();
            throw A2AException(
                error.message,
                static_cast<ErrorCode>(error.code)
            );
        }
        
        return rpc_response;
    }
};

A2AClient::A2AClient(const std::string& base_url)
    : impl_(std::make_unique<Impl>(base_url)) {}

A2AClient::~A2AClient() = default;

A2AClient::A2AClient(A2AClient&&) noexcept = default;
A2AClient& A2AClient::operator=(A2AClient&&) noexcept = default;

A2AResponse A2AClient::send_message(const MessageSendParams& params) {
    // Serialize params to JSON
    std::string params_json = params.to_json();
    
    // Send JSON-RPC request
    auto response = impl_->send_rpc_request(A2AMethods::MESSAGE_SEND, params_json);
    
    // Parse result
    if (!response.result_json().has_value()) {
        throw A2AException("No result in response", ErrorCode::InternalError);
    }
    
    const std::string& result_json = *response.result_json();

    // Unwrap A2A JSON-RPC envelope if present.
    // Python agents return: {"type":"message","message":{...}}
    // The actual message fields (messageId, role, parts) are nested inside "message".
    std::string effective_json = result_json;
    if (result_json.find("\"message\":") != std::string::npos &&
        result_json.find("\"type\":") != std::string::npos) {
        // Extract the inner message object from the A2A envelope
        size_t msg_pos = result_json.find("\"message\":");
        size_t obj_start = result_json.find("{", msg_pos + 10);
        if (obj_start != std::string::npos) {
            // Find matching closing brace
            int depth = 0;
            size_t obj_end = obj_start;
            for (size_t i = obj_start; i < result_json.length(); ++i) {
                if (result_json[i] == '{') depth++;
                else if (result_json[i] == '}') {
                    depth--;
                    if (depth == 0) {
                        obj_end = i;
                        break;
                    }
                }
            }
            effective_json = result_json.substr(obj_start, obj_end - obj_start + 1);
        }
    }

    // Determine if result is Task or Message
    // Check for "kind" field or "status" field to distinguish
    if (result_json.find("\"status\":") != std::string::npos) {
        // It's a Task
        AgentTask task = AgentTask::from_json(result_json);
        return A2AResponse(task);
    } else {
        // It's a Message
        AgentMessage message = AgentMessage::from_json(result_json);
        return A2AResponse(message);
    }
}

void A2AClient::send_message_streaming(const MessageSendParams& params,
                                       std::function<void(const std::string&)> callback) {
    // Serialize params to JSON
    std::string params_json = params.to_json();
    
    // Create JSON-RPC request
    JsonRpcRequest request(generate_uuid(), A2AMethods::MESSAGE_STREAM, params_json);
    std::string request_json = request.to_json();
    
    // Send streaming POST request
    impl_->http_client_.post_stream(
        impl_->base_url_,
        request_json,
        "application/json",
        callback
    );
}

AgentTask A2AClient::get_task(const std::string& task_id) {
    // Create params
    TaskIdParams params;
    params.id = task_id;
    std::string params_json = params.to_json();
    
    // Send JSON-RPC request
    auto response = impl_->send_rpc_request(A2AMethods::TASK_GET, params_json);
    
    // Parse result
    if (!response.result_json().has_value()) {
        throw A2AException("No result in response", ErrorCode::InternalError);
    }
    
    return AgentTask::from_json(*response.result_json());
}

AgentTask A2AClient::cancel_task(const std::string& task_id) {
    // Create params
    TaskIdParams params;
    params.id = task_id;
    std::string params_json = params.to_json();
    
    // Send JSON-RPC request
    auto response = impl_->send_rpc_request(A2AMethods::TASK_CANCEL, params_json);
    
    // Parse result
    if (!response.result_json().has_value()) {
        throw A2AException("No result in response", ErrorCode::InternalError);
    }
    
    return AgentTask::from_json(*response.result_json());
}

void A2AClient::subscribe_to_task(const std::string& task_id,
                                  std::function<void(const std::string&)> callback) {
    // Create params
    TaskIdParams params;
    params.id = task_id;
    std::string params_json = params.to_json();
    
    // Create JSON-RPC request
    JsonRpcRequest request(generate_uuid(), A2AMethods::TASK_SUBSCRIBE, params_json);
    std::string request_json = request.to_json();
    
    // Send streaming POST request
    impl_->http_client_.post_stream(
        impl_->base_url_,
        request_json,
        "application/json",
        callback
    );
}

void A2AClient::set_timeout(long seconds) {
    impl_->http_client_.set_timeout(seconds);
}

void A2AClient::set_abort_flag(const std::atomic<bool>* flag) {
    impl_->http_client_.set_abort_flag(flag);
}

void A2AClient::set_resolve_entries(const std::vector<std::string>& entries) {
    impl_->http_client_.set_resolve_entries(entries);
}

void A2AClient::add_header(const std::string& key, const std::string& value) {
    impl_->http_client_.add_header(key, value);
}

void A2AClient::clear_headers() {
    impl_->http_client_.clear_headers();
}

} // namespace a2a
