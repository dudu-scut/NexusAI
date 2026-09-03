#include <a2a/core/http_client.hpp>
#include <a2a/core/exception.hpp>
#include <curl/curl.h>
#include <atomic>
#include <sstream>
#include <cstring>

namespace a2a {

// RAII wrappers for CURL resources to prevent leaks on exception paths.
// These ensure curl_easy handles and slist are cleaned up even when exceptions
// are thrown between init and cleanup calls.
struct CurlHandle {
    CURL* h = nullptr;
    CurlHandle() : h(curl_easy_init()) {}
    ~CurlHandle() { if (h) curl_easy_cleanup(h); }
    operator CURL*() const { return h; }
};

struct CurlSList {
    curl_slist* h = nullptr;
    ~CurlSList() { if (h) curl_slist_free_all(h); }
    void append(const char* s) { h = curl_slist_append(h, s); }
    operator curl_slist*() const { return h; }
};

// Callback for writing response data
static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    std::string* response = static_cast<std::string*>(userp);
    response->append(static_cast<char*>(contents), total_size);
    return total_size;
}

// Progress callback used as the in-flight abort channel: returning 1 aborts
// the transfer with CURLE_ABORTED_BY_CALLBACK (P20: DAG subtask cancellation).
static int abort_check_callback(void* clientp, curl_off_t /*dltotal*/,
                                curl_off_t /*dlnow*/, curl_off_t /*ultotal*/,
                                curl_off_t /*ulnow*/) {
    const auto* flag = static_cast<const std::atomic<bool>*>(clientp);
    return (flag && flag->load(std::memory_order_relaxed)) ? 1 : 0;
}

/**
 * @brief Streaming context that handles UTF-8 boundary issues
 *
 * CURL may split data at network packet boundaries, potentially cutting
 * multi-byte UTF-8 characters in half. Splitting on SSE event boundaries
 * (double newlines) guarantees complete JSON events.
 */
struct StreamContext {
    std::function<void(const std::string&)>* callback;
    std::string buffer;  // Accumulates incomplete event data
    std::string last_error;  // Stores the last error message
    
    /**
     * @brief Returns the length of the valid UTF-8 prefix of a string
     *
     * If the string ends with an incomplete UTF-8 sequence, returns the
     * length of the valid portion.
     */
    static size_t find_valid_utf8_end(const std::string& str) {
        if (str.empty()) return 0;
        
        size_t len = str.length();
        size_t i = len;
        
        // Scan backwards for an incomplete UTF-8 sequence.
        // UTF-8 encoding rules:
        // - Single byte: 0xxxxxxx (0x00-0x7F)
        // - Two-byte lead: 110xxxxx (0xC0-0xDF)
        // - Three-byte lead: 1110xxxx (0xE0-0xEF)
        // - Four-byte lead: 11110xxx (0xF0-0xF7)
        // - Continuation byte: 10xxxxxx (0x80-0xBF)
        
        // Scan back at most 4 bytes (maximum UTF-8 sequence length)
        size_t check_start = (len > 4) ? len - 4 : 0;
        
        for (i = len; i > check_start; ) {
            i--;
            unsigned char c = static_cast<unsigned char>(str[i]);
            
            if ((c & 0x80) == 0) {
                // Single-byte character (ASCII); everything before is valid
                return len;
            } else if ((c & 0xC0) == 0x80) {
                // Continuation byte; keep scanning for the lead byte
                continue;
            } else if ((c & 0xE0) == 0xC0) {
                // Two-byte lead; check whether the sequence is complete
                size_t expected_len = 2;
                size_t actual_len = len - i;
                if (actual_len >= expected_len) {
                    return len;  // Complete
                } else {
                    return i;  // Incomplete; return the position before the lead byte
                }
            } else if ((c & 0xF0) == 0xE0) {
                // Three-byte lead; check whether the sequence is complete
                size_t expected_len = 3;
                size_t actual_len = len - i;
                if (actual_len >= expected_len) {
                    return len;  // Complete
                } else {
                    return i;  // Incomplete
                }
            } else if ((c & 0xF8) == 0xF0) {
                // Four-byte lead; check whether the sequence is complete
                size_t expected_len = 4;
                size_t actual_len = len - i;
                if (actual_len >= expected_len) {
                    return len;  // Complete
                } else {
                    return i;  // Incomplete
                }
            }
        }
        
        return len;  // Default: return everything
    }
    
    /**
     * @brief Invoke the callback safely, swallowing all exceptions
     */
    void safe_callback(const std::string& data) {
        try {
            (*callback)(data);
        } catch (const std::exception& e) {
            // Record the error without propagating the exception
            last_error = e.what();
        } catch (...) {
            last_error = "Unknown exception in callback";
        }
    }
    
    /**
     * @brief Process an incoming data chunk
     *
     * SSE format: "data: {...}\n\n". Splitting on double newlines
     * ensures each callback receives a complete event.
     */
    void process_chunk(const char* data, size_t size) {
        buffer.append(data, size);
        
        // Split on double newlines to extract complete SSE events
        size_t pos = 0;
        while (pos < buffer.size()) {
            // Find the SSE event delimiter (double newline)
            size_t event_end = buffer.find("\n\n", pos);
            if (event_end == std::string::npos) {
                // No complete event yet; keep the remaining data
                break;
            }
            
            // Extract the complete event (including the first newline)
            std::string event = buffer.substr(pos, event_end - pos + 1);
            
            // Validate UTF-8 completeness
            size_t valid_end = find_valid_utf8_end(event);
            if (valid_end == event.length()) {
                // UTF-8 is complete; forward to the callback
                safe_callback(event);
            }
            // Skip if UTF-8 is incomplete (should not happen since we split on event boundaries)
            
            pos = event_end + 2;  // Skip the double newline
        }
        
        // Keep unprocessed incomplete data
        if (pos < buffer.size()) {
            buffer = buffer.substr(pos);
        } else {
            buffer.clear();
        }
    }
    
    /**
     * @brief Flush the remaining buffer (called when the stream ends)
     */
    void flush() {
        if (!buffer.empty()) {
            // Validate UTF-8 completeness
            size_t valid_end = find_valid_utf8_end(buffer);
            if (valid_end > 0) {
                std::string valid_data = buffer.substr(0, valid_end);
                if (!valid_data.empty() && valid_data != "\n") {
                    safe_callback(valid_data);
                }
            }
            buffer.clear();
        }
    }
};

// Callback for streaming data with UTF-8 safe handling
static size_t stream_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    auto* ctx = static_cast<StreamContext*>(userp);
    ctx->process_chunk(static_cast<const char*>(contents), total_size);
    return total_size;
}

// curl_global_init is now called once in server/src/main.cpp.
// This module no longer calls it independently.

// PIMPL implementation
class HttpClient::Impl {
public:
    Impl() : timeout_(120L) {  // Streaming AI responses need a longer timeout (120s)
        // curl_global_init is centralized in server/src/main.cpp
    }
    
    ~Impl() {
        // Do not call curl_global_cleanup() here; the CurlGlobalInit
        // singleton handles it at process exit.
    }
    
    long timeout_;
    std::map<std::string, std::string> headers_;
    const std::atomic<bool>* abort_flag_ = nullptr;
    std::vector<std::string> resolve_entries_;
};

HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {}

HttpClient::~HttpClient() = default;

HttpClient::HttpClient(HttpClient&&) noexcept = default;
HttpClient& HttpClient::operator=(HttpClient&&) noexcept = default;

void HttpClient::set_abort_flag(const std::atomic<bool>* flag) {
    impl_->abort_flag_ = flag;
}

void HttpClient::set_resolve_entries(const std::vector<std::string>& entries) {
    impl_->resolve_entries_ = entries;
}

HttpResponse HttpClient::get(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw A2AException("Failed to initialize CURL", ErrorCode::InternalError);
    }
    
    std::string response_body;
    HttpResponse response;
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, impl_->timeout_);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    
    // Add custom headers
    struct curl_slist* header_list = nullptr;
    for (const auto& [key, value] : impl_->headers_) {
        std::string header = key + ": " + value;
        header_list = curl_slist_append(header_list, header.c_str());
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }
    
    CURLcode res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        throw A2AException(
            std::string("CURL error: ") + curl_easy_strerror(res),
            ErrorCode::InternalError
        );
    }
    
    long status_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
    
    response.status_code = static_cast<int>(status_code);
    response.body = response_body;
    
    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    
    return response;
}

HttpResponse HttpClient::post(const std::string& url,
                              const std::string& body,
                              const std::string& content_type) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw A2AException("Failed to initialize CURL", ErrorCode::InternalError);
    }
    
    std::string response_body;
    HttpResponse response;
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.length());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, impl_->timeout_);
    // P20: in-flight abort channel (DAG subtask cancellation).
    if (impl_->abort_flag_) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abort_check_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, impl_->abort_flag_);
    }
    // P21 L2: pin validated host→IP mappings for this transfer.
    struct curl_slist* resolve_list = nullptr;
    for (const auto& entry : impl_->resolve_entries_) {
        resolve_list = curl_slist_append(resolve_list, entry.c_str());
    }
    if (resolve_list) {
        curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve_list);
    }
    
    // Set headers
    struct curl_slist* header_list = nullptr;
    header_list = curl_slist_append(header_list, ("Content-Type: " + content_type).c_str());
    
    for (const auto& [key, value] : impl_->headers_) {
        std::string header = key + ": " + value;
        header_list = curl_slist_append(header_list, header.c_str());
    }
    
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    
    CURLcode res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        curl_slist_free_all(resolve_list);
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        throw A2AException(
            std::string("CURL error: ") + curl_easy_strerror(res),
            ErrorCode::InternalError
        );
    }
    
    long status_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
    
    response.status_code = static_cast<int>(status_code);
    response.body = response_body;
    
    curl_slist_free_all(resolve_list);
    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    
    return response;
}

void HttpClient::post_stream(const std::string& url,
                             const std::string& body,
                             const std::string& content_type,
                             std::function<void(const std::string&)> callback) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw A2AException("Failed to initialize CURL", ErrorCode::InternalError);
    }
    
    // Create a streaming context to handle UTF-8 boundary issues
    StreamContext ctx;
    ctx.callback = &callback;
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.length());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);  // Pass the context instead of the callback
    
    // Total timeout prevents indefinite connections (default 300s);
    // low-speed timeout acts as a liveness check (disconnect after 60s of no data)
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);  // Total timeout: prevent a malicious server from holding the connection forever
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);  // Minimum speed: 1 byte/s
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);  // Timeout if below minimum speed for 60s
    // P20: in-flight abort channel (DAG subtask cancellation).
    if (impl_->abort_flag_) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abort_check_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, impl_->abort_flag_);
    }
    
    // Set headers
    struct curl_slist* header_list = nullptr;
    header_list = curl_slist_append(header_list, ("Content-Type: " + content_type).c_str());
    header_list = curl_slist_append(header_list, "Accept: text/event-stream");
    
    for (const auto& [key, value] : impl_->headers_) {
        std::string header = key + ": " + value;
        header_list = curl_slist_append(header_list, header.c_str());
    }
    
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    
    CURLcode res = curl_easy_perform(curl);
    
    // Flush any remaining buffered data
    ctx.flush();
    
    curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    
    if (res != CURLE_OK) {
        throw A2AException(
            std::string("CURL error: ") + curl_easy_strerror(res),
            ErrorCode::InternalError
        );
    }
}

void HttpClient::set_timeout(long seconds) {
    impl_->timeout_ = seconds;
}

void HttpClient::add_header(const std::string& key, const std::string& value) {
    impl_->headers_[key] = value;
}

void HttpClient::clear_headers() {
    impl_->headers_.clear();
}

} // namespace a2a
