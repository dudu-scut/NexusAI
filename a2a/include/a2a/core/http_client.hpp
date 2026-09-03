#pragma once

#include <atomic>
#include <string>
#include <map>
#include <memory>
#include <functional>
#include <vector>

namespace a2a {

/**
 * @brief HTTP Response
 */
struct HttpResponse {
    int status_code;
    std::string body;
    std::map<std::string, std::string> headers;
    
    bool is_success() const {
        return status_code >= 200 && status_code < 300;
    }
};

/**
 * @brief HTTP Client wrapper (uses libcurl internally)
 */
class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    
    // Disable copy, enable move
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) noexcept;
    HttpClient& operator=(HttpClient&&) noexcept;
    
    /**
     * @brief Perform GET request
     */
    HttpResponse get(const std::string& url);
    
    /**
     * @brief Perform POST request
     */
    HttpResponse post(const std::string& url, 
                     const std::string& body,
                     const std::string& content_type = "application/json");
    
    /**
     * @brief Perform POST request with streaming response
     * @param callback Called for each chunk of data received
     */
    void post_stream(const std::string& url,
                    const std::string& body,
                    const std::string& content_type,
                    std::function<void(const std::string&)> callback);
    
    /**
     * @brief Set request timeout in seconds
     */
    void set_timeout(long seconds);

    /**
     * @brief Attach an abort flag checked during the transfer.
     * When the flag is set to true mid-request, the in-flight transfer
     * aborts with CURLE_ABORTED_BY_CALLBACK and post/post_stream throw.
     * The flag must outlive the request (caller-owned, typically a
     * shared_ptr<atomic<bool>> held by the orchestrator).
     * @param flag  Pointer to an external atomic abort flag (may be null)
     */
    void set_abort_flag(const std::atomic<bool>* flag);

    /**
     * @brief Pin host→IP mappings for the next transfers (P21 L2).
     * Backs CURLOPT_RESOLVE so the validated addresses are the exact ones
     * used for connection, closing the DNS-rebinding TOCTOU window.
     * Entries use curl's syntax: "host:port:address" (port may be empty).
     * @param entries  "host:port:ip" strings, one per validated address
     */
    void set_resolve_entries(const std::vector<std::string>& entries);
    
    /**
     * @brief Add custom header
     */
    void add_header(const std::string& key, const std::string& value);
    
    /**
     * @brief Clear all custom headers
     */
    void clear_headers();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace a2a
