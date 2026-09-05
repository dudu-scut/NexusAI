#include "agent_rpc/mcp/mcp_client.h"
#include "agent_rpc/common/logger.h"
#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#endif
#include <sstream>
#include <fstream>
#include <json/json.h>
#include <ctime>
#include <curl/curl.h>

namespace agent_rpc {
namespace mcp {

MCPClient::MCPClient() {
    // Logger uses global macros; no instance needed
}

MCPClient::~MCPClient() {
    disconnect();
}

bool MCPClient::connect(const std::string& server_path, const std::vector<std::string>& args) {
    // Connect via STDIO transport
    MCPConnectionConfig config;
    config.transport = MCPTransportType::STDIO;
    config.server_path = server_path;
    config.server_args = args;
    return connect(config);
}

bool MCPClient::connect(const MCPConnectionConfig& config) {
    if (connected_) {
        LOG_WARN("MCP client already connected");
        return true;
    }

    config_ = config;
    transport_type_ = config.transport;

    bool success = false;

    if (transport_type_ == MCPTransportType::STDIO) {
        server_path_ = config.server_path;
        server_args_ = config.server_args;

        if (!startMCPServer()) {
            LOG_ERROR("Failed to start MCP server");
            return false;
        }

        connected_ = true;
        running_ = true;

        // Start the notification processing thread
        notification_thread_ = std::thread([this]() {
            processNotificationsStdio();
        });

        LOG_INFO("MCP client connected to server (STDIO): " + server_path_);
        success = true;
    } else if (transport_type_ == MCPTransportType::SSE) {
        if (!connectSSE()) {
            LOG_ERROR("Failed to connect to MCP server via SSE");
            return false;
        }

        connected_ = true;
        running_ = true;

        LOG_INFO("MCP client connected to server (SSE): " + config.sse_url);
        success = true;
    }

    // Send the MCP initialize handshake as required by the protocol
    if (success) {
        MCPRequest init_req;
        init_req.method = "initialize";
        init_req.id = "init_" + std::to_string(std::time(nullptr));

        Json::Value init_params;
        init_params["protocolVersion"] = "2024-11-05";
        Json::Value capabilities;
        capabilities["tools"] = Json::Value(Json::objectValue);
        capabilities["prompts"] = Json::Value(Json::objectValue);
        capabilities["resources"] = Json::Value(Json::objectValue);
        init_params["capabilities"] = capabilities;
        Json::Value client_info;
        client_info["name"] = "nexusai-mcp-client";
        client_info["version"] = "1.0.0";
        init_params["clientInfo"] = client_info;

        Json::StreamWriterBuilder builder;
        builder["indentation"] = "";
        init_req.params = Json::writeString(builder, init_params);

        if (sendRequest(init_req)) {
            MCPResponse init_resp = receiveResponse();
            if (init_resp.is_error) {
                LOG_WARN("MCP initialize handshake returned error: " + init_resp.error);
            } else {
                LOG_INFO("MCP initialize handshake completed successfully");
            }
        } else {
            LOG_WARN("Failed to send MCP initialize request (server may not require it)");
        }
    }

    return success;
}

MCPTransportType MCPClient::getTransportType() const {
    return transport_type_;
}

void MCPClient::disconnect() {
    if (!connected_) {
        return;
    }
    
    running_ = false;
    connected_ = false;
    
    // Stop the notification thread
    if (notification_thread_.joinable()) {
        notification_thread_.join();
    }
    
    if (transport_type_ == MCPTransportType::STDIO) {
        stopMCPServer();
    } else if (transport_type_ == MCPTransportType::SSE) {
        disconnectSSE();
    }
    
    LOG_INFO("MCP client disconnected");
}

bool MCPClient::isConnected() const {
    return connected_;
}

std::vector<MCPTool> MCPClient::listTools() {
    std::vector<MCPTool> tools;
    
    if (!connected_) {
        LOG_ERROR("MCP client not connected");
        return tools;
    }
    
    MCPRequest request;
    request.method = "tools/list";
    request.id = "list_tools_" + std::to_string(std::time(nullptr));
    
    if (!sendRequest(request)) {
        LOG_ERROR("Failed to send tools/list request");
        return tools;
    }
    
    MCPResponse response = receiveResponse();
    if (response.is_error) {
        LOG_ERROR("Error listing tools: " + response.error);
        return tools;
    }
    
    // Parse the response
    try {
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(response.result, root)) {
            const Json::Value& tools_array = root["tools"];
            for (const auto& tool : tools_array) {
                MCPTool mcp_tool;
                mcp_tool.name = tool["name"].asString();
                mcp_tool.description = tool["description"].asString();
                mcp_tool.input_schema = tool["inputSchema"].toStyledString();
                tools.push_back(mcp_tool);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse tools list: " + std::string(e.what()));
    }
    
    return tools;
}

MCPResponse MCPClient::callTool(const std::string& tool_name, const std::string& arguments) {
    MCPResponse response;
    response.is_error = true;
    response.error = "Not connected";
    
    if (!connected_) {
        LOG_ERROR("MCP client not connected");
        return response;
    }
    
    MCPRequest request;
    request.method = "tools/call";
    request.id = "call_tool_" + std::to_string(std::time(nullptr));
    
    // Build the parameters
    Json::Value params;
    params["name"] = tool_name;
    
    Json::Value args_json;
    Json::Reader reader;
    if (reader.parse(arguments, args_json)) {
        params["arguments"] = args_json;
    } else {
        params["arguments"] = Json::Value(Json::objectValue);
    }
    
    // Set only the params section, without method and id
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    request.params = Json::writeString(builder, params);
    
    if (!sendRequest(request)) {
        LOG_ERROR("Failed to send tools/call request");
        response.error = "Failed to send request";
        return response;
    }
    
    response = receiveResponse();
    if (response.is_error) {
        LOG_ERROR("Error calling tool " + tool_name + ": " + response.error);
    } else {
        LOG_INFO("Successfully called tool: " + tool_name);
    }
    
    return response;
}

std::vector<MCPPrompt> MCPClient::listPrompts() {
    std::vector<MCPPrompt> prompts;
    
    if (!connected_) {
        LOG_ERROR("MCP client not connected");
        return prompts;
    }
    
    MCPRequest request;
    request.method = "prompts/list";
    request.id = "list_prompts_" + std::to_string(std::time(nullptr));
    
    if (!sendRequest(request)) {
        LOG_ERROR("Failed to send prompts/list request");
        return prompts;
    }
    
    MCPResponse response = receiveResponse();
    if (response.is_error) {
        LOG_ERROR("Error listing prompts: " + response.error);
        return prompts;
    }
    
    // Parse the response
    try {
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(response.result, root)) {
            const Json::Value& prompts_array = root["prompts"];
            for (const auto& prompt : prompts_array) {
                MCPPrompt mcp_prompt;
                mcp_prompt.name = prompt["name"].asString();
                mcp_prompt.description = prompt["description"].asString();
                mcp_prompt.arguments = prompt["arguments"].toStyledString();
                prompts.push_back(mcp_prompt);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse prompts list: " + std::string(e.what()));
    }
    
    return prompts;
}

MCPResponse MCPClient::getPrompt(const std::string& prompt_name, const std::string& arguments) {
    MCPResponse response;
    response.is_error = true;
    response.error = "Not connected";
    
    if (!connected_) {
        LOG_ERROR("MCP client not connected");
        return response;
    }
    
    MCPRequest request;
    request.method = "prompts/get";
    request.id = "get_prompt_" + std::to_string(std::time(nullptr));

    // Set only params, not the full JSON-RPC envelope —
    // buildJSONRPCRequest constructs the proper JSON-RPC wrapper
    Json::Value params;
    params["name"] = prompt_name;

    Json::Value args_json;
    Json::Reader reader;
    if (reader.parse(arguments, args_json)) {
        params["arguments"] = args_json;
    } else {
        params["arguments"] = Json::Value();
    }

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    request.params = Json::writeString(builder, params);
    
    if (!sendRequest(request)) {
        LOG_ERROR("Failed to send prompts/get request");
        response.error = "Failed to send request";
        return response;
    }
    
    response = receiveResponse();
    if (response.is_error) {
        LOG_ERROR("Error getting prompt " + prompt_name + ": " + response.error);
    } else {
        LOG_INFO("Successfully got prompt: " + prompt_name);
    }
    
    return response;
}

std::vector<MCPResource> MCPClient::listResources() {
    std::vector<MCPResource> resources;
    
    if (!connected_) {
        LOG_ERROR("MCP client not connected");
        return resources;
    }
    
    MCPRequest request;
    request.method = "resources/list";
    request.id = "list_resources_" + std::to_string(std::time(nullptr));
    
    if (!sendRequest(request)) {
        LOG_ERROR("Failed to send resources/list request");
        return resources;
    }
    
    MCPResponse response = receiveResponse();
    if (response.is_error) {
        LOG_ERROR("Error listing resources: " + response.error);
        return resources;
    }
    
    // Parse the response
    try {
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(response.result, root)) {
            const Json::Value& resources_array = root["resources"];
            for (const auto& resource : resources_array) {
                MCPResource mcp_resource;
                mcp_resource.name = resource["name"].asString();
                mcp_resource.description = resource["description"].asString();
                mcp_resource.uri = resource["uri"].asString();
                mcp_resource.mime_type = resource["mimeType"].asString();
                resources.push_back(mcp_resource);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse resources list: " + std::string(e.what()));
    }
    
    return resources;
}

MCPResponse MCPClient::readResource(const std::string& uri) {
    MCPResponse response;
    response.is_error = true;
    response.error = "Not connected";
    
    if (!connected_) {
        LOG_ERROR("MCP client not connected");
        return response;
    }
    
    MCPRequest request;
    request.method = "resources/read";
    request.id = "read_resource_" + std::to_string(std::time(nullptr));

    // Set only params, not the full JSON-RPC envelope
    Json::Value params;
    params["uri"] = uri;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    request.params = Json::writeString(builder, params);
    
    if (!sendRequest(request)) {
        LOG_ERROR("Failed to send resources/read request");
        response.error = "Failed to send request";
        return response;
    }
    
    response = receiveResponse();
    if (response.is_error) {
        LOG_ERROR("Error reading resource " + uri + ": " + response.error);
    } else {
        LOG_INFO("Successfully read resource: " + uri);
    }
    
    return response;
}

void MCPClient::setNotificationCallback(std::function<void(const std::string&, const std::string&)> callback) {
    notification_callback_ = callback;
}

bool MCPClient::sendRequest(const MCPRequest& request) {
    if (transport_type_ == MCPTransportType::STDIO) {
        return sendRequestStdio(request);
    } else if (transport_type_ == MCPTransportType::SSE) {
        return sendRequestSSE(request);
    }
    return false;
}

bool MCPClient::sendRequestStdio(const MCPRequest& request) {
#ifndef _WIN32
    if (stdin_pipe_ == -1) {
        LOG_ERROR("MCP server stdin pipe not available");
        return false;
    }

    std::string json_request = buildJSONRPCRequest(request);
    json_request += "\n";  // MCP protocol separates messages with newlines

    ssize_t written = write(stdin_pipe_, json_request.c_str(), json_request.length());
    if (written != static_cast<ssize_t>(json_request.length())) {
        LOG_ERROR("Failed to write request to MCP server");
        return false;
    }

    return true;
#else
    LOG_ERROR("STDIO transport not supported on Windows");
    return false;
#endif
}

MCPResponse MCPClient::receiveResponse() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    
    // Wait for a response, up to 30 seconds
    if (queue_cv_.wait_for(lock, std::chrono::seconds(30), [this] { return !response_queue_.empty(); })) {
        MCPResponse response = response_queue_.front();
        response_queue_.pop();
        return response;
    }
    
    MCPResponse timeout_response;
    timeout_response.is_error = true;
    timeout_response.error = "Request timeout";
    return timeout_response;
}

void MCPClient::processNotifications() {
    if (transport_type_ == MCPTransportType::STDIO) {
        processNotificationsStdio();
    } else if (transport_type_ == MCPTransportType::SSE) {
        processNotificationsSSE();
    }
}

void MCPClient::processNotificationsStdio() {
#ifndef _WIN32
    if (stdout_pipe_ == -1) {
        LOG_ERROR("MCP server stdout pipe not available");
        return;
    }

    char buffer[4096];
    std::string line;

    while (running_) {
        ssize_t bytes_read = read(stdout_pipe_, buffer, sizeof(buffer) - 1);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            line += buffer;

            // Process complete lines
            size_t pos = 0;
            while ((pos = line.find('\n')) != std::string::npos) {
                std::string message = line.substr(0, pos);
                line.erase(0, pos + 1);

                if (!message.empty()) {
                    MCPResponse response = parseJSONRPCResponse(message);

                    // Check whether it is a notification
                    if (response.id.empty()) {
                        // This is a notification (MCP notifications have no id, with or without error)
                        if (notification_callback_) {
                            // Parse the notification content
                            try {
                                Json::Value root;
                                Json::Reader reader;
                                if (reader.parse(message, root)) {
                                    std::string method = root["method"].asString();
                                    if (method == "notifications/message") {
                                        const Json::Value& params = root["params"];
                                        std::string plugin_name = params["pluginName"].asString();
                                        std::string notification = params["notification"].asString();
                                        notification_callback_(plugin_name, notification);
                                    }
                                }
                            } catch (const std::exception& e) {
                                LOG_WARN("Failed to parse notification: " + std::string(e.what()));
                            }
                        }
                    } else {
                        // This is a response
                        std::lock_guard<std::mutex> lock(queue_mutex_);
                        response_queue_.push(response);
                        queue_cv_.notify_one();
                    }
                }
            }
        } else if (bytes_read == 0) {
            // Pipe closed
            break;
        } else {
            // Read error
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR("Error reading from MCP server stdout");
                break;
            }
        }
    }
#endif
}

bool MCPClient::startMCPServer() {
#ifndef _WIN32
    // Create pipes
    int stdin_pipe_fd[2];
    int stdout_pipe_fd[2];

    if (pipe(stdin_pipe_fd) == -1 || pipe(stdout_pipe_fd) == -1) {
        LOG_ERROR("Failed to create pipes for MCP server");
        return false;
    }

    // Fork the child process
    server_pid_ = fork();
    if (server_pid_ == -1) {
        LOG_ERROR("Failed to fork process for MCP server");
        return false;
    }

    if (server_pid_ == 0) {
        // Child process: run the MCP server
        close(stdin_pipe_fd[1]);  // Close the write end
        close(stdout_pipe_fd[0]); // Close the read end

        // Redirect stdin and stdout
        dup2(stdin_pipe_fd[0], STDIN_FILENO);
        dup2(stdout_pipe_fd[1], STDOUT_FILENO);

        // Prepare arguments
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(server_path_.c_str()));

        for (const auto& arg : server_args_) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        // Execute the MCP server
        execv(server_path_.c_str(), argv.data());

        // execv failed
        LOG_ERROR("Failed to execute MCP server: " + server_path_);
        exit(1);
    } else {
        // Parent process
        close(stdin_pipe_fd[0]);  // Close the read end
        close(stdout_pipe_fd[1]); // Close the write end

        stdin_pipe_ = stdin_pipe_fd[1];
        stdout_pipe_ = stdout_pipe_fd[0];

        // Set non-blocking mode
        fcntl(stdout_pipe_, F_SETFL, O_NONBLOCK);

        // Wait for the server to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        return true;
    }
#else
    LOG_ERROR("STDIO transport not supported on Windows. Use SSE transport instead.");
    return false;
#endif
}

void MCPClient::stopMCPServer() {
#ifndef _WIN32
    if (server_pid_ > 0) {
        kill(server_pid_, SIGTERM);
        waitpid(server_pid_, nullptr, 0);
        server_pid_ = -1;
    }

    if (stdin_pipe_ != -1) {
        close(stdin_pipe_);
        stdin_pipe_ = -1;
    }

    if (stdout_pipe_ != -1) {
        close(stdout_pipe_);
        stdout_pipe_ = -1;
    }
#endif
}

std::string MCPClient::buildJSONRPCRequest(const MCPRequest& request) {
    Json::Value root;
    root["jsonrpc"] = "2.0";
    root["method"] = request.method;
    root["id"] = request.id;
    
    if (!request.params.empty()) {
        Json::Value params;
        Json::Reader reader;
        if (reader.parse(request.params, params)) {
            root["params"] = params;
        }
    }
    
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, root);
}

MCPResponse MCPClient::parseJSONRPCResponse(const std::string& response) {
    MCPResponse mcp_response;
    
    try {
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(response, root)) {
            mcp_response.id = root["id"].asString();
            
            if (root.isMember("error")) {
                mcp_response.is_error = true;
                mcp_response.error = root["error"]["message"].asString();
            } else {
                mcp_response.is_error = false;
                mcp_response.result = root["result"].toStyledString();
            }
        }
    } catch (const std::exception& e) {
        mcp_response.is_error = true;
        mcp_response.error = "Failed to parse response: " + std::string(e.what());
    }
    
    return mcp_response;
}

// SSE (Server-Sent Events) transport implementation

bool MCPClient::connectSSE() {
    // curl_global_init is called once in server/src/main.cpp
    curl_handle_ = curl_easy_init();
    
    if (!curl_handle_) {
        LOG_ERROR("Failed to initialize CURL for SSE");
        return false;
    }
    
    // Send the initialization request to obtain a session ID
    CURL* curl = static_cast<CURL*>(curl_handle_);
    
    // Build the initialization URL
    std::string init_url = config_.sse_url;
    if (init_url.back() != '/') {
        init_url += "/";
    }
    init_url += "sse";
    
    curl_easy_setopt(curl, CURLOPT_URL, init_url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sseWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, sseHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, this);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (config_.connect_timeout_ms + 999) / 1000);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (config_.connect_timeout_ms + 999) / 1000);
    
    // Set API key if provided
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    headers = curl_slist_append(headers, "Cache-Control: no-cache");
    if (!config_.api_key.empty()) {
        std::string auth_header = "Authorization: Bearer " + config_.api_key;
        headers = curl_slist_append(headers, auth_header.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    
    // SSL verification
    if (!config_.verify_ssl) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    
    // Perform the request to obtain the session
    {
        std::lock_guard<std::mutex> lock(sse_buffer_mutex_);
        sse_response_buffer_.clear();
    }
    CURLcode res = curl_easy_perform(curl);
    
    curl_slist_free_all(headers);
    
    if (res != CURLE_OK) {
        LOG_ERROR("SSE connection failed: " + std::string(curl_easy_strerror(res)));
        curl_easy_cleanup(curl);
        curl_handle_ = nullptr;
        return false;
    }
    
    // Parse the session ID
    if (sse_session_id_.empty()) {
        // Try parsing from the response
        std::lock_guard<std::mutex> lock(sse_buffer_mutex_);
        try {
            Json::Value root;
            Json::Reader reader;
            if (reader.parse(sse_response_buffer_, root)) {
                if (root.isMember("sessionId")) {
                    sse_session_id_ = root["sessionId"].asString();
                }
            }
        } catch (...) {
            // Ignore parse errors
        }
    }
    
    // Start the SSE event listener thread
    sse_event_thread_ = std::thread([this]() {
        processNotificationsSSE();
    });
    
    LOG_INFO("SSE connection established, session: " + sse_session_id_);
    return true;
}

void MCPClient::disconnectSSE() {
    running_ = false;
    
    if (sse_event_thread_.joinable()) {
        sse_event_thread_.join();
    }
    
    if (curl_handle_) {
        curl_easy_cleanup(static_cast<CURL*>(curl_handle_));
        curl_handle_ = nullptr;
    }

    // curl_global_cleanup is called once in server/src/main.cpp
    sse_session_id_.clear();
    
    LOG_INFO("SSE connection closed");
}

bool MCPClient::sendRequestSSE(const MCPRequest& request) {
    if (!curl_handle_) {
        LOG_ERROR("SSE not connected");
        return false;
    }
    
    // Create a new CURL handle for the POST request
    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("Failed to create CURL handle for SSE request");
        return false;
    }
    
    // Build the request URL
    std::string post_url = config_.sse_url;
    if (post_url.back() != '/') {
        post_url += "/";
    }
    post_url += "message";
    
    // Append the session ID to the URL if present
    if (!sse_session_id_.empty()) {
        post_url += "?sessionId=" + sse_session_id_;
    }
    
    // Build the JSON-RPC request
    std::string json_request = buildJSONRPCRequest(request);
    
    curl_easy_setopt(curl, CURLOPT_URL, post_url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_request.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, json_request.length());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (config_.request_timeout_ms + 999) / 1000);
    
    // Set the response callback
    std::string response_data;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, 
        +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            std::string* data = static_cast<std::string*>(userdata);
            data->append(ptr, size * nmemb);
            return size * nmemb;
        });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    
    // Set request headers
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!config_.api_key.empty()) {
        std::string auth_header = "Authorization: Bearer " + config_.api_key;
        headers = curl_slist_append(headers, auth_header.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    
    // SSL verification
    if (!config_.verify_ssl) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    
    // Perform the request
    CURLcode res = curl_easy_perform(curl);
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        LOG_ERROR("SSE request failed: " + std::string(curl_easy_strerror(res)));
        return false;
    }
    
    // Parse the response and enqueue it
    if (!response_data.empty()) {
        MCPResponse response = parseJSONRPCResponse(response_data);
        std::lock_guard<std::mutex> lock(queue_mutex_);
        response_queue_.push(response);
        queue_cv_.notify_one();
    }
    
    return true;
}

MCPResponse MCPClient::receiveResponseSSE() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    
    // Wait for a response
    int timeout_ms = config_.request_timeout_ms > 0 ? config_.request_timeout_ms : 30000;
    if (queue_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), 
                          [this] { return !response_queue_.empty(); })) {
        MCPResponse response = response_queue_.front();
        response_queue_.pop();
        return response;
    }
    
    MCPResponse timeout_response;
    timeout_response.is_error = true;
    timeout_response.error = "SSE request timeout";
    return timeout_response;
}

void MCPClient::processNotificationsSSE() {
    if (!curl_handle_) {
        return;
    }
    
    // Create the SSE listening connection
    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("Failed to create CURL handle for SSE events");
        return;
    }
    
    // Build the SSE event URL
    std::string sse_url = config_.sse_url;
    if (sse_url.back() != '/') {
        sse_url += "/";
    }
    sse_url += "sse";
    if (!sse_session_id_.empty()) {
        sse_url += "?sessionId=" + sse_session_id_;
    }
    
    curl_easy_setopt(curl, CURLOPT_URL, sse_url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sseWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);  // No timeout; keep listening
    
    // Set request headers
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    headers = curl_slist_append(headers, "Cache-Control: no-cache");
    if (!config_.api_key.empty()) {
        std::string auth_header = "Authorization: Bearer " + config_.api_key;
        headers = curl_slist_append(headers, auth_header.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    
    // SSL verification
    if (!config_.verify_ssl) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    
    // Listen for SSE events (blocks until the connection closes or running_ is false)
    while (running_) {
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK && running_) {
            LOG_WARN("SSE connection interrupted: " + std::string(curl_easy_strerror(res)));
            // Wait briefly before reconnecting
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

size_t MCPClient::sseWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    MCPClient* client = static_cast<MCPClient*>(userdata);
    size_t total_size = size * nmemb;
    
    std::string data(ptr, total_size);

    // Lock to protect sse_response_buffer_ from concurrent CURL I/O thread access
    {
        std::lock_guard<std::mutex> lock(client->sse_buffer_mutex_);
        client->sse_response_buffer_ += data;
    }

    // Parse SSE events (processing under lock for thread safety)
    size_t pos = 0;
    {
        std::lock_guard<std::mutex> lock(client->sse_buffer_mutex_);
        while ((pos = client->sse_response_buffer_.find("\n\n")) != std::string::npos) {
            std::string event = client->sse_response_buffer_.substr(0, pos);
            client->sse_response_buffer_.erase(0, pos + 2);

        // Parse event data
        std::string event_data;
        std::istringstream iss(event);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.substr(0, 5) == "data:") {
                event_data = line.substr(5);
                // Strip leading whitespace
                size_t start = event_data.find_first_not_of(" ");
                if (start != std::string::npos) {
                    event_data = event_data.substr(start);
                }
            } else if (line.substr(0, 3) == "id:") {
                // Update the session ID
                client->sse_session_id_ = line.substr(3);
                size_t start = client->sse_session_id_.find_first_not_of(" ");
                if (start != std::string::npos) {
                    client->sse_session_id_ = client->sse_session_id_.substr(start);
                }
            }
        }
        
        if (!event_data.empty()) {
            // Parse the JSON-RPC response or notification
            MCPResponse response = client->parseJSONRPCResponse(event_data);
            
            if (response.id.empty()) {
                // This is a notification
                if (client->notification_callback_) {
                    try {
                        Json::Value root;
                        Json::Reader reader;
                        if (reader.parse(event_data, root)) {
                            std::string method = root["method"].asString();
                            if (method == "notifications/message") {
                                const Json::Value& params = root["params"];
                                std::string plugin_name = params["pluginName"].asString();
                                std::string notification = params["notification"].asString();
                                client->notification_callback_(plugin_name, notification);
                            }
                        }
                    } catch (const std::exception& e) {
                        LOG_WARN("Failed to parse SSE notification: " + std::string(e.what()));
                    }
                }
            } else {
                // This is a response
                std::lock_guard<std::mutex> lock(client->queue_mutex_);
                client->response_queue_.push(response);
                client->queue_cv_.notify_one();
            }
        }
        }
    }

    return total_size;
}

size_t MCPClient::sseHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    // Response headers could be parsed here
    return size * nitems;
}

} // namespace mcp
} // namespace agent_rpc


