/**
 * @file in_flight_registry.h
 * @brief P24 C0/C2: request-scoped cancellation hub for A2A calls
 *
 * Two-level cancellation model (P24 C2):
 *  - Request-level token (`cancelled_requests_`): single source of truth
 *    for "this request was cancelled", written by every detection point
 *    (client disconnect, post-return IsCancelled checks) and read by
 *    registerInFlight (pre-arms fresh calls) and LLM callers via tokenFor().
 *  - Per-call flag (`in_flight_calls_`): the curl-readable copy, kept
 *    separate so a DAG subtask timeout aborts ONE call by URL without
 *    touching sibling subtasks (R18) — a concurrent request sharing the
 *    same agent URL is never aborted as collateral; entries with an empty
 *    owner_request_id are anonymous and cancelable by any URL-scoped
 *    request.
 *
 * Every path issuing a blocking A2A HTTP call (DAG subtask, single-agent
 * fast path, direct adapter path, ExecutePlan call agent) registers a flag
 * before the call and unregisters on every exit path; the libcurl XFERINFO
 * progress callback reads the flag and interrupts the transfer.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agent_rpc {
namespace server {

class InFlightAbortRegistry {
public:
    static InFlightAbortRegistry& instance() {
        static InFlightAbortRegistry registry;
        return registry;
    }

    // P24 C2: request-level cancellation token (create-if-absent). Callers
    // that hand the raw pointer to a long-running call (planning/aggregation
    // LLM) must hold the returned shared_ptr for the duration of the call so
    // a sweep cannot invalidate it.
    std::shared_ptr<std::atomic<bool>> tokenFor(
        const std::string& request_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return tokenForLocked(request_id);
    }

    // Create and register a fresh abort flag bound to (agent_url, owner),
    // pre-armed when the request was already cancelled.
    std::shared_ptr<std::atomic<bool>> registerInFlight(
        const std::string& agent_url, const std::string& owner_request_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Pre-arm from the request-level token or a pending-cancel marker;
        // the lookups and the emplace share one lock scope — both maps are
        // mutated on concurrent request threads.
        const bool pre_armed =
            isRequestCancelledUnlocked(owner_request_id) ||
            pending_cancels_.erase(pendingCancelKey(owner_request_id,
                                                   agent_url)) > 0;
        auto flag = std::make_shared<std::atomic<bool>>(pre_armed);
        in_flight_calls_.emplace(
            agent_url, InFlightCall{flag, owner_request_id, ""});
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

    // P24 C1②: remember the A2A task id of a task-typed response so a
    // later cancellation can send a best-effort protocol-level
    // tasks/cancel to the agent. Stored on the live in-flight entry, so
    // the RAII unregister clears it (no separate lifecycle to leak).
    void setInFlightTaskId(
        const std::string& agent_url,
        const std::shared_ptr<std::atomic<bool>>& flag,
        const std::string& task_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto range = in_flight_calls_.equal_range(agent_url);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second.flag == flag) {
                it->second.task_id = task_id;
                return;
            }
        }
    }

    // Task id recorded for the named request's live call on agent_url
    // (empty when the response was message-typed or the call is gone).
    std::string inFlightTaskId(
        const std::string& agent_url,
        const std::string& owner_request_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto range = in_flight_calls_.equal_range(agent_url);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second.owner_request_id == owner_request_id &&
                !it->second.task_id.empty()) {
                return it->second.task_id;
            }
        }
        return "";
    }

    // Abort the named request's live calls sharing agent_url (DAG subtask
    // timeout). Does NOT mark the request-level token — sibling subtasks of
    // the same request keep running. A miss (worker still inside
    // buildCallAgent, flag not yet registered) records a pending-cancel
    // marker consumed by the next registerInFlight for (owner, url),
    // closing the check-then-act window between the collector's timeout
    // and the worker's registration.
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
            if (flags.empty()) {
                rememberPendingCancelLocked(owner_request_id, agent_url);
            }
        }
        for (auto& flag : flags) {
            flag->store(true, std::memory_order_release);
        }
    }

    // Abort every live call owned by the request regardless of URL and
    // mark the request-level token so future calls register pre-armed
    // (disconnect entry points do not know the in-flight URL).
    void cancelInFlightByRequest(const std::string& owner_request_id) {
        std::vector<std::shared_ptr<std::atomic<bool>>> flags;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto token = tokenForLocked(owner_request_id);
            token->store(true, std::memory_order_release);
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

    // Read point for plumbing layers (LLM callers): true when the request
    // was already cancelled. Non-creating — a request that was never
    // cancelled has no token entry.
    bool isRequestCancelled(const std::string& request_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return cancelled_requests_.find(request_id) !=
               cancelled_requests_.end();
    }

private:
    InFlightAbortRegistry() = default;

    // Must hold mutex_. Sweeps stale markers so the map stays bounded; an
    // evicted request only loses the pre-arm optimization — terminal-state
    // correctness is owned by the durable pipeline, not here.
    std::shared_ptr<std::atomic<bool>> tokenForLocked(
        const std::string& request_id) {
        const auto now = std::chrono::steady_clock::now();
        if (cancelled_requests_.size() >= kMaxTrackedRequests) {
            for (auto it = cancelled_requests_.begin();
                 it != cancelled_requests_.end();) {
                if (now - it->second.time_point > kTokenTtl) {
                    it = cancelled_requests_.erase(it);
                } else {
                    ++it;
                }
            }
            if (cancelled_requests_.size() >= kMaxTrackedRequests) {
                // Burst guard: evict the oldest marker even if young.
                auto oldest = cancelled_requests_.begin();
                for (auto it = cancelled_requests_.begin();
                     it != cancelled_requests_.end(); ++it) {
                    if (it->second.time_point < oldest->second.time_point) {
                        oldest = it;
                    }
                }
                cancelled_requests_.erase(oldest);
            }
        }
        auto& entry = cancelled_requests_[request_id];
        if (entry.time_point == std::chrono::steady_clock::time_point{}) {
            entry.time_point = now;
        }
        if (!entry.flag) {
            entry.flag = std::make_shared<std::atomic<bool>>(false);
        }
        return entry.flag;
    }

    // Caller must hold the lock or accept a benign race on a bool.
    bool isRequestCancelledUnlocked(const std::string& request_id) {
        auto it = cancelled_requests_.find(request_id);
        return it != cancelled_requests_.end() &&
               it->second.flag->load(std::memory_order_acquire);
    }

    // Must hold mutex_. Markers are tiny and consumed on registration; the
    // size cap only guards against pathological accumulations (owners whose
    // calls never re-register).
    static std::string pendingCancelKey(const std::string& owner,
                                        const std::string& url) {
        return owner + '\x1f' + url;
    }

    void rememberPendingCancelLocked(const std::string& owner,
                                     const std::string& url) {
        if (pending_cancels_.size() >= kMaxTrackedRequests * 2) {
            pending_cancels_.clear();
        }
        pending_cancels_.insert(pendingCancelKey(owner, url));
    }

    struct InFlightCall {
        std::shared_ptr<std::atomic<bool>> flag;
        std::string owner_request_id;
        std::string task_id;  // P24 C1②: best-effort tasks/cancel target
    };

    struct CancelledRequest {
        std::shared_ptr<std::atomic<bool>> flag;
        std::chrono::steady_clock::time_point time_point{};
    };

    static constexpr size_t kMaxTrackedRequests = 4096;
    static constexpr std::chrono::minutes kTokenTtl{10};

    std::mutex mutex_;
    std::unordered_multimap<std::string, InFlightCall> in_flight_calls_;
    std::unordered_map<std::string, CancelledRequest> cancelled_requests_;
    std::unordered_set<std::string> pending_cancels_;
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
