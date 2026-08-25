/**
 * @file test_agent_router_properties.cpp
 * @brief Property-based tests for Agent Router
 */

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

// AgentRouter's member layout is guarded by AGENT_RPC_ENABLE_MCP (three
// extra unique_ptr members in MCP builds). The orchestrator library defines
// that macro PRIVATE (see orchestrator/CMakeLists.txt), so this TU must
// mirror it or it would see a different layout than liborchestrator.a —
// an ODR violation that corrupts the heap. Detection: orchestrator links
// agent_rpc_mcp PUBLIC only in ENABLE_MCP builds, which propagates mcp's
// PUBLIC include dirs to this target; their absence means the default
// (non-MCP) layout. Do NOT define the macro unconditionally.
#if defined(__has_include)
#if __has_include("agent_rpc/mcp/rag/embedding_service.h")
#ifndef AGENT_RPC_ENABLE_MCP
#define AGENT_RPC_ENABLE_MCP 1
#endif
#endif
#endif

#include "agent_rpc/orchestrator/agent_router.h"
#include "agent_rpc/orchestrator/agent_info.h"
#include "agent_rpc/orchestrator/task_executor.h"
#include "agent_rpc/common/trace_context.h"
#include "agent_rpc/common/load_balancer.h"

#include <cstdlib>
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

    // An enabled request in the default (non-MCP) build is refused by the
    // stub and routing keeps its baseline behavior; in MCP builds the call
    // may succeed or degrade, but skill lookup must keep working either way.
    EmbeddingRouterConfig on_config;
    on_config.enabled = true;
    on_config.api_key = "test-key";
#ifndef AGENT_RPC_ENABLE_MCP
    EXPECT_FALSE(router.enableEmbedding(on_config));
#else
    (void)router.enableEmbedding(on_config);
#endif
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

// Main

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
