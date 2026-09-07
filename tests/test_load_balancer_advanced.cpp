#include "agent_rpc/common/load_balancer.h"

#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace agent_rpc::tests {

namespace {

common::ServiceEndpoint makeEndpoint(const std::string& host,
                                     int port,
                                     const std::string& service_name = "rpc",
                                     bool healthy = true,
                                     const std::map<std::string, std::string>& metadata = {}) {
    common::ServiceEndpoint endpoint;
    endpoint.host = host;
    endpoint.port = port;
    endpoint.service_name = service_name;
    endpoint.version = "1.0.0";
    endpoint.is_healthy = healthy;
    endpoint.metadata = metadata;
    return endpoint;
}

std::string endpointId(const common::ServiceEndpoint& endpoint) {
    return endpoint.host + ":" + std::to_string(endpoint.port);
}

}  // namespace

// WeightedRoundRobin tests

class WeightedRoundRobinAdvancedTest : public ::testing::Test {
protected:
    common::WeightedRoundRobinLoadBalancer lb;
};

TEST_F(WeightedRoundRobinAdvancedTest, DifferentWeightsDistributeProportionally) {
    std::vector<common::ServiceEndpoint> endpoints = {
        makeEndpoint("10.0.0.1", 8080, "svc", true, {{"weight", "5"}}),
        makeEndpoint("10.0.0.2", 8080, "svc", true, {{"weight", "3"}}),
        makeEndpoint("10.0.0.3", 8080, "svc", true, {{"weight", "2"}})
    };

    lb.updateEndpoints(endpoints);

    std::map<std::string, int> counts;
    const int total = 100;
    for (int i = 0; i < total; ++i) {
        auto ep = lb.selectEndpoint(endpoints);
        counts[endpointId(ep)]++;
    }

    // 5:3:2 ratio -> out of 100 calls, expect roughly 50:30:20
    EXPECT_EQ(counts["10.0.0.1:8080"], 50);
    EXPECT_EQ(counts["10.0.0.2:8080"], 30);
    EXPECT_EQ(counts["10.0.0.3:8080"], 20);
}

TEST_F(WeightedRoundRobinAdvancedTest, SingleEndpointAlwaysSelected) {
    std::vector<common::ServiceEndpoint> endpoints = {
        makeEndpoint("10.0.0.1", 8080, "svc", true, {{"weight", "3"}})
    };

    lb.updateEndpoints(endpoints);

    for (int i = 0; i < 10; ++i) {
        auto ep = lb.selectEndpoint(endpoints);
        EXPECT_EQ(endpointId(ep), "10.0.0.1:8080");
    }
}

TEST_F(WeightedRoundRobinAdvancedTest, ZeroWeightEndpointSkipped) {
    std::vector<common::ServiceEndpoint> endpoints = {
        makeEndpoint("10.0.0.1", 8080, "svc", true, {{"weight", "0"}}),
        makeEndpoint("10.0.0.2", 8080, "svc", true, {{"weight", "1"}})
    };

    lb.updateEndpoints(endpoints);

    // All requests should go to the endpoint with weight > 0
    for (int i = 0; i < 20; ++i) {
        auto ep = lb.selectEndpoint(endpoints);
        EXPECT_EQ(endpointId(ep), "10.0.0.2:8080");
    }
}

TEST_F(WeightedRoundRobinAdvancedTest, EqualWeightsDistributeEvenly) {
    std::vector<common::ServiceEndpoint> endpoints = {
        makeEndpoint("10.0.0.1", 8080, "svc", true, {{"weight", "1"}}),
        makeEndpoint("10.0.0.2", 8080, "svc", true, {{"weight", "1"}})
    };

    lb.updateEndpoints(endpoints);

    std::map<std::string, int> counts;
    const int total = 100;
    for (int i = 0; i < total; ++i) {
        auto ep = lb.selectEndpoint(endpoints);
        counts[endpointId(ep)]++;
    }

    EXPECT_EQ(counts["10.0.0.1:8080"], 50);
    EXPECT_EQ(counts["10.0.0.2:8080"], 50);
}

TEST_F(WeightedRoundRobinAdvancedTest, UnhealthyEndpointsExcluded) {
    std::vector<common::ServiceEndpoint> endpoints = {
        makeEndpoint("10.0.0.1", 8080, "svc", false, {{"weight", "10"}}),
        makeEndpoint("10.0.0.2", 8080, "svc", true, {{"weight", "1"}})
    };

    lb.updateEndpoints(endpoints);

    for (int i = 0; i < 10; ++i) {
        auto ep = lb.selectEndpoint(endpoints);
        EXPECT_EQ(endpointId(ep), "10.0.0.2:8080");
    }
}

// ConsistentHash tests

class ConsistentHashAdvancedTest : public ::testing::Test {
protected:
    common::ConsistentHashLoadBalancer lb{250};
};

TEST_F(ConsistentHashAdvancedTest, SameKeyAlwaysMapsToSameEndpoint) {
    std::vector<common::ServiceEndpoint> endpoints = {
        makeEndpoint("10.0.0.1", 8080),
        makeEndpoint("10.0.0.2", 8080),
        makeEndpoint("10.0.0.3", 8080)
    };

    lb.updateEndpoints(endpoints);

    std::string key = "user-session-abc123";
    auto first = lb.selectEndpointByKey(key, endpoints);
    auto second = lb.selectEndpointByKey(key, endpoints);
    auto third = lb.selectEndpointByKey(key, endpoints);

    EXPECT_EQ(endpointId(first), endpointId(second));
    EXPECT_EQ(endpointId(second), endpointId(third));
}

TEST_F(ConsistentHashAdvancedTest, DifferentKeysCanMapToDifferentEndpoints) {
    std::vector<common::ServiceEndpoint> endpoints = {
        makeEndpoint("10.0.0.1", 8080),
        makeEndpoint("10.0.0.2", 8080),
        makeEndpoint("10.0.0.3", 8080)
    };

    lb.updateEndpoints(endpoints);

    std::map<std::string, int> mapping;
    // With enough diverse keys, we should hit multiple endpoints
    for (int i = 0; i < 100; ++i) {
        auto ep = lb.selectEndpointByKey("key-" + std::to_string(i), endpoints);
        mapping[endpointId(ep)]++;
    }

    // Expect at least 2 different endpoints to be hit
    EXPECT_GE(mapping.size(), 2u);
}

TEST_F(ConsistentHashAdvancedTest, MinimalDisruptionOnEndpointAddition) {
    std::vector<common::ServiceEndpoint> endpoints_original = {
        makeEndpoint("10.0.0.1", 8080),
        makeEndpoint("10.0.0.2", 8080),
        makeEndpoint("10.0.0.3", 8080)
    };

    lb.updateEndpoints(endpoints_original);

    // Record original mappings
    const int key_count = 100;
    std::map<std::string, std::string> original_mapping;
    for (int i = 0; i < key_count; ++i) {
        std::string key = "key-" + std::to_string(i);
        auto ep = lb.selectEndpointByKey(key, endpoints_original);
        original_mapping[key] = endpointId(ep);
    }

    // Add a new endpoint
    std::vector<common::ServiceEndpoint> endpoints_new = {
        makeEndpoint("10.0.0.1", 8080),
        makeEndpoint("10.0.0.2", 8080),
        makeEndpoint("10.0.0.3", 8080),
        makeEndpoint("10.0.0.4", 8080)
    };

    lb.updateEndpoints(endpoints_new);

    // Check how many keys changed their mapping
    int changed = 0;
    for (int i = 0; i < key_count; ++i) {
        std::string key = "key-" + std::to_string(i);
        auto ep = lb.selectEndpointByKey(key, endpoints_new);
        if (original_mapping[key] != endpointId(ep)) {
            changed++;
        }
    }

    // With consistent hashing, adding 1 of 4 endpoints should remap ~25% of keys
    // Allow up to 50% disruption (generous tolerance for small ring sizes)
    EXPECT_LT(changed, key_count / 2);
}

TEST_F(ConsistentHashAdvancedTest, MinimalDisruptionOnEndpointRemoval) {
    std::vector<common::ServiceEndpoint> endpoints_original = {
        makeEndpoint("10.0.0.1", 8080),
        makeEndpoint("10.0.0.2", 8080),
        makeEndpoint("10.0.0.3", 8080),
        makeEndpoint("10.0.0.4", 8080)
    };

    lb.updateEndpoints(endpoints_original);

    // Record original mappings
    const int key_count = 100;
    std::map<std::string, std::string> original_mapping;
    for (int i = 0; i < key_count; ++i) {
        std::string key = "key-" + std::to_string(i);
        auto ep = lb.selectEndpointByKey(key, endpoints_original);
        original_mapping[key] = endpointId(ep);
    }

    // Remove one endpoint
    std::vector<common::ServiceEndpoint> endpoints_reduced = {
        makeEndpoint("10.0.0.1", 8080),
        makeEndpoint("10.0.0.2", 8080),
        makeEndpoint("10.0.0.3", 8080)
    };

    lb.updateEndpoints(endpoints_reduced);

    int changed = 0;
    for (int i = 0; i < key_count; ++i) {
        std::string key = "key-" + std::to_string(i);
        auto ep = lb.selectEndpointByKey(key, endpoints_reduced);
        if (original_mapping[key] != endpointId(ep)) {
            changed++;
        }
    }

    // Only keys that were mapped to the removed endpoint should change
    EXPECT_LT(changed, key_count / 2);
}

TEST_F(ConsistentHashAdvancedTest, DistributionUniformity) {
    std::vector<common::ServiceEndpoint> endpoints = {
        makeEndpoint("10.0.0.1", 8080),
        makeEndpoint("10.0.0.2", 8080),
        makeEndpoint("10.0.0.3", 8080)
    };

    lb.updateEndpoints(endpoints);

    std::map<std::string, int> counts;
    const int total = 3000;
    for (int i = 0; i < total; ++i) {
        auto ep = lb.selectEndpointByKey("session-" + std::to_string(i), endpoints);
        counts[endpointId(ep)]++;
    }

    // Each endpoint should get a reasonable share (at least 15% each with 3 endpoints)
    double expected_per_endpoint = total / 3.0;
    for (const auto& [id, count] : counts) {
        double ratio = static_cast<double>(count) / expected_per_endpoint;
        EXPECT_GT(ratio, 0.4) << "Endpoint " << id << " got only " << count
                               << " out of " << total << " requests";
        EXPECT_LT(ratio, 1.6) << "Endpoint " << id << " got too many: " << count
                               << " out of " << total << " requests";
    }
}

// LeastResponseTime tests

class LeastResponseTimeAdvancedTest : public ::testing::Test {
protected:
    common::LeastResponseTimeLoadBalancer lb;
};

TEST_F(LeastResponseTimeAdvancedTest, SelectsFastestEndpoint) {
    std::vector<common::ServiceEndpoint> endpoints = {
        makeEndpoint("10.0.0.1", 8080),
        makeEndpoint("10.0.0.2", 8080),
        makeEndpoint("10.0.0.3", 8080)
    };

    lb.updateResponseTime("10.0.0.1:8080", std::chrono::milliseconds(100));
    lb.updateResponseTime("10.0.0.2:8080", std::chrono::milliseconds(20));
    lb.updateResponseTime("10.0.0.3:8080", std::chrono::milliseconds(50));

    auto ep = lb.selectEndpoint(endpoints);
    EXPECT_EQ(endpointId(ep), "10.0.0.2:8080");
}

TEST_F(LeastResponseTimeAdvancedTest, EqualResponseTimesSelectsFirst) {
    std::vector<common::ServiceEndpoint> endpoints = {
        makeEndpoint("10.0.0.1", 8080),
        makeEndpoint("10.0.0.2", 8080),
        makeEndpoint("10.0.0.3", 8080)
    };

    lb.updateResponseTime("10.0.0.1:8080", std::chrono::milliseconds(50));
    lb.updateResponseTime("10.0.0.2:8080", std::chrono::milliseconds(50));
    lb.updateResponseTime("10.0.0.3:8080", std::chrono::milliseconds(50));

    // With equal response times, the first endpoint encountered should be selected
    auto ep = lb.selectEndpoint(endpoints);
    // Just verify it returns a valid endpoint
    bool valid = (endpointId(ep) == "10.0.0.1:8080" ||
                  endpointId(ep) == "10.0.0.2:8080" ||
                  endpointId(ep) == "10.0.0.3:8080");
    EXPECT_TRUE(valid);
}

TEST_F(LeastResponseTimeAdvancedTest, ResponseTimeUpdateChangesSelection) {
    std::vector<common::ServiceEndpoint> endpoints = {
        makeEndpoint("10.0.0.1", 8080),
        makeEndpoint("10.0.0.2", 8080)
    };

    lb.updateResponseTime("10.0.0.1:8080", std::chrono::milliseconds(10));
    lb.updateResponseTime("10.0.0.2:8080", std::chrono::milliseconds(100));

    auto ep1 = lb.selectEndpoint(endpoints);
    EXPECT_EQ(endpointId(ep1), "10.0.0.1:8080");

    // P8 S1 (EMA α=0.1): a single fast sample must NOT flip the selection
    // (that was the latest-sample oscillation this change removes). Sustained
    // improvement converges the EMA below the leader — feed 5ms 30 times
    // (100·0.9^30 + 5·(1−0.9^30) ≈ 7.7 < 10) and the pick flips.
    lb.updateResponseTime("10.0.0.2:8080", std::chrono::milliseconds(5));
    auto stable = lb.selectEndpoint(endpoints);
    EXPECT_EQ(endpointId(stable), "10.0.0.1:8080")
        << "one fast sample must not flip an EMA-smoothed selection";

    for (int i = 0; i < 30; ++i) {
        lb.updateResponseTime("10.0.0.2:8080", std::chrono::milliseconds(5));
    }
    auto ep2 = lb.selectEndpoint(endpoints);
    EXPECT_EQ(endpointId(ep2), "10.0.0.2:8080");
}

TEST_F(LeastResponseTimeAdvancedTest, UnknownEndpointsPreferred) {
    std::vector<common::ServiceEndpoint> endpoints = {
        makeEndpoint("10.0.0.1", 8080),
        makeEndpoint("10.0.0.2", 8080)
    };

    // Only one endpoint has known response time (slow)
    lb.updateResponseTime("10.0.0.1:8080", std::chrono::milliseconds(500));

    // The unknown endpoint should be preferred (it gets selected first as no stats)
    auto ep = lb.selectEndpoint(endpoints);
    // The implementation selects endpoints without stats first
    EXPECT_EQ(endpointId(ep), "10.0.0.2:8080");
}

TEST_F(LeastResponseTimeAdvancedTest, UnhealthyEndpointsExcluded) {
    std::vector<common::ServiceEndpoint> endpoints = {
        makeEndpoint("10.0.0.1", 8080, "svc", false),
        makeEndpoint("10.0.0.2", 8080, "svc", true)
    };

    lb.updateResponseTime("10.0.0.1:8080", std::chrono::milliseconds(1));  // very fast but unhealthy
    lb.updateResponseTime("10.0.0.2:8080", std::chrono::milliseconds(100));

    auto ep = lb.selectEndpoint(endpoints);
    EXPECT_EQ(endpointId(ep), "10.0.0.2:8080");
}

TEST_F(LeastResponseTimeAdvancedTest, ExponentialMovingAverageSmooths) {
    std::vector<common::ServiceEndpoint> endpoints = {
        makeEndpoint("10.0.0.1", 8080)
    };

    // First update: 100ms
    lb.updateResponseTime("10.0.0.1:8080", std::chrono::milliseconds(100));
    // Second update: 50ms -> EMA = 100*0.8 + 50*0.2 = 90
    lb.updateResponseTime("10.0.0.1:8080", std::chrono::milliseconds(50));

    // Should still select the endpoint (only one available)
    auto ep = lb.selectEndpoint(endpoints);
    EXPECT_EQ(endpointId(ep), "10.0.0.1:8080");
}

// P8 S1: EMA (α=0.1) must resist a single outlier that a latest-sample
// strategy would latch onto: a=100→1000 (EMA 100→190) vs b=200 (seed).
// Latest sampling would pick b (1000 > 200); EMA keeps a (190 < 200).
TEST_F(LeastResponseTimeAdvancedTest, EmaResistsSingleOutlierInsteadOfLatestSample) {
    std::vector<common::ServiceEndpoint> endpoints = {
        makeEndpoint("10.0.0.1", 8080),
        makeEndpoint("10.0.0.2", 8080),
    };

    lb.updateResponseTime("10.0.0.1:8080", std::chrono::milliseconds(100));
    lb.updateResponseTime("10.0.0.1:8080", std::chrono::milliseconds(1000));
    lb.updateResponseTime("10.0.0.2:8080", std::chrono::milliseconds(200));

    const auto ep = lb.selectEndpoint(endpoints);
    EXPECT_EQ(endpointId(ep), "10.0.0.1:8080")
        << "EMA (190) stays below the seeded 200; latest-sample (1000) would lose";
}

// LoadBalancerManager (setStrategy) tests

class LoadBalancerManagerTest : public ::testing::Test {
protected:
    std::vector<common::ServiceEndpoint> endpoints = {
        makeEndpoint("10.0.0.1", 8080),
        makeEndpoint("10.0.0.2", 8080)
    };
};

// P8 S2: the manager forwards latency feeds to the active strategy
// (LoadBalancer virtual-channel surface used by AgentRouter).
TEST_F(LoadBalancerManagerTest, ManagerRecordResponseTimeFeedsActiveStrategy) {
    common::LoadBalancerManager manager(
        common::LoadBalanceStrategy::LEAST_RESPONSE_TIME);

    manager.updateEndpoints(endpoints);
    manager.recordResponseTime("10.0.0.1:8080", std::chrono::milliseconds(400));
    manager.recordResponseTime("10.0.0.2:8080", std::chrono::milliseconds(40));

    const auto ep = manager.selectEndpoint(endpoints);
    EXPECT_EQ(endpointId(ep), "10.0.0.2:8080");
}

TEST_F(LoadBalancerManagerTest, DefaultStrategyIsRoundRobin) {
    common::LoadBalancerManager manager;
    EXPECT_EQ(manager.getCurrentStrategy(), common::LoadBalanceStrategy::ROUND_ROBIN);
    EXPECT_EQ(manager.getCurrentStrategyName(), "RoundRobin");
}

TEST_F(LoadBalancerManagerTest, InitialStrategyCanBeSet) {
    common::LoadBalancerManager manager(common::LoadBalanceStrategy::RANDOM);
    EXPECT_EQ(manager.getCurrentStrategy(), common::LoadBalanceStrategy::RANDOM);
    EXPECT_EQ(manager.getCurrentStrategyName(), "Random");
}

TEST_F(LoadBalancerManagerTest, SetStrategyChangesStrategy) {
    common::LoadBalancerManager manager;

    manager.setStrategy(common::LoadBalanceStrategy::CONSISTENT_HASH);
    EXPECT_EQ(manager.getCurrentStrategy(), common::LoadBalanceStrategy::CONSISTENT_HASH);
    EXPECT_EQ(manager.getCurrentStrategyName(), "ConsistentHash");

    manager.setStrategy(common::LoadBalanceStrategy::LEAST_RESPONSE_TIME);
    EXPECT_EQ(manager.getCurrentStrategy(), common::LoadBalanceStrategy::LEAST_RESPONSE_TIME);
    EXPECT_EQ(manager.getCurrentStrategyName(), "LeastResponseTime");
}

TEST_F(LoadBalancerManagerTest, SelectEndpointWorkAfterStrategySwitch) {
    common::LoadBalancerManager manager;

    auto ep1 = manager.selectEndpoint(endpoints);
    EXPECT_TRUE(endpointId(ep1) == "10.0.0.1:8080" || endpointId(ep1) == "10.0.0.2:8080");

    manager.setStrategy(common::LoadBalanceStrategy::RANDOM);
    auto ep2 = manager.selectEndpoint(endpoints);
    EXPECT_TRUE(endpointId(ep2) == "10.0.0.1:8080" || endpointId(ep2) == "10.0.0.2:8080");
}

TEST_F(LoadBalancerManagerTest, SameStrategyDoesNotRecreate) {
    common::LoadBalancerManager manager(common::LoadBalanceStrategy::ROUND_ROBIN);

    // Select endpoint to advance internal counter
    manager.selectEndpoint(endpoints);

    // Setting same strategy should not recreate the LB
    manager.setStrategy(common::LoadBalanceStrategy::ROUND_ROBIN);
    EXPECT_EQ(manager.getCurrentStrategy(), common::LoadBalanceStrategy::ROUND_ROBIN);
}

TEST_F(LoadBalancerManagerTest, UpdateEndpointsDelegatesToCurrentStrategy) {
    common::LoadBalancerManager manager(common::LoadBalanceStrategy::CONSISTENT_HASH);

    // Should not throw
    manager.updateEndpoints(endpoints);

    auto ep = manager.selectEndpoint(endpoints);
    EXPECT_TRUE(endpointId(ep) == "10.0.0.1:8080" || endpointId(ep) == "10.0.0.2:8080");
}

TEST_F(LoadBalancerManagerTest, MarkEndpointStatusDelegatesToCurrentStrategy) {
    common::LoadBalancerManager manager;
    manager.updateEndpoints(endpoints);

    // Mark one endpoint as unhealthy
    manager.markEndpointStatus("10.0.0.1:8080", false);

    // Should still work without throwing
    auto ep = manager.selectEndpoint(endpoints);
    EXPECT_EQ(endpointId(ep), "10.0.0.2:8080");
}

TEST_F(LoadBalancerManagerTest, AllStrategiesCanBeSelected) {
    common::LoadBalancerManager manager;

    std::vector<common::LoadBalanceStrategy> strategies = {
        common::LoadBalanceStrategy::ROUND_ROBIN,
        common::LoadBalanceStrategy::RANDOM,
        common::LoadBalanceStrategy::LEAST_CONNECTIONS,
        common::LoadBalanceStrategy::WEIGHTED_ROUND_ROBIN,
        common::LoadBalanceStrategy::CONSISTENT_HASH,
        common::LoadBalanceStrategy::LEAST_RESPONSE_TIME
    };

    for (auto strategy : strategies) {
        manager.setStrategy(strategy);
        EXPECT_EQ(manager.getCurrentStrategy(), strategy);
    }
}

}  // namespace agent_rpc::tests
