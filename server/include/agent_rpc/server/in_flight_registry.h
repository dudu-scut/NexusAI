/**
 * @file in_flight_registry.h
 * @brief P24 C0: request-scoped registry of in-flight A2A HTTP calls
 *
 * Every path that issues a blocking A2A HTTP call (DAG subtask via
 * buildCallAgent, single-agent fast path, direct adapter path, ExecutePlan
 * call agent) registers a per-call abort flag here before the call and
 * unregisters it on every exit path. Cancellation entry points flip the
 * flags; the libcurl XFERINFO progress callback reads the flag and
 * interrupts the transfer.
 *
 * Cancellation is scoped by owner_request_id: aborting one request never
 * touches a concurrent request's in-flight calls sharing the same agent
 * URL (R18). Entries whose owner_request_id is empty are treated as
 * anonymous and are cancelled by any request scoped to the same URL.
 */

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace agent_rpc {
namespace server {

class InFlightAbortRegistry {
public:
    static InFlightAbortRegistry& instance() {
        static InFlightAbortRegistry registry;
        return registry;
    }

    // Create and register a fresh abort flag bound to (agent_url, owner).
    std::shared_ptr<std::atomic<bool>> registerInFlight(
        const std::string& agent_url, const std::string& owner_request_id) {
        auto flag = std::make_shared<std::atomic<bool>>(false);
        std::lock_guard<std::mutex> lock(mutex_);
        in_flight_calls_.emplace(
            agent_url, InFlightCall{flag, owner_request_id});
        return flag;
    }

    void unregisterInFlight(
        const std::string& agent_url,
        const std::shared_ptr<std::atomic<bool>>& flag) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto range = in_flight_calls_.equal_range(agent_url);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second.flag == flag) {
                in_flight_calls_.erase(it);
                return;
            }
        }
    }

    // Abort the named request's live calls sharing agent_url (timeout
    // entry points know the exact URL).
    void cancelInFlight(const std::string& agent_url,
                        const std::string& owner_request_id) {
        std::vector<std::shared_ptr<std::atomic<bool>>> flags;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto range = in_flight_calls_.equal_range(agent_url);
            for (auto it = range.first; it != range.second; ++it) {
                if (it->second.owner_request_id == owner_request_id ||
                    it->second.owner_request_id.empty()) {
                    flags.push_back(it->second.flag);
                }
            }
        }
        for (auto& flag : flags) {
            flag->store(true, std::memory_order_release);
        }
    }

    // Abort every live call owned by the request regardless of URL
    // (client-disconnect entry points usually do not know which agent
    // URL is in flight).
    void cancelInFlightByRequest(const std::string& owner_request_id) {
        std::vector<std::shared_ptr<std::atomic<bool>>> flags;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [url, call] : in_flight_calls_) {
                (void)url;
                if (call.owner_request_id == owner_request_id ||
                    call.owner_request_id.empty()) {
                    flags.push_back(call.flag);
                }
            }
        }
        for (auto& flag : flags) {
            flag->store(true, std::memory_order_release);
        }
    }

private:
    InFlightAbortRegistry() = default;

    struct InFlightCall {
        std::shared_ptr<std::atomic<bool>> flag;
        std::string owner_request_id;
    };

    std::mutex mutex_;
    std::unordered_multimap<std::string, InFlightCall> in_flight_calls_;
};

// RAII wrapper: registers on construction, unregisters on every exit path
// of the scope that issued the HTTP call.
class InFlightRegistration {
public:
    InFlightRegistration(const std::string& agent_url,
                         const std::string& owner_request_id)
        : url_(agent_url),
          flag_(InFlightAbortRegistry::instance().registerInFlight(
              agent_url, owner_request_id)) {}

    ~InFlightRegistration() {
        if (flag_) {
            InFlightAbortRegistry::instance().unregisterInFlight(url_, flag_);
        }
    }

    InFlightRegistration(const InFlightRegistration&) = delete;
    InFlightRegistration& operator=(const InFlightRegistration&) = delete;

    const std::shared_ptr<std::atomic<bool>>& flag() const { return flag_; }

private:
    std::string url_;
    std::shared_ptr<std::atomic<bool>> flag_;
};

} // namespace server
} // namespace agent_rpc
