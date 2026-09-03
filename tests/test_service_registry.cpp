#include "agent_rpc/registry/service_registry.h"
#include "agent_rpc/common/load_balancer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace agent_rpc::tests {

namespace {

common::ServiceEndpoint makeEndpoint(const std::string& host,
                                     int port,
                                     const std::string& service_name = "rpc") {
    common::ServiceEndpoint endpoint;
    endpoint.host = host;
    endpoint.port = port;
    endpoint.service_name = service_name;
    endpoint.version = "1.0.0";
    endpoint.is_healthy = true;
    return endpoint;
}

}  // namespace

TEST(ServiceRegistryTest, MemoryRegistrySupportsRegisterDiscoverHeartbeatUnregister) {
    registry::MemoryServiceRegistry registry;
    auto endpoint = makeEndpoint("127.0.0.1", 5001, "rpc_server");
    const std::string service_id = endpoint.host + ":" + std::to_string(endpoint.port);

    EXPECT_TRUE(registry.registerService(endpoint));

    auto discovered = registry.discoverServices("rpc_server");
    ASSERT_EQ(discovered.size(), 1u);
    EXPECT_EQ(discovered.front().port, 5001);
    EXPECT_TRUE(registry.isServiceHealthy(service_id));
    EXPECT_TRUE(registry.updateHeartbeat(service_id));

    EXPECT_TRUE(registry.unregisterService(service_id));
    EXPECT_TRUE(registry.discoverServices("rpc_server").empty());
}

TEST(ServiceRegistryTest, MemoryRegistryNotifiesWatcherOnChanges) {
    registry::MemoryServiceRegistry registry;
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<size_t> callback_sizes;

    registry.watchServices("rpc_server", [&](const std::vector<common::ServiceEndpoint>& endpoints) {
        std::lock_guard<std::mutex> lock(mutex);
        callback_sizes.push_back(endpoints.size());
        cv.notify_all();
    });

    auto endpoint = makeEndpoint("127.0.0.1", 5001, "rpc_server");
    const std::string service_id = endpoint.host + ":" + std::to_string(endpoint.port);

    EXPECT_TRUE(registry.registerService(endpoint));
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(1), [&] {
            return callback_sizes.size() >= 1;
        }));
    }

    EXPECT_TRUE(registry.unregisterService(service_id));
    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(1), [&] {
            return callback_sizes.size() >= 2;
        }));
    }

    ASSERT_EQ(callback_sizes.size(), 2u);
    EXPECT_EQ(callback_sizes[0], 1u);
    EXPECT_EQ(callback_sizes[1], 0u);
}

TEST(ServiceRegistryTest, RegistryResultsCanFeedLoadBalancer) {
    registry::MemoryServiceRegistry registry;
    registry.registerService(makeEndpoint("127.0.0.1", 5001, "rpc_server"));
    registry.registerService(makeEndpoint("127.0.0.1", 5002, "rpc_server"));

    auto discovered = registry.discoverServices("rpc_server");
    ASSERT_EQ(discovered.size(), 2u);

    auto load_balancer =
        common::LoadBalancerFactory::createLoadBalancer(common::LoadBalanceStrategy::ROUND_ROBIN);

    auto first = load_balancer->selectEndpoint(discovered);
    auto second = load_balancer->selectEndpoint(discovered);

    EXPECT_NE(first.port, second.port);
}

// ── P14: live-metrics health evaluation (batch 7) ────────────────────────

TEST(ServiceRegistryTest, HealthTiersFollowSuccessRateAndLatencyBoundaries) {
    // 19/20 success + fast → HEALTHY (0.95 / <5000ms).
    for (int i = 0; i < 19; ++i) {
        registry::ServiceRegistry::recordAgentCall("healthy-agent", true, 100.0);
    }
    registry::ServiceRegistry::recordAgentCall("healthy-agent", false, 100.0);
    EXPECT_EQ(registry::ServiceRegistry::evaluateHealth("healthy-agent"),
              registry::HealthStatus::HEALTHY);

    // 16/20 success = 0.80 with moderate latency → DEGRADED.
    for (int i = 0; i < 16; ++i) {
        registry::ServiceRegistry::recordAgentCall("degraded-agent", true, 1000.0);
    }
    for (int i = 0; i < 4; ++i) {
        registry::ServiceRegistry::recordAgentCall("degraded-agent", false, 1000.0);
    }
    EXPECT_EQ(registry::ServiceRegistry::evaluateHealth("degraded-agent"),
              registry::HealthStatus::DEGRADED);

    // High success but slow (>5000ms) → not HEALTHY.
    for (int i = 0; i < 20; ++i) {
        registry::ServiceRegistry::recordAgentCall("slow-agent", true, 8000.0);
    }
    EXPECT_EQ(registry::ServiceRegistry::evaluateHealth("slow-agent"),
              registry::HealthStatus::DEGRADED);

    // Total failure → UNHEALTHY.
    for (int i = 0; i < 20; ++i) {
        registry::ServiceRegistry::recordAgentCall("dead-agent", false, 100.0);
    }
    EXPECT_EQ(registry::ServiceRegistry::evaluateHealth("dead-agent"),
              registry::HealthStatus::UNHEALTHY);

    // Unknown agent → UNKNOWN.
    EXPECT_EQ(registry::ServiceRegistry::evaluateHealth("never-seen"),
              registry::HealthStatus::UNKNOWN);
}

TEST(ServiceRegistryTest, EvaluateAllHealthDeliversCallbackPerEvaluatedAgent) {
    for (int i = 0; i < 20; ++i) {
        registry::ServiceRegistry::recordAgentCall("good", true, 100.0);
    }
    for (int i = 0; i < 20; ++i) {
        registry::ServiceRegistry::recordAgentCall("bad", false, 100.0);
    }

    std::map<std::string, registry::HealthStatus> seen;
    registry::ServiceRegistry::evaluateAllHealth(
        [&seen](const std::string& agent_id, registry::HealthStatus status) {
            seen[agent_id] = status;
        });

    ASSERT_EQ(seen.count("good"), 1u);
    EXPECT_EQ(seen.at("good"), registry::HealthStatus::HEALTHY);
    ASSERT_EQ(seen.count("bad"), 1u);
    EXPECT_EQ(seen.at("bad"), registry::HealthStatus::UNHEALTHY);
}

TEST(ServiceRegistryTest, EvaluateAllHealthSkipsColdStartAgents) {
    // Only 5 samples — below the kMinHealthSamples gate: the agent must not
    // be evaluated (cold-start protection). live_metrics_ is process-global
    // and shared across tests in this binary, so assert on the cold-start
    // agent's absence from the callback set rather than a global count.
    for (int i = 0; i < 5; ++i) {
        registry::ServiceRegistry::recordAgentCall("cold-start", false, 100.0);
    }

    std::vector<std::string> evaluated;
    registry::ServiceRegistry::evaluateAllHealth(
        [&evaluated](const std::string& agent_id, registry::HealthStatus) {
            evaluated.push_back(agent_id);
        });

    EXPECT_EQ(std::find(evaluated.begin(), evaluated.end(), "cold-start"),
              evaluated.end());
}

TEST(ServiceRegistryTest, HeartbeatTimeoutDetection) {
    registry::ServiceRegistry::recordHeartbeat("heartbeat-agent");

    // Fresh heartbeat: not timed out under a 90s threshold.
    std::map<std::string, bool> verdict_90s;
    registry::ServiceRegistry::evaluateHeartbeatTimeouts(
        std::chrono::seconds(90),
        [&verdict_90s](const std::string& agent_id, bool t) {
            verdict_90s[agent_id] = t;
        });
    ASSERT_EQ(verdict_90s.count("heartbeat-agent"), 1u);
    EXPECT_FALSE(verdict_90s.at("heartbeat-agent"));

    // A zero threshold flips the verdict once any time has passed.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::map<std::string, bool> verdict_zero;
    registry::ServiceRegistry::evaluateHeartbeatTimeouts(
        std::chrono::seconds(0),
        [&verdict_zero](const std::string& agent_id, bool t) {
            verdict_zero[agent_id] = t;
        });
    ASSERT_EQ(verdict_zero.count("heartbeat-agent"), 1u);
    EXPECT_TRUE(verdict_zero.at("heartbeat-agent"));
}

TEST(ServiceRegistryTest, RecordHeartbeatDoesNotDisturbCallMetrics) {
    for (int i = 0; i < 19; ++i) {
        registry::ServiceRegistry::recordAgentCall("mixed", true, 100.0);
    }
    registry::ServiceRegistry::recordAgentCall("mixed", false, 100.0);
    registry::ServiceRegistry::recordHeartbeat("mixed");

    // Heartbeat refresh must not touch the success buffer / EMA.
    EXPECT_EQ(registry::ServiceRegistry::evaluateHealth("mixed"),
              registry::HealthStatus::HEALTHY);
}

}  // namespace agent_rpc::tests
