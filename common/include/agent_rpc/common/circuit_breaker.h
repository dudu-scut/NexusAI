#pragma once

#include "types.h"
#include <memory>
#include <string>
#include <map>
#include <mutex>
#include <atomic>
#include <chrono>

namespace agent_rpc {
namespace common {

// Circuit breaker states
enum class CircuitState {
    CLOSED,     // normal request flow
    OPEN,       // tripped, requests rejected
    HALF_OPEN   // probing recovery
};

// Circuit breaker configuration
struct CircuitBreakerConfig {
    int failure_threshold = 5;           // failures before tripping
    int success_threshold = 3;           // successes in half-open before closing
    std::chrono::milliseconds timeout = std::chrono::milliseconds(60000); // open timeout
    std::chrono::milliseconds half_open_timeout = std::chrono::milliseconds(30000); // half-open timeout
    double failure_rate_threshold = 0.5; // failure rate threshold
    int min_request_count = 10;          // minimum samples before computing failure rate
};

// Circuit breaker statistics
struct CircuitBreakerStats {
    int total_requests = 0;
    int successful_requests = 0;
    int failed_requests = 0;
    int rejected_requests = 0;
    std::chrono::steady_clock::time_point last_failure_time;
    std::chrono::steady_clock::time_point last_success_time;
    double current_failure_rate = 0.0;
};

// Circuit breaker with failure tracking and state transitions
class CircuitBreaker {
public:
    explicit CircuitBreaker(const CircuitBreakerConfig& config = CircuitBreakerConfig{});
    ~CircuitBreaker() = default;

    void recordSuccess();

    void recordFailure();

    bool isRequestAllowed();

    // Current state, read under lock to stay consistent with stats
    CircuitState getState() const {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        return state_;
    }

    CircuitBreakerStats getStats() const;

    void reset();

    void updateConfig(const CircuitBreakerConfig& config);

    const CircuitBreakerConfig& getConfig() const { return config_; }

private:
    void transitionToOpen();
    void transitionToHalfOpen();
    void transitionToClosed();
    void updateFailureRate();
    bool shouldAttemptReset();

    CircuitBreakerConfig config_;
    std::atomic<CircuitState> state_{CircuitState::CLOSED};
    mutable std::mutex stats_mutex_;
    CircuitBreakerStats stats_;
    std::chrono::steady_clock::time_point last_state_change_;
    // HALF_OPEN probe admission: reserved under stats_mutex_ so concurrent
    // callers cannot all pass the check before any of them records an
    // outcome (the old total_requests-based check allowed a probe storm).
    bool probe_in_flight_ = false;
};

class CircuitBreakerManager {
public:
    static CircuitBreakerManager& getInstance();
    
    std::shared_ptr<CircuitBreaker> getCircuitBreaker(const std::string& service_name);
    
    void removeCircuitBreaker(const std::string& service_name);
    
    std::map<std::string, CircuitState> getAllStates() const;
    
    void resetAll();
    
    void updateConfig(const std::string& service_name, const CircuitBreakerConfig& config);

private:
    CircuitBreakerManager() = default;
    ~CircuitBreakerManager() = default;
    CircuitBreakerManager(const CircuitBreakerManager&) = delete;
    CircuitBreakerManager& operator=(const CircuitBreakerManager&) = delete;

    mutable std::mutex circuit_breakers_mutex_;
    std::map<std::string, std::shared_ptr<CircuitBreaker>> circuit_breakers_;
};

} // namespace common
} // namespace agent_rpc
