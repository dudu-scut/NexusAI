#include <gtest/gtest.h>
#include "agent_rpc/common/cost_tracker.h"

using namespace agent_rpc::common;

TEST(CostTrackerTest, CalculateCostForKnownModel) {
    double cost = CostTracker::calculateCost(
        "deepseek-v4-pro", 1000, 500);  // 1k prompt, 0.5k completion
    // deepseek-v4: ~$0.14/1k prompt, ~$0.28/1k completion
    // 1000 prompt = 0.14, 500 completion = 0.14
    EXPECT_GT(cost, 0.0);
    EXPECT_LT(cost, 0.01);  // should be fractions of a cent
}

TEST(CostTrackerTest, ZeroTokensCostsZero) {
    double cost = CostTracker::calculateCost("deepseek-v4-pro", 0, 0);
    EXPECT_DOUBLE_EQ(cost, 0.0);
}

TEST(CostTrackerTest, UnknownModelUsesDefaultPricing) {
    double cost = CostTracker::calculateCost("nonexistent-model", 1000, 1000);
    // Should not crash, should return some default
    EXPECT_GE(cost, 0.0);
}

TEST(CostTrackerTest, FormatCostForBudgetCounter) {
    // CostTracker stores budgets in micro-dollars (cost * 1,000,000) as int64
    double cost = 0.001234; // $0.001234
    int64_t micro = CostTracker::toMicroDollars(cost);
    EXPECT_EQ(micro, 1234);
}

TEST(CostTrackerTest, EstimatedTokensProduceNonZeroCost) {
    // P4: callers now pass estimated (non-zero) token counts. Even small
    // estimates must produce a strictly positive cost instead of the old
    // zero-token no-op.
    double cost = CostTracker::calculateCost("estimate-model", 320, 240);
    EXPECT_GT(cost, 0.0);
}

TEST(CostTrackerTest, EstimatedTokensRoundTripToMicroDollarIntegers) {
    // Default pricing is $0.0005/1k prompt + $0.0015/1k completion, so
    // 1000 + 1000 estimated tokens = $0.002 = 2000 micro-dollars exactly.
    double cost = CostTracker::calculateCost("nonexistent-model", 1000, 1000);
    EXPECT_GT(cost, 0.0);
    int64_t micro = CostTracker::toMicroDollars(cost);
    EXPECT_GT(micro, 0);
    EXPECT_EQ(micro, 2000);
}
