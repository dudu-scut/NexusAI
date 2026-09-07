/**
 * @file test_agent_router_properties.cpp
 * @brief Property-based tests for Agent Router
 */

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

// AgentRouter's member layout is guarded by AGENT_RPC_ENABLE_MCP (three
// extra unique_ptr members in MCP builds). P20 遗留 ODR 根治：该宏现经
// orchestrator/CMakeLists.txt 以 PUBLIC 传播，链接本库的测试 TU 自动与
// liborchestrator.a 布局一致——不再需要本地 __has_include 探测。

#include "agent_rpc/orchestrator/agent_router.h"
#include "agent_rpc/orchestrator/agent_info.h"
#include "agent_rpc/orchestrator/task_executor.h"
#include "agent_rpc/orchestrator/task_planner.h"
#include "agent_rpc/common/trace_context.h"
#include "agent_rpc/common/load_balancer.h"
#include "agent_rpc/a2a_adapter/url_validation.h"

#include <atomic>
#include <cstdlib>
#include <thread>
#include <unordered_set>
#include <algorithm>

using namespace agent_rpc::orchestrator;

// Test Fixtures

class AgentRouterPropertyTest : public ::testing::Test {
protected:
    void SetUp() override {
        router_ = std::make_unique<AgentRouter>();
        ASSERT_TRUE(router_->initialize(RoutingStrategy::SKILL_MATCH));
    }
    
    void TearDown() override {
        router_->shutdown();
    }
    
    AgentInfo createAgent(const std::string& id, 
                          const std::vector<std::string>& skills,
                          bool healthy = true) {
        AgentInfo agent;
        agent.id = id;
        agent.name = "Agent " + id;
        agent.url = "http://localhost:500" + id;
        agent.skills = skills;
        agent.is_healthy = healthy;
        agent.current_load = 0;
        return agent;
    }
    
    std::unique_ptr<AgentRouter> router_;
};

// Helper Generators

namespace rc {

// Generator for agent IDs
Gen<std::string> genAgentId() {
    return gen::map(
        gen::inRange(1, 1000),
        [](int n) { return "agent-" + std::to_string(n); }
    );
}

// Generator for skills
Gen<std::string> genSkill() {
    return gen::element<std::string>(
        "math", "coding", "writing", "translation", "general"
    );
}

// Generator for skill list
Gen<std::vector<std::string>> genSkillList() {
    return gen::unique<std::vector<std::string>>(genSkill());
}

// Generator for agent count
Gen<int> genAgentCount() {
    return gen::inRange(1, 20);
}

} // namespace rc

// Property 4: Agent Selection Determinism

/**
 * Property 4.1: Round-robin produces cyclic sequence
 * Note: The exact order depends on internal map iteration, but the pattern should repeat
 */
TEST_F(AgentRouterPropertyTest, RoundRobinProducesCyclicSequence) {
    router_->setStrategy(RoutingStrategy::ROUND_ROBIN);
    
    // Add agents
    std::vector<AgentInfo> agents;
    for (int i = 0; i < 5; ++i) {
        agents.push_back(createAgent(std::to_string(i), {"general"}));
    }
    router_->updateAgentList(agents);
    
    // Select agents multiple times
    std::vector<std::string> selected_ids;
    for (int i = 0; i < 15; ++i) {
        auto selected = router_->selectAgent("test question");
        ASSERT_TRUE(selected.has_value());
        selected_ids.push_back(selected->id);
    }
    
    // Verify round-robin pattern: sequence should repeat every 5 selections
    // First cycle establishes the order
    std::vector<std::string> first_cycle(selected_ids.begin(), selected_ids.begin() + 5);
    std::vector<std::string> second_cycle(selected_ids.begin() + 5, selected_ids.begin() + 10);
    std::vector<std::string> third_cycle(selected_ids.begin() + 10, selected_ids.begin() + 15);
    
    EXPECT_EQ(first_cycle, second_cycle);
    EXPECT_EQ(second_cycle, third_cycle);
    
    // Verify all agents are selected in each cycle
    std::unordered_set<std::string> unique_in_cycle(first_cycle.begin(), first_cycle.end());
    EXPECT_EQ(unique_in_cycle.size(), 5u);
}

/**
 * Property 4.2: Skill-match returns agents with matching skills
 */
RC_GTEST_FIXTURE_PROP(AgentRouterPropertyTest, SkillMatchReturnsMatchingAgents, ()) {
    router_->setStrategy(RoutingStrategy::SKILL_MATCH);
    
    // Create agents with different skills
    auto math_agent = createAgent("math-1", {"math", "general"});
    auto code_agent = createAgent("code-1", {"coding", "general"});
    auto write_agent = createAgent("write-1", {"writing", "general"});
    
    router_->addAgent(math_agent);
    router_->addAgent(code_agent);
    router_->addAgent(write_agent);
    
    // Select with specific skill requirement
    auto selected = router_->selectAgent("", {"math"});
    
    RC_ASSERT(selected.has_value());
    RC_ASSERT(selected->hasSkill("math"));
}

/**
 * Property 4.3: Selection only returns healthy agents
 */
RC_GTEST_FIXTURE_PROP(AgentRouterPropertyTest, SelectionOnlyReturnsHealthyAgents, ()) {
    int agent_count = *rc::gen::inRange(2, 10);
    
    std::vector<AgentInfo> agents;
    for (int i = 0; i < agent_count; ++i) {
        auto agent = createAgent(std::to_string(i), {"general"});
        agent.is_healthy = (i % 2 == 0);  // Half healthy, half unhealthy
        agents.push_back(agent);
    }
    router_->updateAgentList(agents);
    
    // Select multiple times
    for (int i = 0; i < 20; ++i) {
        auto selected = router_->selectAgent("test");
        if (selected.has_value()) {
            RC_ASSERT(selected->is_healthy);
        }
    }
}

/**
 * Property 4.4: Least-load selects agent with minimum load
 */
TEST_F(AgentRouterPropertyTest, LeastLoadSelectsMinimumLoad) {
    router_->setStrategy(RoutingStrategy::LEAST_LOAD);
    
    // Create agents with different loads
    auto agent1 = createAgent("1", {"general"});
    agent1.current_load = 10;
    auto agent2 = createAgent("2", {"general"});
    agent2.current_load = 5;
    auto agent3 = createAgent("3", {"general"});
    agent3.current_load = 15;
    
    router_->addAgent(agent1);
    router_->addAgent(agent2);
    router_->addAgent(agent3);
    
    auto selected = router_->selectAgent("test");
    
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->id, "2");  // Agent with load 5
}

/**
 * Property 4.5: Empty agent list returns nullopt
 */
TEST_F(AgentRouterPropertyTest, EmptyAgentListReturnsNullopt) {
    auto selected = router_->selectAgent("test question");
    EXPECT_FALSE(selected.has_value());
}

/**
 * Property 4.6: All unhealthy agents returns nullopt
 */
TEST_F(AgentRouterPropertyTest, AllUnhealthyReturnsNullopt) {
    auto agent1 = createAgent("1", {"general"}, false);
    auto agent2 = createAgent("2", {"general"}, false);
    
    router_->addAgent(agent1);
    router_->addAgent(agent2);
    
    auto selected = router_->selectAgent("test");
    EXPECT_FALSE(selected.has_value());
}

// Property 8: Agent Health State Consistency

/**
 * Property 8.1: Unhealthy agents are excluded from selection
 */
RC_GTEST_FIXTURE_PROP(AgentRouterPropertyTest, UnhealthyAgentsExcludedFromSelection, ()) {
    int agent_count = *rc::gen::inRange(2, 10);
    
    std::vector<AgentInfo> agents;
    for (int i = 0; i < agent_count; ++i) {
        agents.push_back(createAgent(std::to_string(i), {"general"}));
    }
    router_->updateAgentList(agents);
    
    // Mark some agents unhealthy
    std::unordered_set<std::string> unhealthy_ids;
    for (int i = 0; i < agent_count; i += 2) {
        router_->markAgentUnhealthy(std::to_string(i));
        unhealthy_ids.insert(std::to_string(i));
    }
    
    // Select multiple times
    for (int i = 0; i < 50; ++i) {
        auto selected = router_->selectAgent("test");
        if (selected.has_value()) {
            RC_ASSERT(unhealthy_ids.find(selected->id) == unhealthy_ids.end());
        }
    }
}

/**
 * Property 8.2: markAgentUnhealthy changes health state
 */
RC_GTEST_FIXTURE_PROP(AgentRouterPropertyTest, MarkUnhealthyChangesState, ()) {
    auto agent_id = *rc::genAgentId();
    auto agent = createAgent(agent_id, {"general"}, true);
    
    router_->addAgent(agent);
    
    RC_ASSERT(router_->isAgentHealthy(agent_id));
    
    router_->markAgentUnhealthy(agent_id);
    
    RC_ASSERT(!router_->isAgentHealthy(agent_id));
}

/**
 * Property 8.3: markAgentHealthy restores health state
 */
RC_GTEST_FIXTURE_PROP(AgentRouterPropertyTest, MarkHealthyRestoresState, ()) {
    auto agent_id = *rc::genAgentId();
    auto agent = createAgent(agent_id, {"general"}, false);
    
    router_->addAgent(agent);
    
    RC_ASSERT(!router_->isAgentHealthy(agent_id));
    
    router_->markAgentHealthy(agent_id);
    
    RC_ASSERT(router_->isAgentHealthy(agent_id));
}

/**
 * Property 8.4: Health state is consistent across queries
 */
RC_GTEST_FIXTURE_PROP(AgentRouterPropertyTest, HealthStateConsistentAcrossQueries, ()) {
    auto agent_id = *rc::genAgentId();
    auto agent = createAgent(agent_id, {"general"});
    
    router_->addAgent(agent);
    
    // Toggle health state
    bool expected_healthy = *rc::gen::arbitrary<bool>();
    if (expected_healthy) {
        router_->markAgentHealthy(agent_id);
    } else {
        router_->markAgentUnhealthy(agent_id);
    }
    
    // Query multiple times
    for (int i = 0; i < 10; ++i) {
        RC_ASSERT(router_->isAgentHealthy(agent_id) == expected_healthy);
    }
}

/**
 * Property 8.5: getHealthyAgents only returns healthy agents
 */
RC_GTEST_FIXTURE_PROP(AgentRouterPropertyTest, GetHealthyAgentsOnlyReturnsHealthy, ()) {
    int agent_count = *rc::gen::inRange(1, 10);
    
    for (int i = 0; i < agent_count; ++i) {
        auto agent = createAgent(std::to_string(i), {"general"});
        agent.is_healthy = *rc::gen::arbitrary<bool>();
        router_->addAgent(agent);
    }
    
    auto healthy_agents = router_->getHealthyAgents();
    
    for (const auto& agent : healthy_agents) {
        RC_ASSERT(agent.is_healthy);
    }
}

/**
 * Property 8.6: Healthy agent count matches actual healthy agents
 */
RC_GTEST_FIXTURE_PROP(AgentRouterPropertyTest, HealthyCountMatchesActual, ()) {
    int agent_count = *rc::gen::inRange(1, 10);
    int expected_healthy = 0;
    
    for (int i = 0; i < agent_count; ++i) {
        auto agent = createAgent(std::to_string(i), {"general"});
        agent.is_healthy = *rc::gen::arbitrary<bool>();
        if (agent.is_healthy) expected_healthy++;
        router_->addAgent(agent);
    }
    
    RC_ASSERT(router_->getHealthyAgentCount() == static_cast<size_t>(expected_healthy));
}

// Additional Unit Tests

TEST_F(AgentRouterPropertyTest, InitializeAndShutdown) {
    AgentRouter router;
    
    EXPECT_TRUE(router.initialize(RoutingStrategy::ROUND_ROBIN));
    EXPECT_EQ(router.getStrategy(), RoutingStrategy::ROUND_ROBIN);
    
    router.shutdown();
    
    // Can reinitialize
    EXPECT_TRUE(router.initialize(RoutingStrategy::SKILL_MATCH));
}

TEST_F(AgentRouterPropertyTest, AddAndRemoveAgent) {
    auto agent = createAgent("test-1", {"math"});
    
    router_->addAgent(agent);
    EXPECT_EQ(router_->getAgentCount(), 1u);
    
    auto retrieved = router_->getAgent("test-1");
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->id, "test-1");
    
    EXPECT_TRUE(router_->removeAgent("test-1"));
    EXPECT_EQ(router_->getAgentCount(), 0u);
    
    EXPECT_FALSE(router_->removeAgent("non-existent"));
}

TEST_F(AgentRouterPropertyTest, FindAgentsBySkill) {
    router_->addAgent(createAgent("1", {"math", "general"}));
    router_->addAgent(createAgent("2", {"coding", "general"}));
    router_->addAgent(createAgent("3", {"math", "coding"}));
    
    auto math_agents = router_->findAgentsBySkill("math");
    EXPECT_EQ(math_agents.size(), 2u);
    
    auto coding_agents = router_->findAgentsBySkill("coding");
    EXPECT_EQ(coding_agents.size(), 2u);
    
    auto general_agents = router_->findAgentsBySkill("general");
    EXPECT_EQ(general_agents.size(), 2u);
}

TEST_F(AgentRouterPropertyTest, FindAgentsByTags) {
    auto agent1 = createAgent("1", {"math"});
    agent1.tags = {"production", "fast"};
    auto agent2 = createAgent("2", {"coding"});
    agent2.tags = {"production", "slow"};
    auto agent3 = createAgent("3", {"writing"});
    agent3.tags = {"staging"};
    
    router_->addAgent(agent1);
    router_->addAgent(agent2);
    router_->addAgent(agent3);
    
    auto prod_agents = router_->findAgentsByTags({"production"});
    EXPECT_EQ(prod_agents.size(), 2u);
    
    auto fast_prod_agents = router_->findAgentsByTags({"production", "fast"});
    EXPECT_EQ(fast_prod_agents.size(), 1u);
}

TEST_F(AgentRouterPropertyTest, UpdateAgentLoad) {
    auto agent = createAgent("1", {"general"});
    router_->addAgent(agent);
    
    router_->updateAgentLoad("1", 50);
    
    auto retrieved = router_->getAgent("1");
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved->current_load, 50);
}

TEST_F(AgentRouterPropertyTest, SkillAnalysisFromQuestion) {
    // Create agents with skill descriptions so dynamic keyword extraction works.
    // Descriptions use distinct vocabulary to avoid cross-skill keyword overlap.
    AgentInfo math_agent = createAgent("math-1", {"math"});
    math_agent.skill_descriptions["math"] = "Calculate and compute mathematical equations and formulas";
    router_->addAgent(math_agent);
    
    AgentInfo code_agent = createAgent("code-1", {"coding"});
    code_agent.skill_descriptions["coding"] = "Develop, implement and debug software programs";
    router_->addAgent(code_agent);
    
    AgentInfo write_agent = createAgent("write-1", {"writing"});
    write_agent.skill_descriptions["writing"] = "Compose essays, articles and creative prose";
    router_->addAgent(write_agent);
    
    // Math question — "calculate" is unique to math description
    auto math_result = router_->selectAgent("Please calculate 2+2");
    ASSERT_TRUE(math_result.has_value());
    EXPECT_TRUE(math_result->hasSkill("math"));
    
    // Code question — "debug" is unique to coding description
    auto code_result = router_->selectAgent("Help me debug this program");
    ASSERT_TRUE(code_result.has_value());
    EXPECT_TRUE(code_result->hasSkill("coding"));
    
    // Writing question — "essay" is unique to writing description
    auto write_result = router_->selectAgent("Compose an essay about nature");
    ASSERT_TRUE(write_result.has_value());
    EXPECT_TRUE(write_result->hasSkill("writing"));
}

TEST_F(AgentRouterPropertyTest, InvertedIndexSharedKeywords) {
    // Two skills share the keyword "write" in their descriptions.
    // With IDF weighting, shared "write" gets weight 0.5 while unique
    // keywords get weight 1.0, so unique matches dominate the score.
    AgentInfo article_agent = createAgent("article-1", {"article-writing"});
    article_agent.skill_descriptions["article-writing"] =
        "Write creative articles and storytelling prose";
    router_->addAgent(article_agent);

    AgentInfo code_agent = createAgent("code-gen-1", {"code-generation"});
    code_agent.skill_descriptions["code-generation"] =
        "Write programs and implement software";
    router_->addAgent(code_agent);

    // "write creative story":
    //   "write" → article-writing += 0.5, code-generation += 0.5
    //   "creative" → article-writing += 1.0
    //   "storytelling" → article-writing += 1.0
    //   article-writing = 2.5, code-generation = 0.5
    auto article_result = router_->selectAgent("Write creative story");
    ASSERT_TRUE(article_result.has_value());
    EXPECT_TRUE(article_result->hasSkill("article-writing"));

    // "write programs in Python":
    //   "write" → article-writing += 0.5, code-generation += 0.5
    //   "programs" → code-generation += 1.0
    //   "implement" → code-generation += 1.0
    //   "software" → code-generation += 1.0
    //   code-generation = 3.5, article-writing = 0.5
    auto code_result = router_->selectAgent("Write programs in Python");
    ASSERT_TRUE(code_result.has_value());
    EXPECT_TRUE(code_result->hasSkill("code-generation"));
}

TEST_F(AgentRouterPropertyTest, AgentInfoHasSkillMethods) {
    AgentInfo agent;
    agent.skills = {"math", "coding"};
    agent.tags = {"production"};
    
    EXPECT_TRUE(agent.hasSkill("math"));
    EXPECT_TRUE(agent.hasSkill("coding"));
    EXPECT_FALSE(agent.hasSkill("writing"));
    
    EXPECT_TRUE(agent.hasTag("production"));
    EXPECT_FALSE(agent.hasTag("staging"));
    
    EXPECT_TRUE(agent.hasAllSkills({"math", "coding"}));
    EXPECT_FALSE(agent.hasAllSkills({"math", "writing"}));
    
    EXPECT_TRUE(agent.hasAnySkill({"math", "writing"}));
    EXPECT_FALSE(agent.hasAnySkill({"writing", "translation"}));
}

// P5: child-task spans must be merged back into the parent TraceContext

namespace {

// Builds a two-task same-layer plan (forces the std::async parallel path)
// and a router with one healthy worker agent.
ExecutionPlan buildParallelPlan() {
    ExecutionPlan plan;
    plan.is_single_agent = false;

    SubTask t1;
    t1.id = "t1";
    t1.description = "task one";
    t1.preferred_agent_id = "worker";

    SubTask t2;
    t2.id = "t2";
    t2.description = "task two";
    t2.preferred_agent_id = "worker";

    plan.tasks = {t1, t2};
    return plan;
}

void registerWorkerAgent(AgentRouter& router) {
    AgentInfo agent;
    agent.id = "worker";
    agent.name = "Worker";
    agent.url = "http://localhost:9999";
    agent.skills = {"general"};
    agent.is_healthy = true;
    router.addAgent(agent);
}

} // namespace

TEST(TaskExecutorTracePropagationTest, ChildSpansMergeIntoParentContext) {
#ifndef _WIN32
    // This test asserts the default-on behavior; pin the switch explicitly
    // so a regression run with NEXUSAI_TRACE_PARENT_PROPAGATION=0 in the
    // environment cannot flip the expectation.
    const char* previous = ::getenv("NEXUSAI_TRACE_PARENT_PROPAGATION");
    const std::string previous_value = previous ? previous : "";
    const bool had_previous = previous != nullptr;
    ::setenv("NEXUSAI_TRACE_PARENT_PROPAGATION", "1", 1);
#endif

    AgentRouter router;
    ASSERT_TRUE(router.initialize(RoutingStrategy::SKILL_MATCH));
    registerWorkerAgent(router);

    agent_rpc::common::TraceContext::init("owner", "ctx");
    auto* parent = agent_rpc::common::TraceContext::current();
    const std::string parent_trace = parent->traceId();

    TaskExecutor executor(router, ExecutorConfig{});
    auto results = executor.execute(
        buildParallelPlan(),
        [](const std::string&, const std::string&) { return std::string("ok"); });

    ASSERT_EQ(results.size(), 2u);
    EXPECT_TRUE(results["t1"].success);
    EXPECT_TRUE(results["t2"].success);

    // Worker-thread spans are merged into the parent context after the layer
    // futures are collected.
    bool found_t1 = false;
    bool found_t2 = false;
    for (const auto& span : parent->completedSpans()) {
        if (span.name == "subtask_t1") found_t1 = true;
        if (span.name == "subtask_t2") found_t2 = true;
    }
    EXPECT_TRUE(found_t1);
    EXPECT_TRUE(found_t2);
    EXPECT_EQ(parent->traceId(), parent_trace);

#ifndef _WIN32
    if (had_previous) {
        ::setenv("NEXUSAI_TRACE_PARENT_PROPAGATION", previous_value.c_str(), 1);
    } else {
        ::unsetenv("NEXUSAI_TRACE_PARENT_PROPAGATION");
    }
#endif
}

TEST(TaskExecutorTracePropagationTest, SwitchOffSkipsSpanMerge) {
#ifndef _WIN32
    ::setenv("NEXUSAI_TRACE_PARENT_PROPAGATION", "0", 1);

    AgentRouter router;
    ASSERT_TRUE(router.initialize(RoutingStrategy::SKILL_MATCH));
    registerWorkerAgent(router);

    agent_rpc::common::TraceContext::init("owner", "ctx");
    auto* parent = agent_rpc::common::TraceContext::current();
    const size_t spans_before = parent->completedSpans().size();

    TaskExecutor executor(router, ExecutorConfig{});
    auto results = executor.execute(
        buildParallelPlan(),
        [](const std::string&, const std::string&) { return std::string("ok"); });

    ::unsetenv("NEXUSAI_TRACE_PARENT_PROPAGATION");

    ASSERT_EQ(results.size(), 2u);
    EXPECT_TRUE(results["t1"].success);
    // Legacy behavior when the switch is off: nothing is merged back.
    EXPECT_EQ(parent->completedSpans().size(), spans_before);
#endif
}

// P7: embedding tier must stay inert unless explicitly enabled

TEST(AgentRouterEmbeddingGateTest, DisabledEmbeddingKeepsBaselineBehavior) {
    AgentRouter router;
    ASSERT_TRUE(router.initialize(RoutingStrategy::SKILL_MATCH));

    AgentInfo agent;
    agent.id = "math-agent";
    agent.name = "Math";
    agent.url = "http://localhost:6001";
    agent.skills = {"math"};
    agent.is_healthy = true;
    router.addAgent(agent);

    // Baseline state before touching the embedding tier.
    ASSERT_TRUE(router.getAgent("math-agent").has_value());
    ASSERT_EQ(router.getHealthyAgentCount(), 1u);

    // Explicitly disabled config must succeed and change nothing.
    EmbeddingRouterConfig off_config;
    off_config.enabled = false;
    EXPECT_TRUE(router.enableEmbedding(off_config));
    EXPECT_TRUE(router.getAgent("math-agent").has_value());
    EXPECT_EQ(router.getHealthyAgentCount(), 1u);

    // P7 (批次十一): the tier is real in EVERY build now (vector blocks live
    // in agent_rpc_common); an enabled request may succeed or degrade on
    // embedding availability, but skill lookup must keep working either way.
    EmbeddingRouterConfig on_config;
    on_config.enabled = true;
    on_config.api_key = "test-key";
    (void)router.enableEmbedding(on_config);
    EXPECT_TRUE(router.getAgent("math-agent").has_value());
    EXPECT_EQ(router.getHealthyAgentCount(), 1u);
}

// P8: load-balancer tier gate tests

namespace {

#ifndef _WIN32
// RAII guard: set/restore an environment variable around a test body so
// parallel/sequential cases never leak switch state into each other.
class EnvGuard {
public:
    explicit EnvGuard(const char* name, const char* value) : name_(name) {
        const char* old = ::getenv(name);
        had_old_ = (old != nullptr);
        if (had_old_) old_value_ = old;
        if (value) {
            ::setenv(name, value, 1);
        } else {
            ::unsetenv(name);
        }
    }
    ~EnvGuard() {
        if (had_old_) {
            ::setenv(name_, old_value_.c_str(), 1);
        } else {
            ::unsetenv(name_);
        }
    }

private:
    const char* name_;
    bool had_old_ = false;
    std::string old_value_;
};
#endif

AgentInfo makeLbAgent(const std::string& id) {
    AgentInfo agent;
    agent.id = id;
    agent.name = "Agent " + id;
    agent.url = "http://localhost:700" + id;
    agent.skills = {"general"};
    agent.is_healthy = true;
    agent.current_load = 0;
    return agent;
}

} // namespace

// Switch unset: the router must keep the legacy quality-weighted path
// (all-equal coefficients degenerate to round-robin), byte-for-byte.
TEST(AgentRouterLoadBalancerGateTest, SwitchOffKeepsLegacySemantics) {
#ifndef _WIN32
    EnvGuard guard("NEXUSAI_ROUTER_LB_STRATEGY", nullptr);

    AgentRouter router;
    ASSERT_TRUE(router.initialize(RoutingStrategy::SKILL_MATCH));
    for (int i = 0; i < 5; ++i) {
        router.addAgent(makeLbAgent(std::to_string(i)));
    }

    std::vector<std::string> selected;
    for (int i = 0; i < 10; ++i) {
        auto picked = router.selectAgent("lb question", {"general"});
        ASSERT_TRUE(picked.has_value());
        selected.push_back(picked->id);
    }

    // Legacy degenerate path: fair round-robin over the 5 healthy agents,
    // repeating with period 5.
    std::vector<std::string> first(selected.begin(), selected.begin() + 5);
    std::vector<std::string> second(selected.begin() + 5, selected.end());
    EXPECT_EQ(first, second);
    EXPECT_EQ((std::unordered_set<std::string>(first.begin(), first.end())).size(), 5u);

    // An invalid switch value must be rejected and keep the same behavior.
    EnvGuard invalid_guard("NEXUSAI_ROUTER_LB_STRATEGY", "not_a_strategy");
    AgentRouter fallback_router;
    ASSERT_TRUE(fallback_router.initialize(RoutingStrategy::SKILL_MATCH));
    for (int i = 0; i < 5; ++i) {
        fallback_router.addAgent(makeLbAgent(std::to_string(i)));
    }
    auto picked = fallback_router.selectAgent("lb question", {"general"});
    ASSERT_TRUE(picked.has_value());
#endif
}

// Switch on (round_robin) with distinct quality coefficients: consecutive
// selections must ADVANCE the balancer cursor instead of always picking the
// first candidate. This regresses the updateEndpoints() reset defect — when
// every selection rebuilt the endpoint set, the cursor was zeroed and the
// strategy silently degenerated to "always the first healthy candidate".
TEST(AgentRouterLoadBalancerGateTest, RoundRobinAdvancesCursorWithoutReset) {
#ifndef _WIN32
    EnvGuard guard("NEXUSAI_ROUTER_LB_STRATEGY", "round_robin");

    AgentRouter router;
    ASSERT_TRUE(router.initialize(RoutingStrategy::SKILL_MATCH));
    const std::vector<std::string> ids = {"a1", "a2", "a3"};
    for (const auto& id : ids) {
        router.addAgent(makeLbAgent(id));
    }
    // Distinct quality coefficients force the tier into the load balancer
    // (all-equal coefficients would degenerate to the legacy round-robin
    // path and never exercise the balancer state).
    router.setQualityProvider([](const std::string& agent_id, const std::string&) {
        if (agent_id == "a1") return 0.9;
        if (agent_id == "a2") return 0.7;
        return 0.5;
    });

    std::unordered_set<std::string> seen;
    for (int i = 0; i < 6; ++i) {
        auto picked = router.selectAgent("lb question", {"general"});
        ASSERT_TRUE(picked.has_value());
        seen.insert(picked->id);
    }
    // A resetting balancer would have picked a single candidate six times;
    // the advancing cursor must cover at least two of the three agents.
    EXPECT_GE(seen.size(), 2u);
#endif
}

// Switch on (consistent_hash): the router tier stays inside the healthy
// candidate set, and the underlying consistent-hash balancer maps the same
// key onto the same endpoint deterministically.
TEST(AgentRouterLoadBalancerGateTest, ConsistentHashSameKeySamePick) {
#ifndef _WIN32
    EnvGuard guard("NEXUSAI_ROUTER_LB_STRATEGY", "consistent_hash");

    AgentRouter router;
    ASSERT_TRUE(router.initialize(RoutingStrategy::SKILL_MATCH));
    const std::vector<std::string> ids = {"a1", "a2", "a3"};
    for (const auto& id : ids) {
        router.addAgent(makeLbAgent(id));
    }
    const std::unordered_set<std::string> candidates(ids.begin(), ids.end());

    // Router-level selection through the tier must always land on one of
    // the healthy candidates (the consistent-hash base uses a random key,
    // so the exact pick is not asserted here).
    for (int i = 0; i < 10; ++i) {
        auto picked = router.selectAgent("lb question", {"general"});
        ASSERT_TRUE(picked.has_value());
        EXPECT_TRUE(candidates.count(picked->id) == 1u);
    }

    // Deterministic guarantee lives one layer down: same key, same endpoint.
    std::vector<agent_rpc::common::ServiceEndpoint> endpoints;
    for (const auto& id : ids) {
        agent_rpc::common::ServiceEndpoint ep;
        ep.host = id;
        ep.port = 0;
        ep.service_name = id;
        ep.is_healthy = true;
        ep.metadata["agent_id"] = id;
        endpoints.push_back(ep);
    }
    agent_rpc::common::ConsistentHashLoadBalancer balancer;
    balancer.updateEndpoints(endpoints);
    const auto first_pick = balancer.selectEndpointByKey("stable-key", endpoints);
    for (int i = 0; i < 5; ++i) {
        const auto again = balancer.selectEndpointByKey("stable-key", endpoints);
        EXPECT_EQ(again.host, first_pick.host);
    }
#endif
}

// ── P13: TaskPlanner missing-critical-field handling (batch 7) ──────────

namespace {

// Scripted LLM fake: returns canned responses in call order; records every
// prompt it received so tests can assert the retry correction hint.
class ScriptedLLMClient : public LLMClient {
public:
    explicit ScriptedLLMClient(std::vector<std::string> responses)
        : LLMClient("test-key", "scripted", "http://127.0.0.1:1/unused")
        , responses_(std::move(responses)) {}

    std::string chat(const std::string& /*system_prompt*/,
                     const std::string& user_message) override {
        return chatWithRecording(user_message, LLMClient::kDefaultChatTimeoutSeconds);
    }

    // A3/P20-8: the planner now calls the deadline-aware overload — record
    // the timeout alongside the prompt so tests can assert the contraction.
    std::string chat(const std::string& system_prompt,
                     const std::string& user_message,
                     int timeout_seconds) override {
        return chatWithRecording(user_message, timeout_seconds);
    }

    int callCount() const { return static_cast<int>(calls_.size()); }
    const std::string& promptAt(int i) const { return calls_.at(i); }
    int timeoutAt(int i) const { return timeouts_.at(i); }

private:
    std::string chatWithRecording(const std::string& user_message, int timeout_seconds) {
        calls_.push_back(user_message);
        timeouts_.push_back(timeout_seconds);
        if (responses_.empty()) {
            return "";  // no scripted reply — keep the fake safe for
                         // parse-only tests that never reach chat()
        }
        const auto idx = calls_.size() - 1;
        return responses_[idx < responses_.size() ? idx : responses_.size() - 1];
    }

    std::vector<std::string> responses_;
    std::vector<std::string> calls_;
    std::vector<int> timeouts_;
};

} // anonymous namespace

TEST(TaskPlannerDropTest, MissingDescriptionIsDropped) {
    TaskPlannerConfig cfg;
    TaskPlanner planner(cfg, std::make_unique<ScriptedLLMClient>(std::vector<std::string>{}));
    const std::string response = R"({"single": false, "tasks": [
        {"id": "t1", "description": "翻译文本", "skill": "translation"},
        {"id": "t2", "skill": "math"},
        {"id": "", "description": "无 id", "skill": "math"}
    ]})";
    auto plan = planner.parsePlanResponse(response, "测试查询");
    ASSERT_FALSE(plan.is_single_agent);
    ASSERT_EQ(plan.tasks.size(), 1u);
    EXPECT_EQ(plan.tasks[0].id, "t1");
}

TEST(TaskPlannerDropTest, MissingSkillIsRetained) {
    TaskPlannerConfig cfg;
    TaskPlanner planner(cfg, std::make_unique<ScriptedLLMClient>(std::vector<std::string>{}));
    const std::string response = R"({"single": false, "tasks": [
        {"id": "t1", "description": "无技能任务"},
        {"id": "t2", "description": "正常任务", "skill": "math"}
    ]})";
    auto plan = planner.parsePlanResponse(response, "测试查询");
    ASSERT_FALSE(plan.is_single_agent);
    // Missing skill is kept — routing falls back to the four-tier pipeline.
    ASSERT_EQ(plan.tasks.size(), 2u);
    EXPECT_EQ(plan.tasks[0].required_skill, "");
}

TEST(TaskPlannerDropTest, AllDroppedFallsBackToSingle) {
    TaskPlannerConfig cfg;
    TaskPlanner planner(cfg, std::make_unique<ScriptedLLMClient>(std::vector<std::string>{}));
    const std::string response = R"({"single": false, "tasks": [
        {"id": "t1", "skill": "math"},
        {"id": "t2", "skill": "math"}
    ]})";
    auto plan = planner.parsePlanResponse(response, "测试查询");
    EXPECT_TRUE(plan.is_single_agent);
    EXPECT_TRUE(plan.tasks.empty());
}

TEST(TaskPlannerDropTest, RetriesExactlyOnceWhenTasksDropped) {
    // First response drops one task (missing description); the retry prompt
    // carries the correction hint and the clean second result is accepted.
    const std::string first = R"({"single": false, "tasks": [
        {"id": "t1", "description": "翻译文本", "skill": "translation"},
        {"id": "t2", "skill": "math"}
    ]})";
    const std::string second = R"({"single": false, "tasks": [
        {"id": "t1", "description": "翻译文本", "skill": "translation"},
        {"id": "t2", "description": "计算", "skill": "math"}
    ]})";
    TaskPlannerConfig cfg;
    auto fake = std::make_unique<ScriptedLLMClient>(
        std::vector<std::string>{first, second});
    auto* fake_ptr = fake.get();
    TaskPlanner planner(cfg, std::move(fake));

    std::unordered_map<std::string, std::string> skills = {
        {"translation", "翻译"}, {"math", "数学"}};
    auto plan = planner.plan("翻译并计算", skills);

    EXPECT_EQ(fake_ptr->callCount(), 2);  // exactly one retry
    ASSERT_FALSE(plan.is_single_agent);
    ASSERT_EQ(plan.tasks.size(), 2u);     // retry result accepted
    EXPECT_NE(fake_ptr->promptAt(1).find("缺少 description 字段"),
              std::string::npos);
}

TEST(TaskPlannerDropTest, DuplicateTaskIdIsDroppedNotCycled) {
    // P13/R13: a duplicate id must be dropped (counted) instead of reaching
    // the executor, where the shared task_map previously faked a dependency
    // cycle and failed the whole plan.
    TaskPlannerConfig cfg;
    TaskPlanner planner(cfg, std::make_unique<ScriptedLLMClient>(std::vector<std::string>{}));
    const std::string response = R"({"single": false, "tasks": [
        {"id": "t1", "description": "翻译文本", "skill": "translation"},
        {"id": "t1", "description": "重复 id", "skill": "math"}
    ]})";
    auto plan = planner.parsePlanResponse(response, "测试查询");
    ASSERT_FALSE(plan.is_single_agent);
    ASSERT_EQ(plan.tasks.size(), 1u);
    EXPECT_EQ(plan.tasks[0].id, "t1");
}

TEST(TaskPlannerDropTest, PlanCarriesDroppedTaskCount) {
    // P13(d)/A2: the worst-observed drop count lands on the plan so the
    // aggregator can disclose the uncovered requirement to the user.
    const std::string bad = R"({"single": false, "tasks": [
        {"id": "t1", "skill": "math"},
        {"id": "t2", "skill": "math"}
    ]})";
    TaskPlannerConfig cfg;
    auto fake = std::make_unique<ScriptedLLMClient>(std::vector<std::string>{bad, bad});
    TaskPlanner planner(cfg, std::move(fake));

    std::unordered_map<std::string, std::string> skills = {{"math", "数学"}};
    auto plan = planner.plan("计算", skills);
    EXPECT_EQ(plan.dropped_tasks, 2);
}

TEST(TaskPlannerDropTest, PlanningTimeoutPassedThroughToLLM) {
    // A3/P20-8: the per-call timeout is threaded into the LLM call so the
    // planning phase honors the remaining request budget.
    const std::string ok = R"({"single": false, "tasks": [
        {"id": "t1", "description": "计算", "skill": "math"}
    ]})";
    TaskPlannerConfig cfg;
    auto fake = std::make_unique<ScriptedLLMClient>(std::vector<std::string>{ok});
    auto* fake_ptr = fake.get();
    TaskPlanner planner(cfg, std::move(fake));

    std::unordered_map<std::string, std::string> skills = {{"math", "数学"}};
    auto plan = planner.plan("计算", skills, 7);
    EXPECT_EQ(fake_ptr->callCount(), 1);
    EXPECT_EQ(fake_ptr->timeoutAt(0), 7);
    ASSERT_FALSE(plan.is_single_agent);
}

TEST(TaskPlannerDropTest, RetryOnceThenAcceptFailSoft) {
    // Both attempts drop tasks: hard cap stops after one retry, single-agent
    // fallback is preserved (fail-soft baseline unchanged).
    const std::string bad = R"({"single": false, "tasks": [
        {"id": "t1", "skill": "math"}
    ]})";
    TaskPlannerConfig cfg;
    auto fake = std::make_unique<ScriptedLLMClient>(
        std::vector<std::string>{bad, bad});
    auto* fake_ptr = fake.get();
    TaskPlanner planner(cfg, std::move(fake));

    std::unordered_map<std::string, std::string> skills = {{"math", "数学"}};
    auto plan = planner.plan("计算", skills);

    EXPECT_EQ(fake_ptr->callCount(), 2);  // exactly one retry, no loop
    EXPECT_TRUE(plan.is_single_agent);    // all dropped → single fallback
}

TEST(TaskPlannerDropTest, DropCountWrittenToPlanningSpanMetadata) {
    const std::string first = R"({"single": false, "tasks": [
        {"id": "t1", "description": "翻译文本", "skill": "translation"},
        {"id": "t2", "skill": "math"}
    ]})";
    const std::string second = R"({"single": false, "tasks": [
        {"id": "t1", "description": "翻译文本", "skill": "translation"},
        {"id": "t2", "description": "计算", "skill": "math"}
    ]})";
    TaskPlannerConfig cfg;
    TaskPlanner planner(cfg, std::make_unique<ScriptedLLMClient>(
        std::vector<std::string>{first, second}));

    agent_rpc::common::TraceContext::init("test_user", "test_ctx");
    auto* trace = agent_rpc::common::TraceContext::current();
    std::unordered_map<std::string, std::string> skills = {
        {"translation", "翻译"}, {"math", "数学"}};
    auto plan = planner.plan("翻译并计算", skills);
    (void)plan;

    bool planning_found = false;
    for (const auto& span : trace->mutableSpans()) {
        if (span.name == "planning") {
            planning_found = true;
            EXPECT_NE(span.metadata_json.find("\"dropped_tasks\":1"),
                      std::string::npos);
        }
    }
    EXPECT_TRUE(planning_found);
}

// ── P20: subtask timeout enforcement + in-flight cancellation ───────────

TEST_F(AgentRouterPropertyTest, SingleTaskLayerTimesOutAndCancelsInFlight) {
    auto agent = createAgent("slow-agent", {"math"});
    router_->addAgent(agent);

    ExecutorConfig cfg;
    cfg.subtask_timeout_seconds = 1;   // per-subtask cap (previously dead)
    cfg.global_timeout_seconds = 30;
    TaskExecutor executor(*router_, cfg);

    ExecutionPlan plan;
    plan.is_single_agent = false;
    SubTask st;
    st.id = "t1";
    st.description = "do math";
    st.required_skill = "math";
    st.preferred_agent_id = "slow-agent";
    plan.tasks.push_back(st);

    std::string cancelled_url;
    auto call_agent = [](const std::string&, const std::string&) -> std::string {
        // Simulate an agent that never answers: the worker keeps sleeping,
        // which also exercises the future-destructor join path.
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return "late answer";
    };
    auto on_cancel = [&cancelled_url](const std::string& url) {
        cancelled_url = url;
    };

    auto start = std::chrono::steady_clock::now();
    auto results = executor.execute(plan, call_agent, nullptr, on_cancel);
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();

    ASSERT_EQ(results.count("t1"), 1u);
    EXPECT_FALSE(results.at("t1").success);
    EXPECT_EQ(results.at("t1").error_message, "Subtask timeout exceeded");
    // The in-flight call was cancelled with the pre-resolved agent URL.
    EXPECT_EQ(cancelled_url, agent.url);
    // Waiting stopped at the per-subtask cap (then joined the worker); the
    // global deadline (30s) was never close to being consumed.
    EXPECT_LT(elapsed, 25);
}

TEST_F(AgentRouterPropertyTest, SingleTaskLayerCompletesNormallyWithAgentId) {
    auto agent = createAgent("fast-agent", {"math"});
    router_->addAgent(agent);

    ExecutorConfig cfg;
    cfg.subtask_timeout_seconds = 10;
    cfg.global_timeout_seconds = 30;
    TaskExecutor executor(*router_, cfg);

    ExecutionPlan plan;
    plan.is_single_agent = false;
    SubTask st;
    st.id = "t1";
    st.description = "do math";
    st.required_skill = "math";
    st.preferred_agent_id = "fast-agent";
    plan.tasks.push_back(st);

    auto call_agent = [](const std::string&, const std::string&) -> std::string {
        return "answer";
    };

    auto results = executor.execute(plan, call_agent);

    ASSERT_EQ(results.count("t1"), 1u);
    EXPECT_TRUE(results.at("t1").success);
    EXPECT_EQ(results.at("t1").result, "answer");
    EXPECT_EQ(results.at("t1").agent_id, "fast-agent");  // P14(a) backfill
}

// ── P19: fixed subtask pool + dequeue-before-run check ──────────────────

// With a 1-worker pool, the first task occupies the worker while the second
// one queues past its wait deadline. After the layer is given up (both tasks
// time out), the queued task must NOT be executed late — the dequeue check
// (deadline passed / abandoned flag) skips it, so the agent is contacted
// exactly once.
TEST_F(AgentRouterPropertyTest, P19QueuedTaskIsSkippedAfterWaitWindowExpires) {
    auto agent = createAgent("p19-agent", {"math"});
    router_->addAgent(agent);

    ExecutorConfig cfg;
    cfg.subtask_timeout_seconds = 1;
    cfg.global_timeout_seconds = 30;
    cfg.subtask_pool_size = 1;  // force queueing behind the first task
    TaskExecutor executor(*router_, cfg);

    ExecutionPlan plan;
    plan.is_single_agent = false;
    for (const char* id : {"t1", "t2"}) {
        SubTask st;
        st.id = id;
        st.description = "do math";
        st.required_skill = "math";
        st.preferred_agent_id = "p19-agent";
        plan.tasks.push_back(st);
    }

    std::atomic<int> calls{0};
    auto call_agent = [&calls](const std::string&, const std::string&) -> std::string {
        calls.fetch_add(1);
        // Occupy the single worker past both tasks' 1s wait windows.
        std::this_thread::sleep_for(std::chrono::seconds(2));
        return "late answer";
    };

    auto results = executor.execute(plan, call_agent);

    ASSERT_EQ(results.count("t1"), 1u);
    ASSERT_EQ(results.count("t2"), 1u);
    EXPECT_FALSE(results.at("t1").success);
    EXPECT_FALSE(results.at("t2").success);
    EXPECT_EQ(results.at("t1").error_message, "Subtask timeout exceeded");
    EXPECT_EQ(results.at("t2").error_message, "Subtask timeout exceeded");

    // Grace window: the worker frees up ~1s after execute() returned. A
    // missing dequeue check would execute t2 at that point (calls → 2).
    std::this_thread::sleep_for(std::chrono::seconds(3));
    EXPECT_EQ(calls.load(), 1);
}

// ── P24 C0.5: client-disconnect propagation aborts the in-flight layer ──

// The cancellation probe fires mid-wait; the collector must notice it within
// the 200ms probe slice, abort the in-flight call via on_cancel, and return
// promptly instead of waiting out the full (30s) subtask budget.
TEST_F(AgentRouterPropertyTest, P24ClientDisconnectAbortsInFlightLayerTask) {
    auto agent = createAgent("p24-agent", {"math"});
    router_->addAgent(agent);

    ExecutorConfig cfg;
    cfg.subtask_timeout_seconds = 30;  // no per-subtask timeout: the probe wins
    cfg.global_timeout_seconds = 30;
    TaskExecutor executor(*router_, cfg);

    ExecutionPlan plan;
    plan.is_single_agent = false;
    SubTask st;
    st.id = "t1";
    st.description = "do math";
    st.required_skill = "math";
    st.preferred_agent_id = "p24-agent";
    plan.tasks.push_back(st);

    auto call_agent = [](const std::string&, const std::string&) -> std::string {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return "late answer";
    };

    std::string cancelled_url;
    auto on_cancel = [&cancelled_url](const std::string& url) {
        cancelled_url = url;
    };
    const auto probe_start = std::chrono::steady_clock::now();
    auto cancelled = [&probe_start]() {
        return std::chrono::steady_clock::now() - probe_start >
               std::chrono::milliseconds(500);
    };

    auto start = std::chrono::steady_clock::now();
    auto results = executor.execute(plan, call_agent, nullptr, on_cancel, cancelled);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    ASSERT_EQ(results.count("t1"), 1u);
    EXPECT_FALSE(results.at("t1").success);
    EXPECT_NE(results.at("t1").error_message.find("client disconnected"),
              std::string::npos);
    // The in-flight call was aborted with the pre-resolved agent URL.
    EXPECT_EQ(cancelled_url, agent.url);
    // Returned right after the probe fired (~0.5s + one probe slice), not
    // after the 5s fake call or the 30s budget.
    EXPECT_LT(elapsed, 3000);
}

// ── P21: SSRF URL validation (L1 always-on; L2 resolution blacklist) ─────

TEST(AgentUrlValidationTest, RejectsBadSchemesUserinfoAndAmbiguity) {
    std::string err;
    EXPECT_FALSE(agent_rpc::a2a_adapter::validateAgentUrl("", err));
    EXPECT_FALSE(agent_rpc::a2a_adapter::validateAgentUrl("file:///etc/passwd", err));
    EXPECT_FALSE(agent_rpc::a2a_adapter::validateAgentUrl("ftp://host/", err));
    EXPECT_FALSE(agent_rpc::a2a_adapter::validateAgentUrl("http://user:pass@host/", err));
    EXPECT_FALSE(agent_rpc::a2a_adapter::validateAgentUrl(" HTTP://host/", err));
    EXPECT_FALSE(agent_rpc::a2a_adapter::validateAgentUrl("http://evil.com\\@127.0.0.1/", err));
    EXPECT_FALSE(agent_rpc::a2a_adapter::validateAgentUrl("http://", err));
}

TEST(AgentUrlValidationTest, AcceptsCleanUrlsCaseInsensitiveScheme) {
    std::string err;
    EXPECT_TRUE(agent_rpc::a2a_adapter::validateAgentUrl("http://127.0.0.1:5100/api", err));
    EXPECT_TRUE(agent_rpc::a2a_adapter::validateAgentUrl("https://example.com", err));
    // Case-insensitive scheme: legacy prefix check rejected this; the real
    // parse accepts it as the same http scheme.
    EXPECT_TRUE(agent_rpc::a2a_adapter::validateAgentUrl("HTTP://example.com", err));
}

TEST(AgentUrlValidationTest, SplitExtractsHostAndPort) {
    std::string host;
    std::string port;
    ASSERT_TRUE(agent_rpc::a2a_adapter::splitAgentUrlHostPort(
        "http://agent:8080/v1", host, port));
    EXPECT_EQ(host, "agent");
    EXPECT_EQ(port, "8080");

    ASSERT_TRUE(agent_rpc::a2a_adapter::splitAgentUrlHostPort(
        "https://agent.example.com", host, port));
    EXPECT_EQ(host, "agent.example.com");
    EXPECT_TRUE(port.empty());
}

TEST(AgentUrlValidationTest, HostResolutionRejectsForbiddenRanges) {
    std::vector<std::string> ips;
    std::string err;
    EXPECT_FALSE(agent_rpc::a2a_adapter::validateResolvedHost("127.0.0.1", ips, err));
    EXPECT_FALSE(agent_rpc::a2a_adapter::validateResolvedHost("169.254.169.254", ips, err));
    EXPECT_FALSE(agent_rpc::a2a_adapter::validateResolvedHost("10.0.0.5", ips, err));
    EXPECT_FALSE(agent_rpc::a2a_adapter::validateResolvedHost("192.168.1.1", ips, err));
    EXPECT_FALSE(agent_rpc::a2a_adapter::validateResolvedHost("172.16.0.1", ips, err));
    EXPECT_FALSE(agent_rpc::a2a_adapter::validateResolvedHost("::1", ips, err));
}

TEST(AgentUrlValidationTest, HostResolutionAcceptsPublicIpAndUsesCache) {
    // Numeric public address: no DNS dependency in the test environment.
    std::vector<std::string> ips;
    std::string err;
    ASSERT_TRUE(agent_rpc::a2a_adapter::validateResolvedHost("8.8.8.8", ips, err));
    EXPECT_FALSE(ips.empty());

    // Second call is served from the TTL cache with identical results.
    std::vector<std::string> ips2;
    EXPECT_TRUE(agent_rpc::a2a_adapter::validateResolvedHost("8.8.8.8", ips2, err));
    EXPECT_EQ(ips, ips2);
}

// Main

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
