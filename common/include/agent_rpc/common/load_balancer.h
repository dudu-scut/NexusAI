#pragma once

#include "types.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <random>
#include <algorithm>
#include <chrono>

namespace agent_rpc {
namespace common {

// Load balancing strategies
enum class LoadBalanceStrategy {
    ROUND_ROBIN,        // round robin
    RANDOM,             // random
    LEAST_CONNECTIONS,  // least connections
    WEIGHTED_ROUND_ROBIN, // weighted round robin
    CONSISTENT_HASH,    // consistent hash
    LEAST_RESPONSE_TIME // least response time
};

class LoadBalancer {
public:
    virtual ~LoadBalancer() = default;
    
    virtual ServiceEndpoint selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) = 0;
    
    virtual void updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) = 0;
    
    virtual void markEndpointStatus(const std::string& endpoint_id, bool healthy) = 0;
    
    virtual std::string getStrategyName() const = 0;
};

class RoundRobinLoadBalancer : public LoadBalancer {
public:
    RoundRobinLoadBalancer();
    ~RoundRobinLoadBalancer() = default;
    
    ServiceEndpoint selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) override;
    void updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) override;
    void markEndpointStatus(const std::string& endpoint_id, bool healthy) override;
    std::string getStrategyName() const override { return "RoundRobin"; }

private:
    std::atomic<size_t> current_index_{0};
    mutable std::mutex endpoints_mutex_;
    std::vector<ServiceEndpoint> healthy_endpoints_;
    std::map<std::string, bool> endpoint_health_;
};

class RandomLoadBalancer : public LoadBalancer {
public:
    RandomLoadBalancer();
    ~RandomLoadBalancer() = default;
    
    ServiceEndpoint selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) override;
    void updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) override;
    void markEndpointStatus(const std::string& endpoint_id, bool healthy) override;
    std::string getStrategyName() const override { return "Random"; }

private:
    mutable std::mutex endpoints_mutex_;
    std::vector<ServiceEndpoint> healthy_endpoints_;
    std::map<std::string, bool> endpoint_health_;
    std::random_device rd_;
    mutable std::mt19937 gen_;
};

class LeastConnectionsLoadBalancer : public LoadBalancer {
public:
    LeastConnectionsLoadBalancer();
    ~LeastConnectionsLoadBalancer() = default;

    ServiceEndpoint selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) override;
    void updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) override;
    void markEndpointStatus(const std::string& endpoint_id, bool healthy) override;
    std::string getStrategyName() const override { return "LeastConnections"; }

    void incrementConnections(const std::string& endpoint_id);
    void decrementConnections(const std::string& endpoint_id);

private:
    mutable std::mutex endpoints_mutex_;
    std::map<std::string, ServiceEndpoint> endpoints_;
    std::map<std::string, int> connection_counts_;
};

class WeightedRoundRobinLoadBalancer : public LoadBalancer {
public:
    WeightedRoundRobinLoadBalancer();
    ~WeightedRoundRobinLoadBalancer() = default;

    ServiceEndpoint selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) override;
    void updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) override;
    void markEndpointStatus(const std::string& endpoint_id, bool healthy) override;
    std::string getStrategyName() const override { return "WeightedRoundRobin"; }

private:
    struct WeightedEndpoint {
        ServiceEndpoint endpoint;
        int weight;
        int current_weight;
    };

    mutable std::mutex endpoints_mutex_;
    std::vector<WeightedEndpoint> weighted_endpoints_;
    std::atomic<size_t> current_index_{0};
};

class ConsistentHashLoadBalancer : public LoadBalancer {
public:
    ConsistentHashLoadBalancer(int virtual_nodes = 250);
    ~ConsistentHashLoadBalancer() = default;

    ServiceEndpoint selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) override;
    void updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) override;
    void markEndpointStatus(const std::string& endpoint_id, bool healthy) override;
    std::string getStrategyName() const override { return "ConsistentHash"; }

        // Select an endpoint by key
    ServiceEndpoint selectEndpointByKey(const std::string& key,
                                       const std::vector<ServiceEndpoint>& endpoints);

private:
    struct HashNode {
        std::string key;
        ServiceEndpoint endpoint;
        uint64_t hash;
    };

    void buildHashRing();
    uint64_t hash(const std::string& key) const;
    ServiceEndpoint findEndpoint(uint64_t hash_value);

    int virtual_nodes_;
    mutable std::mutex ring_mutex_;
    std::vector<HashNode> hash_ring_;
    std::map<std::string, ServiceEndpoint> endpoints_;
};

class LeastResponseTimeLoadBalancer : public LoadBalancer {
public:
    LeastResponseTimeLoadBalancer();
    ~LeastResponseTimeLoadBalancer() = default;

    ServiceEndpoint selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) override;
    void updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) override;
    void markEndpointStatus(const std::string& endpoint_id, bool healthy) override;
    std::string getStrategyName() const override { return "LeastResponseTime"; }

    void updateResponseTime(const std::string& endpoint_id,
                           std::chrono::milliseconds response_time);

private:
    struct EndpointStats {
        ServiceEndpoint endpoint;
        std::chrono::milliseconds avg_response_time{0};
        int request_count{0};
        std::chrono::steady_clock::time_point last_update;
    };

    mutable std::mutex stats_mutex_;
    std::map<std::string, EndpointStats> endpoint_stats_;
    std::chrono::milliseconds calculateAverageResponseTime(const std::string& endpoint_id);
};

// Factory creating load balancers by strategy
class LoadBalancerFactory {
public:
    static std::unique_ptr<LoadBalancer> createLoadBalancer(LoadBalanceStrategy strategy);
    static std::vector<std::string> getAvailableStrategies();
};

// Load balancer manager with runtime strategy switching
class LoadBalancerManager {
public:
    explicit LoadBalancerManager(LoadBalanceStrategy initial_strategy = LoadBalanceStrategy::ROUND_ROBIN);
    ~LoadBalancerManager() = default;

    // Switch strategy at runtime (thread-safe)
    void setStrategy(LoadBalanceStrategy strategy);

    LoadBalanceStrategy getCurrentStrategy() const;

    std::string getCurrentStrategyName() const;

    ServiceEndpoint selectEndpoint(const std::vector<ServiceEndpoint>& endpoints);

    // Deterministic key-based selection (R16): meaningful only for the
    // consistent-hash strategy — returns nullopt for any other active
    // strategy so the caller can fall back to selectEndpoint().
    std::optional<ServiceEndpoint> selectEndpointByKey(
        const std::string& key, const std::vector<ServiceEndpoint>& endpoints);

    void updateEndpoints(const std::vector<ServiceEndpoint>& endpoints);

    void markEndpointStatus(const std::string& endpoint_id, bool healthy);

private:
    mutable std::mutex mutex_;
    LoadBalanceStrategy current_strategy_;
    std::unique_ptr<LoadBalancer> load_balancer_;
};

} // namespace common
} // namespace agent_rpc
