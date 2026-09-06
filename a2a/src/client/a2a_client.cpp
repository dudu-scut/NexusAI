#include <a2a/client/a2a_client.hpp>
#include <a2a/core/jsonrpc_request.hpp>
#include <a2a/core/jsonrpc_response.hpp>
#include <a2a/core/a2a_methods.hpp>
#include <a2a/core/exception.hpp>
#include <nlohmann/json.hpp>
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
            // 4xx (except 429) is a protocol/configuration error: the
            // adapter must neither retry it nor feed it to the circuit
            // breaker as a backend transport failure.
            if (http_response.status_code >= 400 &&
                http_response.status_code < 500 &&
                http_response.status_code != 429) {
                throw A2AException(
                    "HTTP protocol error: " +
                        std::to_string(http_response.status_code),
                    ErrorCode::InvalidRequest
                );
            }
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
    nlohmann::json parsed = nlohmann::json::parse(result_json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        throw A2AException("Invalid JSON-RPC result", ErrorCode::InternalError);
    }

    // Tasks are recognized by an explicit kind/type discriminator or a
    // top-level status object. A Task result may itself carry a "message"
    // field, so check the discriminator before unwrapping any envelope.
    auto kind_of = [](const nlohmann::json& j) -> std::string {
        auto it = j.contains("kind") ? j.find("kind") : j.find("type");
        return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
    };

    if (kind_of(parsed) == "task") {
        return A2AResponse(AgentTask::from_json(result_json));
    }

    // Unwrap the Python-agent message envelope {"type":"message","message":{...}};
    // a top-level "parts" means the message is already unwrapped.
    nlohmann::json effective = parsed;
    if (!parsed.contains("parts")) {
        const auto envelope_msg = parsed.find("message");
        if (envelope_msg != parsed.end() && envelope_msg->is_object()) {
            effective = *envelope_msg;
        }
    }

    const std::string effective_json = effective.dump();
    if (kind_of(effective) == "task" || effective.contains("status")) {
        return A2AResponse(AgentTask::from_json(effective_json));
    }
    return A2AResponse(AgentMessage::from_json(effective_json));
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
