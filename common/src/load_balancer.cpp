#include "agent_rpc/common/load_balancer.h"
#include "agent_rpc/common/logger.h"
#include <algorithm>
#include <random>
#include <set>
#include <climits>
#include <limits>

namespace agent_rpc {
namespace common {

RoundRobinLoadBalancer::RoundRobinLoadBalancer() = default;

ServiceEndpoint RoundRobinLoadBalancer::selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) {
    if (endpoints.empty()) {
        throw std::runtime_error("No endpoints available");
    }
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    std::vector<ServiceEndpoint> healthy;
    for (const auto& ep : endpoints) {
        std::string id = ep.host + ":" + std::to_string(ep.port);
        auto health_it = endpoint_health_.find(id);
        bool is_healthy = (health_it != endpoint_health_.end()) ? health_it->second : ep.is_healthy;
        if (is_healthy) healthy.push_back(ep);
    }
    if (healthy.empty()) throw std::runtime_error("No healthy endpoints available");
    size_t index = current_index_.fetch_add(1) % healthy.size();
    return healthy[index];
}

void RoundRobinLoadBalancer::updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    healthy_endpoints_ = endpoints;
    // Refresh health state: a stale unhealthy mark left by an earlier
    // markEndpointStatus call must not permanently exclude an endpoint that
    // the fresh snapshot reports as healthy.
    endpoint_health_.clear();
    for (const auto& ep : endpoints) {
        endpoint_health_[ep.host + ":" + std::to_string(ep.port)] = ep.is_healthy;
    }
    current_index_ = 0;
}

void RoundRobinLoadBalancer::markEndpointStatus(const std::string& endpoint_id, bool healthy) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    endpoint_health_[endpoint_id] = healthy;
    for (auto& ep : healthy_endpoints_) {
        if (ep.host + ":" + std::to_string(ep.port) == endpoint_id) {
            ep.is_healthy = healthy;
            break;
        }
    }
}

RandomLoadBalancer::RandomLoadBalancer() : gen_(rd_()) {}

ServiceEndpoint RandomLoadBalancer::selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) {
    if (endpoints.empty()) throw std::runtime_error("No endpoints available");
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    std::vector<ServiceEndpoint> healthy;
    for (const auto& ep : endpoints) {
        std::string id = ep.host + ":" + std::to_string(ep.port);
        auto health_it = endpoint_health_.find(id);
        bool is_healthy = (health_it != endpoint_health_.end()) ? health_it->second : ep.is_healthy;
        if (is_healthy) healthy.push_back(ep);
    }
    if (healthy.empty()) throw std::runtime_error("No healthy endpoints available");
    std::uniform_int_distribution<> dis(0, healthy.size() - 1);
    return healthy[dis(gen_)];
}

void RandomLoadBalancer::updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    healthy_endpoints_ = endpoints;
    endpoint_health_.clear();
    for (const auto& ep : endpoints) {
        endpoint_health_[ep.host + ":" + std::to_string(ep.port)] = ep.is_healthy;
    }
}

void RandomLoadBalancer::markEndpointStatus(const std::string& endpoint_id, bool healthy) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    endpoint_health_[endpoint_id] = healthy;
    for (auto& ep : healthy_endpoints_) {
        if (ep.host + ":" + std::to_string(ep.port) == endpoint_id) {
            ep.is_healthy = healthy;
            break;
        }
    }
}

LeastConnectionsLoadBalancer::LeastConnectionsLoadBalancer() = default;

ServiceEndpoint LeastConnectionsLoadBalancer::selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) {
    if (endpoints.empty()) throw std::runtime_error("No endpoints available");
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    std::string best_id;
    int min_conn = INT_MAX;
    for (const auto& ep : endpoints) {
        std::string id = ep.host + ":" + std::to_string(ep.port);
        // A markEndpointStatus(false) must hold even when the caller passes
        // a stale healthy bit — the internal record wins when present.
        auto known = endpoints_.find(id);
        bool is_healthy = (known != endpoints_.end()) ? known->second.is_healthy
                                                      : ep.is_healthy;
        if (!is_healthy) continue;
        int conn = connection_counts_[id];
        if (conn < min_conn) {
            min_conn = conn;
            best_id = id;
        }
    }
    if (best_id.empty()) throw std::runtime_error("No healthy endpoints available");
    connection_counts_[best_id]++;
    // Return copy from endpoints_ map (mutable) — avoids const_cast UB
    auto it = endpoints_.find(best_id);
    if (it != endpoints_.end()) return it->second;
    // Fallback: find in the input vector (shouldn't normally reach here)
    for (const auto& ep : endpoints) {
        std::string id = ep.host + ":" + std::to_string(ep.port);
        if (id == best_id) return ep;
    }
    throw std::runtime_error("No healthy endpoints available");
}

void LeastConnectionsLoadBalancer::updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    std::set<std::string> current;
    for (const auto& ep : endpoints) {
        current.insert(ep.host + ":" + std::to_string(ep.port));
    }
    auto it = connection_counts_.begin();
    while (it != connection_counts_.end()) {
        if (current.find(it->first) == current.end()) it = connection_counts_.erase(it);
        else ++it;
    }
    for (const auto& ep : endpoints) {
        endpoints_[ep.host + ":" + std::to_string(ep.port)] = ep;
    }
}

void LeastConnectionsLoadBalancer::markEndpointStatus(const std::string& endpoint_id, bool healthy) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    auto it = endpoints_.find(endpoint_id);
    if (it != endpoints_.end()) it->second.is_healthy = healthy;
}

void LeastConnectionsLoadBalancer::incrementConnections(const std::string& endpoint_id) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    connection_counts_[endpoint_id]++;
}

void LeastConnectionsLoadBalancer::decrementConnections(const std::string& endpoint_id) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    auto it = connection_counts_.find(endpoint_id);
    if (it != connection_counts_.end() && it->second > 0) it->second--;
}

WeightedRoundRobinLoadBalancer::WeightedRoundRobinLoadBalancer() = default;

ServiceEndpoint WeightedRoundRobinLoadBalancer::selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) {
    if (endpoints.empty()) throw std::runtime_error("No endpoints available");
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    if (weighted_endpoints_.empty()) throw std::runtime_error("No weighted endpoints available");
    WeightedEndpoint* best = nullptr;
    int max_weight = std::numeric_limits<int>::min();
    int total_weight = 0;
    for (auto& we : weighted_endpoints_) {
        if (!we.endpoint.is_healthy) continue;
        we.current_weight += we.weight;
        total_weight += we.weight;
        if (we.current_weight > max_weight) {
            max_weight = we.current_weight;
            best = &we;
        }
    }
    if (!best) throw std::runtime_error("No healthy endpoints available");
    best->current_weight -= total_weight;
    return best->endpoint;
}

void WeightedRoundRobinLoadBalancer::updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    weighted_endpoints_.clear();
    for (const auto& ep : endpoints) {
        WeightedEndpoint we;
        we.endpoint = ep;
        we.weight = 1;
        we.current_weight = 0;
        auto it = ep.metadata.find("weight");
        if (it != ep.metadata.end()) {
            try { we.weight = std::stoi(it->second); } catch (...) { we.weight = 1; }
        }
        weighted_endpoints_.push_back(we);
    }
    current_index_ = 0;
}

void WeightedRoundRobinLoadBalancer::markEndpointStatus(const std::string& endpoint_id, bool healthy) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    for (auto& we : weighted_endpoints_) {
        if (we.endpoint.host + ":" + std::to_string(we.endpoint.port) == endpoint_id) {
            we.endpoint.is_healthy = healthy;
            break;
        }
    }
}

ConsistentHashLoadBalancer::ConsistentHashLoadBalancer(int virtual_nodes) : virtual_nodes_(virtual_nodes) {}

ServiceEndpoint ConsistentHashLoadBalancer::selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) {
    if (endpoints.empty()) throw std::runtime_error("No endpoints available");
    std::lock_guard<std::mutex> lock(ring_mutex_);
    if (hash_ring_.empty()) throw std::runtime_error("Hash ring is empty");
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 1000000);
    uint64_t hash_value = hash(std::to_string(dis(gen)));
    return findEndpoint(hash_value);
}

ServiceEndpoint ConsistentHashLoadBalancer::selectEndpointByKey(const std::string& key,
                                                               const std::vector<ServiceEndpoint>& endpoints) {
    if (endpoints.empty()) throw std::runtime_error("No endpoints available");
    std::lock_guard<std::mutex> lock(ring_mutex_);
    if (hash_ring_.empty()) throw std::runtime_error("Hash ring is empty");
    return findEndpoint(hash(key));
}

void ConsistentHashLoadBalancer::updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    endpoints_.clear();
    for (const auto& ep : endpoints) {
        endpoints_[ep.host + ":" + std::to_string(ep.port)] = ep;
    }
    buildHashRing();
}

void ConsistentHashLoadBalancer::markEndpointStatus(const std::string& endpoint_id, bool healthy) {
    std::lock_guard<std::mutex> lock(ring_mutex_);
    auto it = endpoints_.find(endpoint_id);
    if (it != endpoints_.end()) {
        it->second.is_healthy = healthy;
        buildHashRing();
    }
}

void ConsistentHashLoadBalancer::buildHashRing() {
    hash_ring_.clear();
    for (const auto& pair : endpoints_) {
        if (!pair.second.is_healthy) continue;
        for (int i = 0; i < virtual_nodes_; ++i) {
            std::string vkey = std::to_string(i) + ":" + pair.first;
            HashNode node;
            node.key = vkey;
            node.endpoint = pair.second;
            node.hash = hash(vkey);
            hash_ring_.push_back(node);
        }
    }
    std::sort(hash_ring_.begin(), hash_ring_.end(),
              [](const HashNode& a, const HashNode& b) { return a.hash < b.hash; });
}

uint64_t ConsistentHashLoadBalancer::hash(const std::string& key) const {
    // FNV-1a hash algorithm
    uint64_t hash = 14695981039346656037ULL;  // FNV offset basis
    for (char c : key) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;  // FNV prime
    }
    return hash;
}

ServiceEndpoint ConsistentHashLoadBalancer::findEndpoint(uint64_t hash_value) {
    if (hash_ring_.empty()) throw std::runtime_error("Hash ring is empty");
    auto it = std::lower_bound(hash_ring_.begin(), hash_ring_.end(), hash_value,
                              [](const HashNode& node, uint64_t value) { return node.hash < value; });
    if (it == hash_ring_.end()) it = hash_ring_.begin();
    return it->endpoint;
}

LeastResponseTimeLoadBalancer::LeastResponseTimeLoadBalancer(double ema_alpha)
    : ema_alpha_(ema_alpha > 0.0 && ema_alpha <= 1.0 ? ema_alpha : 0.1) {}


ServiceEndpoint LeastResponseTimeLoadBalancer::selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) {
    if (endpoints.empty()) throw std::runtime_error("No endpoints available");
    std::lock_guard<std::mutex> lock(stats_mutex_);
    std::string best_unknown_id;
    std::string best_known_id;
    std::chrono::milliseconds min_time = std::chrono::milliseconds::max();
    for (const auto& ep : endpoints) {
        if (!ep.is_healthy) continue;
        std::string id = ep.host + ":" + std::to_string(ep.port);
        auto it = endpoint_stats_.find(id);
        if (it != endpoint_stats_.end() && !it->second.endpoint.is_healthy) {
            continue;  // marked unhealthy via markEndpointStatus
        }
        if (it == endpoint_stats_.end()) {
            // Prefer unknown endpoints (exploration), fallback to fastest known
            if (best_unknown_id.empty()) best_unknown_id = id;
        } else {
            auto rt = calculateAverageResponseTime(id);
            if (rt < min_time) {
                min_time = rt;
                best_known_id = id;
            }
        }
    }
    if (!best_unknown_id.empty()) {
        for (const auto& ep : endpoints) {
            if (ep.host + ":" + std::to_string(ep.port) == best_unknown_id) return ep;
        }
    }
    if (!best_known_id.empty()) {
        for (const auto& ep : endpoints) {
            if (ep.host + ":" + std::to_string(ep.port) == best_known_id) return ep;
        }
    }
    throw std::runtime_error("No healthy endpoints available");
}

void LeastResponseTimeLoadBalancer::updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    std::set<std::string> current;
    for (const auto& ep : endpoints) current.insert(ep.host + ":" + std::to_string(ep.port));
    auto it = endpoint_stats_.begin();
    while (it != endpoint_stats_.end()) {
        if (current.find(it->first) == current.end()) it = endpoint_stats_.erase(it);
        else ++it;
    }
}

void LeastResponseTimeLoadBalancer::markEndpointStatus(const std::string& endpoint_id, bool healthy) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    // Create the entry if missing so the mark survives until updateEndpoints.
    endpoint_stats_[endpoint_id].endpoint.is_healthy = healthy;
}

void LeastResponseTimeLoadBalancer::updateResponseTime(const std::string& endpoint_id,
                                                      std::chrono::milliseconds response_time) {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    auto& stats = endpoint_stats_[endpoint_id];
    const auto previous = stats.ema_response_time_ms;
    if (previous.count() == 0) {
        // First sample seeds the EMA directly (no history to smooth yet).
        stats.ema_response_time_ms = response_time;
    } else {
        // P8 S1: s = α·x + (1−α)·s — a single outlier moves the estimate by
        // α of its distance instead of replacing it (latest-sample selection
        // used to oscillate on one slow response).
        stats.ema_response_time_ms = std::chrono::milliseconds(static_cast<std::int64_t>(
            ema_alpha_ * static_cast<double>(response_time.count()) +
            (1.0 - ema_alpha_) * static_cast<double>(previous.count())));
    }
    stats.request_count++;
    stats.last_update = std::chrono::steady_clock::now();
}

std::chrono::milliseconds LeastResponseTimeLoadBalancer::calculateAverageResponseTime(const std::string& endpoint_id) {
    auto it = endpoint_stats_.find(endpoint_id);
    if (it == endpoint_stats_.end()) return std::chrono::milliseconds(1000);
    return it->second.ema_response_time_ms;
}

std::unique_ptr<LoadBalancer> LoadBalancerFactory::createLoadBalancer(LoadBalanceStrategy strategy) {
    switch (strategy) {
        case LoadBalanceStrategy::ROUND_ROBIN: return std::make_unique<RoundRobinLoadBalancer>();
        case LoadBalanceStrategy::RANDOM: return std::make_unique<RandomLoadBalancer>();
        case LoadBalanceStrategy::LEAST_CONNECTIONS: return std::make_unique<LeastConnectionsLoadBalancer>();
        case LoadBalanceStrategy::WEIGHTED_ROUND_ROBIN: return std::make_unique<WeightedRoundRobinLoadBalancer>();
        case LoadBalanceStrategy::CONSISTENT_HASH: return std::make_unique<ConsistentHashLoadBalancer>();
        case LoadBalanceStrategy::LEAST_RESPONSE_TIME: return std::make_unique<LeastResponseTimeLoadBalancer>();
        default: return std::make_unique<RoundRobinLoadBalancer>();
    }
}

std::vector<std::string> LoadBalancerFactory::getAvailableStrategies() {
    return {"RoundRobin", "Random", "LeastConnections", "WeightedRoundRobin", "ConsistentHash", "LeastResponseTime"};
}

LoadBalancerManager::LoadBalancerManager(LoadBalanceStrategy initial_strategy)
    : current_strategy_(initial_strategy),
      load_balancer_(LoadBalancerFactory::createLoadBalancer(initial_strategy)) {}

void LoadBalancerManager::setStrategy(LoadBalanceStrategy strategy) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (strategy != current_strategy_) {
        current_strategy_ = strategy;
        load_balancer_ = LoadBalancerFactory::createLoadBalancer(strategy);
    }
}

LoadBalanceStrategy LoadBalancerManager::getCurrentStrategy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_strategy_;
}

std::string LoadBalancerManager::getCurrentStrategyName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return load_balancer_ ? load_balancer_->getStrategyName() : "None";
}

std::optional<ServiceEndpoint> LoadBalancerManager::selectEndpointByKey(
    const std::string& key, const std::vector<ServiceEndpoint>& endpoints) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_strategy_ != LoadBalanceStrategy::CONSISTENT_HASH) {
        return std::nullopt;
    }
    auto* consistent_hash = dynamic_cast<ConsistentHashLoadBalancer*>(load_balancer_.get());
    if (!consistent_hash) {
        return std::nullopt;
    }
    return consistent_hash->selectEndpointByKey(key, endpoints);
}

ServiceEndpoint LoadBalancerManager::selectEndpoint(const std::vector<ServiceEndpoint>& endpoints) {
    std::lock_guard<std::mutex> lock(mutex_);
    return load_balancer_->selectEndpoint(endpoints);
}

void LoadBalancerManager::updateEndpoints(const std::vector<ServiceEndpoint>& endpoints) {
    std::lock_guard<std::mutex> lock(mutex_);
    load_balancer_->updateEndpoints(endpoints);
}

void LoadBalancerManager::markEndpointStatus(const std::string& endpoint_id, bool healthy) {
    std::lock_guard<std::mutex> lock(mutex_);
    load_balancer_->markEndpointStatus(endpoint_id, healthy);
}

void LoadBalancerManager::recordResponseTime(const std::string& endpoint_id,
                                             std::chrono::milliseconds response_time) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (load_balancer_) {
        load_balancer_->updateResponseTime(endpoint_id, response_time);
    }
}

} // namespace common
} // namespace agent_rpc
