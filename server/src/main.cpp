/**
 * @file main.cpp
 * @brief RPC server entry point.
 *
 * Serves gRPC requests from the client, coordinates multiple agents via the
 * A2A protocol through the Orchestrator, and supports AI queries with
 * streaming responses.
 *
 * Architecture:
 *   rpc_client ──gRPC──> rpc_server ──A2A/HTTP──> Orchestrator ──> Agents
 */

#include "agent_rpc/server/rpc_server.h"
#include "agent_rpc/server/ai_query_service.h"
#include "agent_rpc/a2a_adapter/a2a_config.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/env_loader.h"
#include "agent_rpc/common/background_scheduler.h"
#include "agent_rpc/orchestrator/feedback_aggregator.h"
#include "agent_rpc/common/profile_summarizer.h"
#include "agent_rpc/common/trace_context.h"
#include "agent_rpc/common/redis_client.h"
#include "agent_rpc/common/postgres_store.h"
#include "agent_rpc/db/migration_runner.h"
#include "agent_rpc/registry/service_registry.h"
#include <curl/curl.h>
#include <deque>
#include <filesystem>
#include <iostream>
#include <signal.h>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <queue>
#include <mutex>
#include <unordered_map>

#ifndef NEXUSAI_MIGRATIONS_DEFAULT_DIR
#error "NEXUSAI_MIGRATIONS_DEFAULT_DIR must be provided by server/CMakeLists.txt"
#endif

using namespace agent_rpc::server;
using namespace agent_rpc::common;

// Global state for graceful shutdown
std::atomic<bool> g_running{true};
RpcServer* g_server = nullptr;

// Global span queue for the span_batch_flush BackgroundScheduler task.
// TraceContext::endSpan() pushes completed spans here via the SpanExporter
// callback; the periodic task drains them into Redis in batches.
namespace {

struct QueuedSpan {
    std::string trace_id;
    std::string user_id;
    std::string span_id;
    std::string name;
    std::string component;
    int duration_ms;
    std::string status;
    std::string metadata_json;
};

std::mutex g_span_queue_mutex;
// deque (not queue): the flush task re-queues an undelivered batch at the
// FRONT so span order within a trace is preserved across Redis outages.
std::deque<QueuedSpan> g_span_queue;

static constexpr size_t kSpanBatchMax = 500;   // max spans per flush
// Spans are transient telemetry (P23): when Redis is down for a long time
// the queue must not grow without bound — drop the OLDEST spans past the
// cap instead of buffering forever.
static constexpr size_t kSpanQueueMax = 10000;

void pushSpanToQueue(const Span& span, const std::string& trace_id,
                     const std::string& user_id) {
    QueuedSpan qs;
    qs.trace_id    = trace_id;
    qs.user_id     = user_id;
    qs.span_id     = span.span_id;
    qs.name        = span.name;
    qs.component   = span.component;
    qs.duration_ms = span.duration_ms;
    qs.status      = span.status;
    qs.metadata_json = span.metadata_json;
    std::lock_guard<std::mutex> lock(g_span_queue_mutex);
    g_span_queue.push_back(std::move(qs));
    while (g_span_queue.size() > kSpanQueueMax) {
        g_span_queue.pop_front();
    }
}

std::filesystem::path resolveMigrationDirectory() {
    if (const char* environment_directory = std::getenv("NEXUSAI_MIGRATIONS_DIR");
        environment_directory != nullptr && *environment_directory != '\0') {
        return environment_directory;
    }
    return NEXUSAI_MIGRATIONS_DEFAULT_DIR;
}

}  // anonymous namespace

void signalHandler(int signal) {
    std::cout << "\n收到信号 " << signal << ", 正在关闭服务器..." << std::endl;
    g_running = false;
    // Do not call stop() from the signal handler; the main loop handles shutdown
}

void crashHandler(int sig) {
    // Best-effort crash diagnostics: flush the async logger before terminating.
    // NOTE: signal handlers run in a restricted context — avoid heap allocation,
    // non-reentrant functions, and I/O beyond async-signal-safe write().
    const char* names[] = {
        "UNKNOWN", "SIGHUP", "SIGINT", "SIGQUIT", "SIGILL",
        "SIGTRAP", "SIGABRT", "SIGBUS", "SIGFPE", "SIGKILL",
        "SIGUSR1", "SIGSEGV", "SIGUSR2", "SIGPIPE", "SIGALRM",
        "SIGTERM"
    };
    const char* name = (sig > 0 && sig < 16) ? names[sig] : "UNKNOWN";
    // Write directly to stderr (async-signal-safe).
    // NOTE: We intentionally do NOT call flushLogger() here — it acquires
    // mutexes and is not async-signal-safe. If the crash interrupted code
    // holding any of those mutexes, the handler would deadlock and never
    // produce a core dump.
    const char msg[] = "\n[FATAL] rpc_server crashed with signal ";
    if (write(STDERR_FILENO, msg, sizeof(msg) - 1) < 0) {}
    if (write(STDERR_FILENO, name, strlen(name)) < 0) {}
    const char msg2[] = "\n[FATAL] Check logs for last recorded entries before this point.\n";
    if (write(STDERR_FILENO, msg2, sizeof(msg2) - 1) < 0) {}
    // Re-raise the signal with default handler to produce core dump
    signal(sig, SIG_DFL);
    raise(sig);
}

void printUsage(const char* program) {
    std::cout << "RPC Server - AI Agent 通信服务端" << std::endl;
    std::cout << std::endl;
    std::cout << "用法: " << program << " [选项]" << std::endl;
    std::cout << std::endl;
    std::cout << "选项:" << std::endl;
    std::cout << "  -p, --port PORT           gRPC 监听端口 (默认: 50051)" << std::endl;
    std::cout << "  -o, --orchestrator URL    Orchestrator 地址 (默认: http://localhost:5000)" << std::endl;
    std::cout << "  -r, --registry ADDR       注册中心地址，例如 consul://127.0.0.1:8500" << std::endl;
    std::cout << "      --enable-registry     显式启用服务注册" << std::endl;
    std::cout << "  -t, --timeout SECONDS     请求超时时间 (默认: 60)" << std::endl;
    std::cout << "  -h, --help                显示帮助信息" << std::endl;
    std::cout << std::endl;
    std::cout << "环境变量:" << std::endl;
    std::cout << "  RPC_SERVER_PORT           gRPC 监听端口" << std::endl;
    std::cout << "  ORCHESTRATOR_URL          Orchestrator 地址" << std::endl;
    std::cout << "  RPC_REGISTRY_ADDRESS      注册中心地址" << std::endl;
    std::cout << std::endl;
    std::cout << "示例:" << std::endl;
    std::cout << "  " << program << std::endl;
    std::cout << "  " << program << " -p 50051 -o http://localhost:5000" << std::endl;
    std::cout << std::endl;
    std::cout << "启动顺序:" << std::endl;
    std::cout << "  1. 启动 ai_orchestrator 系统: ./start_system.sh" << std::endl;
    std::cout << "  2. 启动 rpc_server: ./rpc_server" << std::endl;
    std::cout << "  3. 使用 rpc_client 连接: ./rpc_client localhost:50051" << std::endl;
}

int main(int argc, char* argv[]) {
    // Initialize CURL globally once before any threads are created.
    // This avoids the undefined behavior of calling curl_global_init from
    // multiple modules concurrently. Subsequent calls from individual modules
    // are safe (libcurl >= 7.36.0 uses reference counting).
    curl_global_init(CURL_GLOBAL_ALL);

    // Load .env before any getenv calls
    agent_rpc::common::loadEnvFile(".env");

    std::string port = "50051";
    std::string orchestrator_url = "http://localhost:5000";
    std::string registry_address = "localhost:8500";
    bool enable_registry = false;
    int timeout_seconds = 60;

    if (const char* env_port = std::getenv("RPC_SERVER_PORT")) {
        port = env_port;
    }
    if (const char* env_url = std::getenv("ORCHESTRATOR_URL")) {
        orchestrator_url = env_url;
    }
    if (const char* env_registry = std::getenv("RPC_REGISTRY_ADDRESS")) {
        registry_address = env_registry;
        enable_registry = true;
    }
    
    // Parse command-line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port = argv[++i];
        } else if ((arg == "-o" || arg == "--orchestrator") && i + 1 < argc) {
            orchestrator_url = argv[++i];
        } else if ((arg == "-r" || arg == "--registry") && i + 1 < argc) {
            registry_address = argv[++i];
            enable_registry = true;
        } else if (arg == "--enable-registry") {
            enable_registry = true;
        } else if ((arg == "-t" || arg == "--timeout") && i + 1 < argc) {
            timeout_seconds = std::atoi(argv[++i]);
        } else {
            std::cerr << "未知参数: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }
    
    // Install signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    // Crash signal handlers for diagnostic logging before termination
    signal(SIGSEGV, crashHandler);
    signal(SIGABRT, crashHandler);
    signal(SIGFPE, crashHandler);
    signal(SIGILL, crashHandler);

    LogConfig log_config;
    log_config.level = LogLevel::Level_INFO;
    log_config.async_logging = true;
    log_config.color_output = true;
    initializeAdvancedLogger(log_config);

    // PostgreSQL is the durable source of truth. Apply migrations before
    // constructing or initializing the RPC server so failures fail closed.
    try {
        const auto postgres_config = PostgresConfig::fromEnvironment();
        PostgresStore postgres_store{postgres_config};
        agent_rpc::db::MigrationRunner migration_runner{postgres_store};
        migration_runner.migrate(resolveMigrationDirectory());
    } catch (const std::exception& error) {
        LOG_ERROR("RPC startup migration failed: " + std::string(error.what()));
        std::cerr << "Error: RPC startup migration failed: " << error.what() << std::endl;
        return 1;
    }
    
    // Configure RPC server
    RpcConfig config;
    config.server_address = "0.0.0.0:" + port;
    config.max_message_size = 64 * 1024 * 1024;  // 64MB
    config.max_receive_message_size = 64 * 1024 * 1024;
    config.timeout_seconds = timeout_seconds;
    config.log_level = "INFO";
    config.enable_service_registry = enable_registry;
    if (enable_registry) {
        config.registry_address = registry_address;
    }
    
    // Configure A2A adapter
    agent_rpc::a2a_adapter::A2AConfig a2a_config;
    a2a_config.orchestrator_url = orchestrator_url;
    a2a_config.request_timeout_seconds = timeout_seconds;
    
    // Create and initialize the server
    RpcServer server;
    g_server = &server;
    
    server.setA2AConfig(a2a_config);
    
    LOG_INFO("正在初始化 RPC Server...");
    
    if (!server.initialize(config)) {
        LOG_ERROR("无法初始化 RPC 服务器");
        std::cerr << "错误: 无法初始化 RPC 服务器" << std::endl;
        return 1;
    }
    
    // Check AI query service availability
    auto ai_service = server.getAIQueryService();
    bool ai_available = ai_service && ai_service->isAvailable();
    
    // Start the server
    if (!server.start()) {
        LOG_ERROR("无法启动 RPC 服务器");
        std::cerr << "错误: 无法启动 RPC 服务器" << std::endl;
        return 1;
    }
    
    // Print startup information
    std::cout << "==========================================" << std::endl;
    std::cout << "RPC Server 启动成功" << std::endl;
    std::cout << "==========================================" << std::endl;
    std::cout << "gRPC 地址:      " << config.server_address << std::endl;
    std::cout << "Orchestrator:   " << orchestrator_url << std::endl;
    if (enable_registry) {
        std::cout << "Registry:       " << registry_address << std::endl;
    }
    std::cout << "AI 服务状态:    " << (ai_available ? "可用" : "不可用") << std::endl;
    std::cout << "超时时间:       " << timeout_seconds << " 秒" << std::endl;
    std::cout << std::endl;
    std::cout << "使用客户端连接:" << std::endl;
    std::cout << "  ./rpc_client localhost:" << port << std::endl;
    std::cout << std::endl;
    std::cout << "按 Ctrl+C 停止服务器" << std::endl;
    std::cout << "==========================================" << std::endl;
    
    LOG_INFO("RPC Server 已启动: " + config.server_address);

    // B4 (P14d) startup recovery: load fresh durable health snapshots
    // (V014 agent_health_snapshots) into the registry's live metrics so the
    // first post-restart evaluation continues from the persisted verdict
    // instead of a cold start. Freshness gate: snapshots older than 10
    // minutes are skipped — reviving a stale verdict could exclude a now
    // healthy agent; per-call samples themselves stay in memory only.
    if (auto* health_repo = server.getQueryDomainRepository()) {
        try {
            constexpr std::int64_t kSnapshotMaxAgeSeconds = 600;
            const auto snapshots =
                health_repo->listAgentHealthSnapshots(kSnapshotMaxAgeSeconds);
            for (const auto& snapshot : snapshots) {
                agent_rpc::registry::ServiceRegistry::seedLiveMetricsBaseline(
                    snapshot.agent_id, snapshot.success_rate,
                    snapshot.ema_latency_ms, snapshot.total_calls);
            }
            LOG_INFO("Health baseline seeded from " +
                     std::to_string(snapshots.size()) + " durable snapshot(s)");
        } catch (const std::exception& error) {
            // PG unavailable at startup: cold-start behavior (samples < 20
            // are never evaluated) keeps this safe.
            LOG_WARN(std::string("Health snapshot seeding skipped: ") + error.what());
        }
    }

    // Start BackgroundScheduler for periodic tasks
    agent_rpc::common::BackgroundScheduler::instance().start(2);

    // Register feedback aggregation and metrics aggregation tasks (hourly)
    agent_rpc::common::BackgroundScheduler::instance().scheduleAtFixedRate(
        "feedback_aggregation",
        []() { agent_rpc::orchestrator::FeedbackAggregator::recalculate(); },
        std::chrono::seconds(3600));
    // NOTE: the production writer for agent_invocations is the
    // Query/QueryStream pipeline (AIQueryServiceImpl for the single-agent
    // A2A path, MultiAgentHandler for the orchestrator path — wired in
    // RpcServer::initialize, final wrap-up). This hourly task therefore
    // aggregates real invocation facts and refreshes the Redis
    // agent_metrics cache whenever rows exist.
    agent_rpc::common::BackgroundScheduler::instance().scheduleAtFixedRate(
        "agent_metrics_aggregation",
        []() { agent_rpc::orchestrator::FeedbackAggregator::recalculateMetrics(); },
        std::chrono::seconds(3600));


    // Register profile extraction task (every 5 minutes)
    // Calls ProfileSummarizer::processPending(), which performs real work
    // (reads pending users from Redis and calls the LLM when LLM_API_KEY is
    // set). Known limitation: the extracted profiles are written back to
    // Redis only — they are not persisted to PostgreSQL yet, so treat them
    // as cache-tier data.
    agent_rpc::common::BackgroundScheduler::instance().scheduleAtFixedRate(
        "profile_extraction",
        [&server]() {
            // P17(m): PostgreSQL conversation history is the extraction
            // material source (the Redis Tier-1 mirror stays empty).
            agent_rpc::common::ProfileSummarizer::processPending(
                server.getQueryDomainRepository());
        },
        std::chrono::seconds(300));

    // Register health evaluation task (every 30 seconds)
    agent_rpc::common::BackgroundScheduler::instance().scheduleAtFixedRate(
        "health_evaluation",
        [&server]() {
            auto ai_service = server.getAIQueryService();
            auto* router = ai_service ? ai_service->getAgentRouter() : nullptr;
            if (!router) {
                return;
            }

            // Combine heartbeat-timeout and metric-based signals into one
            // verdict per agent: UNHEALTHY excludes the agent from routing.
            // DEGRADED is observability-only (logged by the dashboard, not
            // excluded — the router flag is boolean).
            //
            // Recovery semantics (R17): a metric-driven UNHEALTHY verdict has
            // no natural recovery when the router already excludes the agent
            // (no new samples can arrive to rehabilitate the frozen ring
            // buffer). After kProbationCycles consecutive metrics-only
            // UNHEALTHY verdicts (~60s), the agent is re-admitted
            // (markAgentHealthy) as a half-open probe: fresh traffic either
            // rehabilitates it or the next verdicts re-latch it. A dead
            // heartbeat is NOT probated — it self-heals only when the agent
            // actually calls home again.
            constexpr int kProbationCycles = 2;
            struct Verdict {
                bool heartbeat_timeout = false;
                bool metrics_unhealthy = false;
            };
            static std::mutex probation_mutex;
            static std::unordered_map<std::string, int> metrics_unhealthy_streak;
            std::unordered_map<std::string, Verdict> verdicts;
            auto accumulate = [&verdicts](const std::string& id, bool heartbeat,
                                          bool metrics) {
                auto& v = verdicts[id];
                v.heartbeat_timeout = v.heartbeat_timeout || heartbeat;
                v.metrics_unhealthy = v.metrics_unhealthy || metrics;
            };

            // Heartbeat timeout guard (90s): agents that stop calling home
            // are excluded; a fresh heartbeat restores them.
            agent_rpc::registry::ServiceRegistry::evaluateHeartbeatTimeouts(
                std::chrono::seconds(90),
                [&accumulate](const std::string& agent_id, bool timed_out) {
                    accumulate(agent_id, timed_out, false);
                });

            // Metric-based evaluation: write results back to the router so
            // the is_healthy flag drives the 11 routing exclusion checks.
            // B4 (P14d): the detailed callback also persists each verdict as
            // a durable snapshot (restart recovery). PG faults degrade to a
            // warning — the in-memory routing decision is unaffected.
            agent_rpc::registry::ServiceRegistry::evaluateAllHealthDetailed(
                [&accumulate, &server](const std::string& agent_id,
                                       agent_rpc::registry::HealthStatus status,
                                       double success_rate, double ema_latency_ms,
                                       int total_writes) {
                    accumulate(agent_id, false,
                               status == agent_rpc::registry::HealthStatus::UNHEALTHY);
                    auto* repo = server.getQueryDomainRepository();
                    if (!repo) {
                        return;
                    }
                    try {
                        agent_rpc::common::AgentHealthSnapshotRecord snapshot;
                        snapshot.agent_id = agent_id;
                        snapshot.health_status =
                            status == agent_rpc::registry::HealthStatus::HEALTHY ? "HEALTHY" :
                            status == agent_rpc::registry::HealthStatus::DEGRADED ? "DEGRADED" :
                            status == agent_rpc::registry::HealthStatus::UNHEALTHY ? "UNHEALTHY" :
                            "UNKNOWN";
                        snapshot.success_rate = success_rate;
                        snapshot.ema_latency_ms = ema_latency_ms;
                        snapshot.total_calls = total_writes;
                        repo->upsertAgentHealthSnapshot(snapshot);
                    } catch (const std::exception& snapshot_error) {
                        LOG_WARN("Health snapshot persist failed for " + agent_id +
                                 ": " + snapshot_error.what());
                    }
                });

            for (const auto& [agent_id, verdict] : verdicts) {
                const bool unhealthy =
                    verdict.heartbeat_timeout || verdict.metrics_unhealthy;
                if (!unhealthy) {
                    {
                        std::lock_guard<std::mutex> lock(probation_mutex);
                        metrics_unhealthy_streak.erase(agent_id);
                    }
                    router->markAgentHealthy(agent_id);
                    continue;
                }
                if (verdict.heartbeat_timeout) {
                    {
                        std::lock_guard<std::mutex> lock(probation_mutex);
                        metrics_unhealthy_streak.erase(agent_id);
                    }
                    router->markAgentUnhealthy(agent_id);
                    continue;
                }
                // Metrics-only UNHEALTHY: probation counter decides.
                int streak = 0;
                {
                    std::lock_guard<std::mutex> lock(probation_mutex);
                    streak = ++metrics_unhealthy_streak[agent_id];
                }
                if (streak >= kProbationCycles) {
                    {
                        std::lock_guard<std::mutex> lock(probation_mutex);
                        metrics_unhealthy_streak[agent_id] = 0;
                    }
                    router->markAgentHealthy(agent_id);
                    LOG_INFO("Agent " + agent_id +
                             " re-admitted to routing after probation (UNHEALTHY x" +
                             std::to_string(kProbationCycles) +
                             "); fresh samples will decide the next verdict");
                } else {
                    router->markAgentUnhealthy(agent_id);
                }
            }
        },
        std::chrono::seconds(30));

    // The legacy CronScheduler and canary-evaluation background tasks
    // were removed per the local delivery boundary and are NOT planned to be
    // rebuilt. Canary weighting was dropped together with the CANARY/DEPRECATED
    // router modifiers — routing quality is now owner-scoped PostgreSQL data.

    // Span batch flush (every 1 second): drains completed spans from the
    // global queue and writes them to Redis as a JSON list under
    // key "trace:spans:<trace_id>".

    // Register the SpanExporter callback so TraceContext::endSpan() pushes
    // completed spans into the global queue.
    agent_rpc::common::TraceContext::setSpanExporter(pushSpanToQueue);

    agent_rpc::common::BackgroundScheduler::instance().scheduleAtFixedRate(
        "span_batch_flush",
        [&server]() {
            std::deque<QueuedSpan> batch;
            {
                std::lock_guard<std::mutex> lock(g_span_queue_mutex);
                size_t count = std::min(g_span_queue.size(), kSpanBatchMax);
                for (size_t i = 0; i < count; ++i) {
                    batch.push_back(std::move(g_span_queue.front()));
                    g_span_queue.pop_front();
                }
            }

            if (batch.empty()) return;

            auto* redis = server.getRedisClient();
            if (!redis || !redis->isConnected()) {
                // Redis unavailable — put the batch back at the FRONT so the
                // per-trace span order survives the outage, then re-apply the
                // hard cap (transient telemetry: drop oldest, never grow
                // unbounded).
                size_t requeued = batch.size();
                {
                    std::lock_guard<std::mutex> lock(g_span_queue_mutex);
                    while (!batch.empty()) {
                        g_span_queue.push_front(std::move(batch.back()));
                        batch.pop_back();
                    }
                    while (g_span_queue.size() > kSpanQueueMax) {
                        g_span_queue.pop_front();
                    }
                }
                LOG_WARN("Span batch flush: Redis unavailable, re-queued " +
                         std::to_string(requeued) + " spans");
                return;
            }

            // Group spans by trace_id and RPUSH each batch as a JSON string.
            // nlohmann::json handles the escaping; metadata_json (when the
            // span carries one) is embedded so Redis mirrors the PG payload.
            size_t flushed = 0;
            while (!batch.empty()) {
                const auto& qs = batch.front();
                nlohmann::json span_json;
                span_json["trace_id"] = qs.trace_id;
                // owner_id lets the GetTraceDetail Redis fallback enforce
                // ownership (the key is trace_id-scoped only).
                span_json["owner_id"] = qs.user_id;
                span_json["span_id"] = qs.span_id;
                span_json["name"] = qs.name;
                span_json["component"] = qs.component;
                span_json["duration_ms"] = qs.duration_ms;
                span_json["status"] = qs.status;
                if (!qs.metadata_json.empty()) {
                    try {
                        span_json["metadata"] = nlohmann::json::parse(qs.metadata_json);
                    } catch (const nlohmann::json::exception&) {
                        // Malformed metadata: keep the raw string for inspection.
                        span_json["metadata_raw"] = qs.metadata_json;
                    }
                }

                std::string key = "trace:spans:" + qs.trace_id;
                redis->rpush(key, span_json.dump());
                // Set 24h TTL on the trace key (refreshed on each push)
                redis->expire(key, 86400);
                ++flushed;
                batch.pop_front();
            }

            LOG_DEBUG("Span batch flush: wrote " + std::to_string(flushed) + " spans to Redis");
        },
        std::chrono::seconds(1));

    // Main loop
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Stop BackgroundScheduler before shutting down server
    agent_rpc::common::BackgroundScheduler::instance().stop();

    // Stop the server
    server.stop();
    LOG_INFO("RPC Server 已停止");
    std::cout << "RPC 服务器已停止" << std::endl;

    curl_global_cleanup();

    return 0;
}
