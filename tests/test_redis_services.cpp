/**
 * @file test_redis_services.cpp
 * @brief Integration tests for RedisClient and MemoryService.
 *
 * Requires a running Redis instance at REDIS_HOST:REDIS_PORT (default 127.0.0.1:6379).
 * Tests use a "nexusai_test:" prefix to avoid polluting real data and clean up after themselves.
 */

#include "agent_rpc/common/redis_client.h"
#include "agent_rpc/common/memory_service.h"
#include "agent_rpc/server/in_flight_registry.h"
#include "agent_rpc/server/query_helpers.h"

#include "ai_query.pb.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

namespace agent_rpc::tests {

namespace {

std::string getRedisHost() {
    const char* h = std::getenv("REDIS_HOST");
    return h ? h : "127.0.0.1";
}

int getRedisPort() {
    const char* p = std::getenv("REDIS_PORT");
    return p ? std::atoi(p) : 6379;
}

// Unique prefix for each test run to avoid collisions.
// Uses '_' instead of ':' for compatibility with Redis key sanitization in MemoryService.
std::string testPrefix() {
    static int counter = 0;
    return "nexusai_test_" + std::to_string(counter++);
}

}  // namespace

// Fixture: provides a connected RedisClient, skips if Redis unavailable
class RedisFixture : public ::testing::Test {
protected:
    common::RedisClient redis;
    bool redis_available = false;

    void SetUp() override {
        redis_available = redis.connect(getRedisHost(), getRedisPort());
        if (!redis_available) {
            GTEST_SKIP() << "Redis not available at " << getRedisHost()
                         << ":" << getRedisPort();
        }
    }

    // Clean up a key after test
    void cleanup(const std::string& key) {
        redis.del(key);
    }
};

// RedisClient basic operations

TEST_F(RedisFixture, StringSetGetDel) {
    auto key = testPrefix() + ":str";
    EXPECT_TRUE(redis.set(key, "hello"));

    std::string val;
    EXPECT_TRUE(redis.get(key, val));
    EXPECT_EQ(val, "hello");

    EXPECT_TRUE(redis.del(key));
    EXPECT_FALSE(redis.get(key, val));
}

TEST_F(RedisFixture, StringSetex) {
    auto key = testPrefix() + ":setex";
    EXPECT_TRUE(redis.setex(key, 10, "temp"));

    std::string val;
    EXPECT_TRUE(redis.get(key, val));
    EXPECT_EQ(val, "temp");

    redis.del(key);
}

TEST_F(RedisFixture, ExistsAndDel) {
    auto key = testPrefix() + ":exists";
    EXPECT_FALSE(redis.exists(key));

    redis.set(key, "x");
    EXPECT_TRUE(redis.exists(key));

    redis.del(key);
    EXPECT_FALSE(redis.exists(key));
}

TEST_F(RedisFixture, HashOperations) {
    auto key = testPrefix() + ":hash";

    EXPECT_TRUE(redis.hset(key, "f1", "v1"));
    EXPECT_TRUE(redis.hset(key, "f2", "v2"));

    std::string val;
    EXPECT_TRUE(redis.hget(key, "f1", val));
    EXPECT_EQ(val, "v1");

    std::map<std::string, std::string> all;
    EXPECT_TRUE(redis.hgetall(key, all));
    EXPECT_EQ(all.size(), 2u);
    EXPECT_EQ(all["f1"], "v1");
    EXPECT_EQ(all["f2"], "v2");

    EXPECT_TRUE(redis.hdel(key, "f1"));
    EXPECT_FALSE(redis.hget(key, "f1", val));

    redis.del(key);
}

TEST_F(RedisFixture, HsetnxAtomic) {
    auto key = testPrefix() + ":hsetnx";

    EXPECT_TRUE(redis.hsetnx(key, "field", "first"));
    EXPECT_FALSE(redis.hsetnx(key, "field", "second"));  // already exists

    std::string val;
    redis.hget(key, "field", val);
    EXPECT_EQ(val, "first");  // original value preserved

    redis.del(key);
}

TEST_F(RedisFixture, ListOperations) {
    auto key = testPrefix() + ":list";

    EXPECT_TRUE(redis.rpush(key, "a"));
    EXPECT_TRUE(redis.rpush(key, "b"));
    EXPECT_TRUE(redis.rpush(key, "c"));

    std::vector<std::string> items;
    EXPECT_TRUE(redis.lrange(key, 0, -1, items));
    EXPECT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0], "a");
    EXPECT_EQ(items[2], "c");

    // Trim to last 2 elements
    EXPECT_TRUE(redis.ltrim(key, -2, -1));
    items.clear();
    redis.lrange(key, 0, -1, items);
    EXPECT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0], "b");

    redis.del(key);
}

TEST_F(RedisFixture, ExpireKey) {
    auto key = testPrefix() + ":expire";
    redis.set(key, "val");
    EXPECT_TRUE(redis.expire(key, 60));
    // Key still exists (hasn't expired yet)
    EXPECT_TRUE(redis.exists(key));
    redis.del(key);
}

// MemoryService (Redis-backed)

class MemoryFixture : public RedisFixture {
protected:
    std::unique_ptr<common::MemoryService> memory;

    void SetUp() override {
        RedisFixture::SetUp();
        if (!redis_available) return;
        memory = std::make_unique<common::MemoryService>(
            std::shared_ptr<common::RedisClient>(&redis, [](common::RedisClient*){}));
    }
};

TEST_F(MemoryFixture, ConversationHistory) {
    auto ctx = testPrefix() + "_ctx";  // sanitization-safe (no colons)
    auto agent = "agent-1";

    memory->appendMessage(ctx, agent, "user", "你好");
    memory->appendMessage(ctx, agent, "agent", "你好！有什么可以帮你的？");
    memory->appendMessage(ctx, agent, "user", "写个排序算法");

    auto history = memory->getConversationHistory(ctx, agent, 10);
    EXPECT_TRUE(history.find("你好") != std::string::npos);
    EXPECT_TRUE(history.find("排序算法") != std::string::npos);

    // Cleanup — keys match sanitized MemoryService internals (colons → underscores)
    redis.del("nexusai:conv:" + ctx + ":" + agent);
    redis.del("nexusai:last_agent:" + ctx);
}

TEST_F(MemoryFixture, LastAgentTracking) {
    auto ctx = testPrefix() + "_ctx";  // sanitization-safe

    memory->setLastAgent(ctx, "agent-a");
    EXPECT_EQ(memory->getLastAgent(ctx), "agent-a");

    memory->setLastAgent(ctx, "agent-b");
    EXPECT_EQ(memory->getLastAgent(ctx), "agent-b");

    redis.del("nexusai:last_agent:" + ctx);
}

TEST_F(MemoryFixture, UserMemory) {
    auto uid = testPrefix() + "_uid";  // sanitization-safe

    memory->setUserMemory(uid, "language", "Chinese");
    memory->setUserMemory(uid, "timezone", "UTC+8");

    auto mem = memory->getUserMemory(uid);
    EXPECT_TRUE(mem.find("language: Chinese") != std::string::npos);
    EXPECT_TRUE(mem.find("timezone: UTC+8") != std::string::npos);

    redis.del("nexusai:memory:" + uid);
}

TEST_F(MemoryFixture, UserMemoryFromHints) {
    // Use sanitization-safe IDs (no colons — they get replaced by '_')
    auto uid = testPrefix() + "_uid";
    auto key = "nexusai:memory:" + uid;

    // Pre-set a field
    redis.hset(key, "existing", "value");

    std::map<std::string, std::string> hints;
    hints["preference"] = "dark_mode";
    hints["existing"] = "updated";

    memory->updateUserMemoryFromHints(uid, hints);

    std::string val;
    redis.hget(key, "preference", val);
    EXPECT_EQ(val, "dark_mode");

    redis.hget(key, "existing", val);
    EXPECT_EQ(val, "updated");

    // Empty value = delete
    hints.clear();
    hints["preference"] = "";  // delete
    memory->updateUserMemoryFromHints(uid, hints);

    EXPECT_FALSE(redis.hget(key, "preference", val));

    redis.del(key);
}

TEST_F(MemoryFixture, CrossAgentSummary) {
    auto ctx = testPrefix() + "_ctx";  // sanitization-safe (no colons)

    EXPECT_EQ(memory->getCrossAgentSummary(ctx), "");

    memory->setCrossAgentSummary(ctx, "User was discussing sorting algorithms.");
    EXPECT_EQ(memory->getCrossAgentSummary(ctx),
              "User was discussing sorting algorithms.");

    redis.del("nexusai:summary:" + ctx);
}

TEST_F(MemoryFixture, BuildSystemContext) {
    auto uid = testPrefix() + "_uid";  // sanitization-safe (no colons)
    auto ctx = testPrefix() + "_ctx";
    auto agent = "agent-1";

    // Set up some data
    memory->appendMessage(ctx, agent, "user", "test message");
    redis.hset("nexusai:memory:" + uid, "lang", "zh");
    memory->setCrossAgentSummary(ctx, "previous context");

    auto sys_ctx = memory->buildSystemContext(uid, ctx, agent, 5);

    EXPECT_TRUE(sys_ctx.conversation_history().find("test message") != std::string::npos);
    EXPECT_TRUE(sys_ctx.user_memory().find("lang: zh") != std::string::npos);
    EXPECT_EQ(sys_ctx.cross_agent_summary(), "previous context");

    // Cleanup
    redis.del("nexusai:conv:" + ctx + ":" + agent);
    redis.del("nexusai:memory:" + uid);
    redis.del("nexusai:summary:" + ctx);
    redis.del("nexusai:last_agent:" + ctx);
}

TEST_F(MemoryFixture, HistoryTrimming) {
    auto ctx = testPrefix() + "_ctx";  // sanitization-safe
    auto agent = "agent-trim";

    // Append more than 50 messages (kMaxHistoryPerAgent)
    for (int i = 0; i < 55; ++i) {
        memory->appendMessage(ctx, agent, "user", "msg-" + std::to_string(i));
    }

    auto history = memory->getConversationHistory(ctx, agent, 100);
    // Should only contain the last 50 messages, not 55
    EXPECT_TRUE(history.find("msg-5") != std::string::npos);
    EXPECT_TRUE(history.find("msg-54") != std::string::npos);
    EXPECT_TRUE(history.find("msg-0") == std::string::npos);  // trimmed

    redis.del("nexusai:conv:" + ctx + ":" + agent);
    redis.del("nexusai:last_agent:" + ctx);
}

// Authentication is PostgreSQL-backed and is covered by LocalAuthContractTest.

// ============================================================================
// Batch-1 memory loop: agent-specialized cross-agent summaries (P3)
// ============================================================================

TEST_F(MemoryFixture, CrossAgentSummaryForAgentScopedKey) {
    auto ctx = testPrefix() + "_ctx";  // sanitization-safe (no colons)

    // Absent key reads back empty.
    EXPECT_EQ(memory->getCrossAgentSummaryFor(ctx, "agent-b"), "");

    memory->setCrossAgentSummaryFor(ctx, "agent-b", "summary for agent-b");
    EXPECT_EQ(memory->getCrossAgentSummaryFor(ctx, "agent-b"), "summary for agent-b");
    // The agent-scoped key must not leak into the legacy context-only key.
    EXPECT_EQ(memory->getCrossAgentSummary(ctx), "");

    // Different agents get independent summaries for the same context.
    memory->setCrossAgentSummaryFor(ctx, "agent-c", "summary for agent-c");
    EXPECT_EQ(memory->getCrossAgentSummaryFor(ctx, "agent-b"), "summary for agent-b");
    EXPECT_EQ(memory->getCrossAgentSummaryFor(ctx, "agent-c"), "summary for agent-c");

    redis.del("nexusai:summary:" + ctx + ":agent-b");
    redis.del("nexusai:summary:" + ctx + ":agent-c");
}

TEST(QueryHelpersPromptTest, BuildSummaryPromptContainsTargetDuties) {
    const std::string prompt = server::QueryHelpers::buildSummaryPrompt(
        "数学助手", "数学计算与公式推导");
    EXPECT_NE(prompt.find("即将接手的助手职责是"), std::string::npos);
    EXPECT_NE(prompt.find("数学助手"), std::string::npos);
    EXPECT_NE(prompt.find("数学计算与公式推导"), std::string::npos);
    EXPECT_NE(prompt.find("只保留与其职责相关的信息"), std::string::npos);

    // Without target info the base prompt stays generic.
    const std::string generic_prompt = server::QueryHelpers::buildSummaryPrompt("", "");
    EXPECT_EQ(generic_prompt.find("即将接手的助手职责是"), std::string::npos);
    EXPECT_NE(generic_prompt.find("对话摘要助手"), std::string::npos);
}

TEST_F(MemoryFixture, AgentSwitchGeneratesTargetSpecializedSummary) {
    auto uid = testPrefix() + "_uid";
    auto ctx = testPrefix() + "_ctx";

    // Seed the previous agent's history (Redis Tier-1 fallback used when no
    // QueryDomainRepository is wired in).
    memory->setLastAgent(ctx, "agent-a");
    memory->appendMessage(ctx, "agent-a", "user", "帮我推导二次方程求根公式");
    memory->appendMessage(ctx, "agent-a", "agent", "好的，使用配方法……");
    // appendMessage records the last agent; restore the switch baseline.
    memory->setLastAgent(ctx, "agent-a");

    int summarize_calls = 0;
    std::string seen_prompt;
    std::string seen_history;

    {
        server::QueryHelpers helpers;
        helpers.summarize_fn = [&](const std::string& prompt,
                                   const std::string& history) {
            ++summarize_calls;
            seen_prompt = prompt;
            seen_history = history;
            return "SUMMARY-OK";
        };
        helpers.handleAgentSwitch(memory.get(), nullptr, nullptr, uid, ctx,
                                  "agent-b", "数学助手", "数学计算与公式推导");
        // Destructor drains the pending summary future before assertions.
    }

    EXPECT_EQ(summarize_calls, 1);
    // Prompt is specialized to the taking-over assistant's duties.
    EXPECT_NE(seen_prompt.find("即将接手的助手职责是"), std::string::npos);
    EXPECT_NE(seen_prompt.find("数学计算与公式推导"), std::string::npos);
    // History came from the previous conversation.
    EXPECT_NE(seen_history.find("二次方程求根公式"), std::string::npos);

    // Agent-scoped key written AND legacy context-only key refreshed so the
    // recall path (getCrossAgentSummary) sees the fresh summary.
    EXPECT_EQ(memory->getCrossAgentSummaryFor(ctx, "agent-b"), "SUMMARY-OK");
    EXPECT_EQ(memory->getCrossAgentSummary(ctx), "SUMMARY-OK");
    // Converged last_agent write records the real agent id.
    EXPECT_EQ(memory->getLastAgent(ctx), "agent-b");

    redis.del("nexusai:conv:" + ctx + ":agent-a");
    redis.del("nexusai:last_agent:" + ctx);
    redis.del("nexusai:summary:" + ctx);
    redis.del("nexusai:summary:" + ctx + ":agent-b");
}

TEST_F(MemoryFixture, AgentSwitchCacheHitSkipsSummarizer) {
    auto uid = testPrefix() + "_uid";
    auto ctx = testPrefix() + "_ctx";

    memory->setLastAgent(ctx, "agent-a");
    memory->appendMessage(ctx, "agent-a", "user", "任意历史内容");
    memory->setLastAgent(ctx, "agent-a");

    // A specialized summary for the taking-over agent already exists — the
    // dedup key must short-circuit the LLM round-trip.
    memory->setCrossAgentSummaryFor(ctx, "agent-b", "EXISTING-SUMMARY");

    int summarize_calls = 0;
    {
        server::QueryHelpers helpers;
        helpers.summarize_fn = [&](const std::string&, const std::string&) {
            ++summarize_calls;
            return std::string("NEVER-WRITTEN");
        };
        helpers.handleAgentSwitch(memory.get(), nullptr, nullptr, uid, ctx,
                                  "agent-b", "数学助手", "数学计算");
    }

    EXPECT_EQ(summarize_calls, 0);
    EXPECT_EQ(memory->getCrossAgentSummaryFor(ctx, "agent-b"), "EXISTING-SUMMARY");
    // Dedup hit still refreshes the legacy context-level key: recall reads
    // that key, so a fresh specialized summary must never coexist with a
    // stale context-level one within the 7-day dedup window.
    EXPECT_EQ(memory->getCrossAgentSummary(ctx), "EXISTING-SUMMARY");
    // last_agent still converges on the real agent id.
    EXPECT_EQ(memory->getLastAgent(ctx), "agent-b");

    redis.del("nexusai:conv:" + ctx + ":agent-a");
    redis.del("nexusai:last_agent:" + ctx);
    redis.del("nexusai:summary:" + ctx + ":agent-b");
}

#ifndef _WIN32
TEST_F(MemoryFixture, AgentSwitchHonorsCrossAgentSummarySwitch) {
    auto uid = testPrefix() + "_uid";
    auto ctx = testPrefix() + "_ctx";

    memory->setLastAgent(ctx, "agent-a");
    memory->appendMessage(ctx, "agent-a", "user", "任意历史内容");
    memory->setLastAgent(ctx, "agent-a");

    ::setenv("NEXUSAI_CROSS_AGENT_SUMMARY", "0", 1);

    int summarize_calls = 0;
    {
        server::QueryHelpers helpers;
        helpers.summarize_fn = [&](const std::string&, const std::string&) {
            ++summarize_calls;
            return std::string("NEVER-WRITTEN");
        };
        helpers.handleAgentSwitch(memory.get(), nullptr, nullptr, uid, ctx,
                                  "agent-b", "翻译助手", "多语言翻译");
    }

    ::unsetenv("NEXUSAI_CROSS_AGENT_SUMMARY");

    EXPECT_EQ(summarize_calls, 0);
    EXPECT_EQ(memory->getCrossAgentSummary(ctx), "");
    // The switch gates only the summary pipeline; last_agent is still written.
    EXPECT_EQ(memory->getLastAgent(ctx), "agent-b");

    redis.del("nexusai:conv:" + ctx + ":agent-a");
    redis.del("nexusai:last_agent:" + ctx);
}
#endif  // _WIN32

// ── P24 C0/C1/C2: InFlightAbortRegistry (header-only, no Redis needed) ──

TEST(InFlightAbortRegistryTest, CancelByRequestPreArmsFutureCalls) {
    auto& registry = agent_rpc::server::InFlightAbortRegistry::instance();
    EXPECT_FALSE(registry.isRequestCancelled("p24-tok-req"));
    registry.cancelInFlightByRequest("p24-tok-req");
    EXPECT_TRUE(registry.isRequestCancelled("p24-tok-req"));

    // A fresh call of the cancelled request registers pre-armed...
    auto flag = registry.registerInFlight("http://p24-tok-agent", "p24-tok-req");
    ASSERT_NE(flag, nullptr);
    EXPECT_TRUE(flag->load());
    registry.unregisterInFlight("http://p24-tok-agent", flag);

    // ...while a concurrent request's call does not.
    auto other = registry.registerInFlight("http://p24-tok-agent", "p24-tok-other");
    EXPECT_FALSE(other->load());
    registry.unregisterInFlight("http://p24-tok-agent", other);
}

TEST(InFlightAbortRegistryTest, CancelByRequestFlipsLiveCallsAndIsolatesOthers) {
    auto& registry = agent_rpc::server::InFlightAbortRegistry::instance();
    auto f1 = registry.registerInFlight("http://p24-iso-a", "p24-iso-r1");
    auto f2 = registry.registerInFlight("http://p24-iso-b", "p24-iso-r1");
    auto f3 = registry.registerInFlight("http://p24-iso-a", "p24-iso-r2");

    registry.cancelInFlightByRequest("p24-iso-r1");
    EXPECT_TRUE(f1->load());
    EXPECT_TRUE(f2->load());
    // R18: another request sharing the same agent URL is untouched.
    EXPECT_FALSE(f3->load());

    registry.unregisterInFlight("http://p24-iso-a", f1);
    registry.unregisterInFlight("http://p24-iso-b", f2);
    registry.unregisterInFlight("http://p24-iso-a", f3);
}

TEST(InFlightAbortRegistryTest, UrlScopedCancelDoesNotMarkRequestToken) {
    auto& registry = agent_rpc::server::InFlightAbortRegistry::instance();
    auto flag = registry.registerInFlight("http://p24-url-agent", "p24-url-req");
    // DAG subtask timeout aborts ONE call — the request-level token must
    // stay untouched so sibling subtasks and the planning LLM keep running.
    registry.cancelInFlight("http://p24-url-agent", "p24-url-req");
    EXPECT_TRUE(flag->load());
    EXPECT_FALSE(registry.isRequestCancelled("p24-url-req"));
    registry.unregisterInFlight("http://p24-url-agent", flag);
}

TEST(InFlightAbortRegistryTest, TaskIdRoundTripAndRaiiCleanup) {
    auto& registry = agent_rpc::server::InFlightAbortRegistry::instance();
    auto flag = registry.registerInFlight("http://p24-task-agent", "p24-task-req");
    EXPECT_EQ(registry.inFlightTaskId("http://p24-task-agent", "p24-task-req"), "");
    registry.setInFlightTaskId("http://p24-task-agent", flag, "task-abc");
    EXPECT_EQ(registry.inFlightTaskId("http://p24-task-agent", "p24-task-req"),
              "task-abc");
    // RAII unregister clears the entry — no task-id leakage.
    registry.unregisterInFlight("http://p24-task-agent", flag);
    EXPECT_EQ(registry.inFlightTaskId("http://p24-task-agent", "p24-task-req"), "");
}

}  // namespace agent_rpc::tests
