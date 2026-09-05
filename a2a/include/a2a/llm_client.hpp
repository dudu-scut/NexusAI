#pragma once

#include <string>
#include <cstdlib>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;

/**
 * @brief Generic LLM API client (OpenAI-compatible interface)
 *
 * Supports any service compatible with the OpenAI API format.
 * Authenticates via Bearer token and uses the /v1/chat/completions endpoint.
 */
class LLMClient {
public:
    // Default per-call wall-clock cap (CURLOPT_TIMEOUT) — the historical
    // hardcoded value, kept as the default for all 2-arg chat() callers.
    static constexpr int kDefaultChatTimeoutSeconds = 20;

    explicit LLMClient(const std::string& api_key,
                       const std::string& model = "",
                       const std::string& api_url = "")
        : api_key_(api_key)
        , model_(model.empty() ? (std::getenv("LLM_MODEL") ? std::getenv("LLM_MODEL") : "deepseek-v4-flash") : model)
        , api_url_(api_url.empty() ? (std::getenv("LLM_API_URL") ? std::getenv("LLM_API_URL") : "https://api.deepseek.com/v1/chat/completions") : api_url) {
        // curl_global_init is now called once in server/src/main.cpp.
        // This module no longer calls it independently.
    }

    virtual ~LLMClient() {
        // Never call curl_global_cleanup in destructor:
        // multiple instances share the process-global curl state,
        // and cleanup is handled at process exit.
    }

    /**
     * @brief Call the LLM API (OpenAI-compatible format)
     * @param system_prompt System prompt
     * @param user_message User message
     * @return AI reply
     * Virtual so tests can inject a scripted fake (see TaskPlanner retry tests).
     */
    virtual std::string chat(const std::string& system_prompt,
                    const std::string& user_message) {
        return chatWithTimeout(system_prompt, user_message, kDefaultChatTimeoutSeconds);
    }

    /**
     * @brief Call the LLM API with a per-call timeout (A3 / P20⑧).
     *
     * Deadline-derived budget: callers pass min(default, remaining budget)
     * so a request that already burned most of its deadline skips the
     * always-20s planning call instead of overrunning it. The curl handle is
     * created per call, so the timeout carries no cross-thread state.
     * Test fakes derived from this class that only override the 2-arg form
     * must also override this variant (ScriptedLLMClient does).
     */
    virtual std::string chat(const std::string& system_prompt,
                             const std::string& user_message,
                             int timeout_seconds) {
        return chatWithTimeout(system_prompt, user_message, timeout_seconds);
    }

protected:
    /**
     * @brief Shared implementation: builds the OpenAI request, sends it with
     * the given wall-clock timeout, and parses the reply.
     */
    std::string chatWithTimeout(const std::string& system_prompt,
                                const std::string& user_message,
                                int timeout_seconds) {
        // Build the OpenAI-compatible request JSON
        json messages = json::array();

        if (!system_prompt.empty()) {
            messages.push_back({
                {"role", "system"},
                {"content", system_prompt}
            });
        }

        messages.push_back({
            {"role", "user"},
            {"content", user_message}
        });

        json request_body = {
            {"model", model_},
            {"messages", messages}
        };

        std::string request_str = request_body.dump();

        // Send the HTTP request
        std::string response = send_post_request(request_str, timeout_seconds);

        // Parse the response
        try {
            json response_json = json::parse(response);

            // Check for errors (OpenAI format)
            if (response_json.contains("error")) {
                std::string error_msg = "API Error: " +
                    response_json["error"].value("message", "Unknown error");
                throw std::runtime_error(error_msg);
            }

            // Extract the reply content (OpenAI format)
            if (response_json.contains("choices") &&
                response_json["choices"].is_array() &&
                !response_json["choices"].empty()) {

                auto& choice = response_json["choices"][0];
                if (choice.contains("message") &&
                    choice["message"].contains("content")) {
                    return choice["message"]["content"].get<std::string>();
                }
            }

            throw std::runtime_error("Invalid response format");

        } catch (const json::exception& e) {
            throw std::runtime_error(std::string("JSON parse error: ") + e.what());
        }
    }

public:
    /// Returns the current model name
    const std::string& model() const { return model_; }

    /// Sets the model name
    void setModel(const std::string& model) { model_ = model; }

private:
    static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    std::string send_post_request(const std::string& data, int timeout_seconds) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to initialize CURL");
        }

        std::string response_data;

        // Set request headers
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        std::string auth_header = "Authorization: Bearer " + api_key_;
        headers = curl_slist_append(headers, auth_header.c_str());

        // Configure CURL. The curl handle is created per call, so the
        // per-call timeout carries no cross-thread state.
        const long timeout_l = timeout_seconds > 0
            ? static_cast<long>(timeout_seconds)
            : static_cast<long>(kDefaultChatTimeoutSeconds);
        curl_easy_setopt(curl, CURLOPT_URL, api_url_.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_l);

        // Perform the request
        CURLcode res = curl_easy_perform(curl);

        // Cleanup
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            throw std::runtime_error(std::string("CURL error: ") +
                                   curl_easy_strerror(res));
        }

        return response_data;
    }

    std::string api_key_;
    std::string model_;
    std::string api_url_;
};
