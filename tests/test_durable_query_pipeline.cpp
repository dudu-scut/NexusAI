/**
 * @file test_durable_query_pipeline.cpp
 * @brief PR-C2: Query/QueryStream durable pipeline — contract + integration tests.
 *
 * Three layers:
 *   1. Static source contracts (owner resolution, removed Redis budget
 *      middleware, single terminal emitter, fail-closed DI).
 *   2. Repository integration against real PostgreSQL (ensureConversation,
 *      appendMessageAutoSequence sequential + concurrent).
 *   3. End-to-end pipeline against a real RpcServer + real PostgreSQL +
 *      real Redis and an embedded mock A2A HTTP agent (no repository mocks).
 */

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <hiredis/hiredis.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <openssl/hmac.h>

#include <grpcpp/grpcpp.h>

#include "agent_rpc/common/postgres_store.h"
#include "agent_rpc/common/postgres_budget_repository.h"
#include "agent_rpc/common/query_domain_repository.h"
#include "agent_rpc/common/auth_repository.h"
#include "agent_rpc/common/redis_client.h"
#include "agent_rpc/a2a_adapter/a2a_config.h"
#include "agent_rpc/server/rpc_server.h"
#include "agent_rpc/server/multi_agent_handler.h"
#include "agent_rpc/server/auth_cache.h"
#include "agent_rpc/server/auth_service.h"

#include <nlohmann/json.hpp>

#include "ai_query.grpc.pb.h"
#include "ai_query.pb.h"
#include "user.grpc.pb.h"
#include "user.pb.h"

#ifndef NEXUSAI_PIPELINE_ROOT
#error "NEXUSAI_PIPELINE_ROOT must point at the repository checkout root"
#endif

namespace {

namespace common_ns = agent_rpc::common;
namespace server_ns = agent_rpc::server;

std::string rootPath() {
    return NEXUSAI_PIPELINE_ROOT;
}

std::string readFileOrEmpty(const std::string& path) {
    std::ifstream source{path, std::ios::binary};
    if (!source.is_open()) {
        return {};
    }
    return {std::istreambuf_iterator<char>{source}, std::istreambuf_iterator<char>{}};
}

std::size_t countOccurrences(const std::string& haystack, const std::string& needle) {
    std::size_t count = 0;
    std::size_t position = haystack.find(needle);
    while (position != std::string::npos) {
        ++count;
        position = haystack.find(needle, position + needle.size());
    }
    return count;
}

std::string uniqueSuffix() {
    static std::atomic<std::int64_t> counter{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(ticks) + "-" + std::to_string(counter.fetch_add(1));
}

// 1. Static source contracts

TEST(DurablePipelineContractTest, QueryServiceUsesAuthenticatedOwnerOnly) {
    const std::string service = readFileOrEmpty(rootPath() + "/server/src/ai_query_service.cpp");
    ASSERT_FALSE(service.empty());

    EXPECT_NE(service.find("AuthInterceptor::currentUserId()"), std::string::npos);
    EXPECT_NE(service.find("enriched_req.set_user_id(owner_id)"), std::string::npos);
    // The request-body-first user_id fallback must be gone.
    EXPECT_EQ(service.find("std::string user_id = request->user_id();"), std::string::npos);
    // Redis micro-dollar budget middleware removed from the Query path.
    EXPECT_EQ(service.find("BudgetMiddleware::checkAndDeduct"), std::string::npos);
}

TEST(DurablePipelineContractTest, QueryServicePersistsThroughDurableRepositories) {
    const std::string service = readFileOrEmpty(rootPath() + "/server/src/ai_query_service.cpp");
    ASSERT_FALSE(service.empty());

    EXPECT_NE(service.find("ensureConversation("), std::string::npos);
    EXPECT_NE(service.find("appendMessageAutoSequence("), std::string::npos);
    EXPECT_NE(service.find("budget_repo_->reserve("), std::string::npos);
    EXPECT_NE(service.find("buildSystemContextFromPg("), std::string::npos);
    EXPECT_NE(service.find("finalizeDurableQuery("), std::string::npos);
    // Exactly-once finalize guard.
    EXPECT_NE(service.find("compare_exchange_strong(expected, true)"), std::string::npos);
    // Estimate-only token accounting.
    EXPECT_NE(service.find("usage.estimated = true"), std::string::npos);
    // Crash guard wraps both Query and QueryStream pipelines.
    EXPECT_GE(countOccurrences(service, "abortDurableRun(run, error.what())"), 2u);
}

TEST(DurablePipelineContractTest, QueryCrashGuardsReturnFixedSanitizedMessages) {
    // Final review M-2 (aligned with PR-D M1): the Query/QueryStream crash
    // guards return fixed, sanitized status messages. Raw exception detail
    // (pqxx::sql_error::what() embeds the failing SQL statement) stays in
    // the server log and never rides the response.
    const std::string service = readFileOrEmpty(rootPath() + "/server/src/ai_query_service.cpp");
    ASSERT_FALSE(service.empty());

    // The old echo paths (prefix + error.what()) are gone.
    EXPECT_EQ(service.find("PostgreSQL persistence error: "), std::string::npos);
    EXPECT_EQ(service.find("Unexpected query pipeline error: "), std::string::npos);
    EXPECT_EQ(service.find("sanitizeErrorMessage(std::string(persistence_fault"),
              std::string::npos);

    // Fixed sanitized texts cover both crash guards (Query + QueryStream).
    EXPECT_EQ(countOccurrences(
                  service,
                  "persistence_fault ? \"Query unavailable: persistence layer error\""),
              2u);
    EXPECT_EQ(countOccurrences(service, "\"Query failed unexpectedly\""), 2u);
    // error.what() survives only in the crash-guard log lines.
    EXPECT_EQ(countOccurrences(
                  service,
                  "LOG_ERROR(std::string(\"Query pipeline crashed: \") + error.what());"),
              2u);
}

TEST(DurablePipelineContractTest, QueryServiceReadsExplicitTokenBudgetQuotas) {
    const std::string service = readFileOrEmpty(rootPath() + "/server/src/ai_query_service.cpp");
    ASSERT_FALSE(service.empty());

    for (const char* name : {"NEXUSAI_BUDGET_GLOBAL_TOKENS",
                             "NEXUSAI_BUDGET_USER_DAILY_TOKENS",
                             "NEXUSAI_BUDGET_USER_MONTHLY_TOKENS",
                             "NEXUSAI_BUDGET_SESSION_TOKENS"}) {
        EXPECT_NE(service.find(name), std::string::npos) << name;
    }
}

TEST(DurablePipelineContractTest, QueryStreamEmitsTerminalEventsExactlyOnce) {
    const std::string service = readFileOrEmpty(rootPath() + "/server/src/ai_query_service.cpp");
    ASSERT_FALSE(service.empty());

    EXPECT_NE(service.find("std::atomic<bool> terminal_emitted{false};"), std::string::npos);
    EXPECT_NE(service.find("terminal_emitted.compare_exchange_strong"), std::string::npos);
    // Relay filters lower-layer terminal events before forwarding.
    EXPECT_NE(service.find("event.event_type() == \"complete\""), std::string::npos);
    EXPECT_NE(service.find("event.event_type() == \"error\""), std::string::npos);
}

TEST(DurablePipelineContractTest, MultiAgentHandlerNeverEmitsTerminalEvents) {
    const std::string handler = readFileOrEmpty(rootPath() + "/server/src/multi_agent_handler.cpp");
    ASSERT_FALSE(handler.empty());

    EXPECT_EQ(countOccurrences(handler, "set_event_type(\"complete\")"), 0u);
    EXPECT_EQ(countOccurrences(handler, "set_event_type(\"error\")"), 0u);
    EXPECT_NE(handler.find("context->IsCancelled()"), std::string::npos);
    EXPECT_NE(handler.find("writer->Write(event))"), std::string::npos)
        << "relay must check the writer result";
}

TEST(DurablePipelineContractTest, RpcServerOwnsRepositoriesAndFailsClosed) {
    const std::string rpc_server = readFileOrEmpty(rootPath() + "/server/src/rpc_server.cpp");
    ASSERT_FALSE(rpc_server.empty());

    EXPECT_NE(rpc_server.find("QueryDomainRepository"), std::string::npos);
    EXPECT_NE(rpc_server.find("PostgresBudgetRepository"), std::string::npos);
    EXPECT_EQ(rpc_server.find("continuing without it"), std::string::npos);
}

TEST(DurablePipelineContractTest, ProxyTracksCompleteSeenAndCancelsOnClose) {
    const std::string proxy = readFileOrEmpty(rootPath() + "/gateway/proxy/server.mjs");
    ASSERT_FALSE(proxy.empty());

    EXPECT_NE(proxy.find("let completeSeen = false;"), std::string::npos);
    EXPECT_NE(proxy.find("if (!completeSeen)"), std::string::npos);
    EXPECT_NE(proxy.find("stream.cancel()"), std::string::npos);
    // Only one synthesized fallback complete payload exists in the proxy.
    EXPECT_EQ(countOccurrences(proxy, "event_type: 'complete'"), 1u);
}

// 2. Repository integration (real PostgreSQL)

class DurableRepositoryTest : public ::testing::Test {
protected:
    struct Context {
        std::unique_ptr<common_ns::PostgresStore> store;
        std::unique_ptr<common_ns::QueryDomainRepository> repository;
    };

    static std::unique_ptr<Context> makeContext(int pool_size) {
        try {
            auto config = common_ns::PostgresConfig::fromEnvironment();
            config.pool_size = pool_size;
            auto context = std::make_unique<Context>();
            context->store = std::make_unique<common_ns::PostgresStore>(std::move(config));
            const std::string migration =
                readFileOrEmpty(rootPath() + "/db/migrations/V011__durable_domain.sql");
            if (migration.empty()) {
                return nullptr;
            }
            context->store->executeTransaction([&migration](pqxx::work& transaction) {
                transaction.exec(migration);
            });
            context->repository =
                std::make_unique<common_ns::QueryDomainRepository>(*context->store);
            return context;
        } catch (const std::exception&) {
            return nullptr;
        }
    }
};

TEST_F(DurableRepositoryTest, EnsureConversationCreatesIdempotentlyAndRefusesCrossOwner) {
    auto context = makeContext(2);
    if (!context) {
        GTEST_SKIP() << "PostgreSQL test DSN is unavailable";
    }

    const std::string owner_a = "dqp-owner-a-" + uniqueSuffix();
    const std::string owner_b = "dqp-owner-b-" + uniqueSuffix();
    const std::string conversation_id = "dqp-conv-" + uniqueSuffix();

    EXPECT_TRUE(context->repository->ensureConversation(owner_a, conversation_id, "first"));
    // Same owner retry is idempotent.
    EXPECT_TRUE(context->repository->ensureConversation(owner_a, conversation_id, "again"));

    auto conversation = context->repository->getConversationById(owner_a, conversation_id);
    ASSERT_TRUE(conversation.has_value());
    EXPECT_EQ(conversation->owner_id, owner_a);
    EXPECT_EQ(context->repository->listConversations(owner_a).size(), 1u);

    // Another owner cannot claim the same conversation id.
    EXPECT_FALSE(context->repository->ensureConversation(owner_b, conversation_id, "intruder"));
    EXPECT_FALSE(context->repository->getConversationById(owner_b, conversation_id).has_value());
}

TEST_F(DurableRepositoryTest, AppendMessageAutoSequenceAssignsContiguousSequences) {
    auto context = makeContext(2);
    if (!context) {
        GTEST_SKIP() << "PostgreSQL test DSN is unavailable";
    }

    const std::string owner_id = "dqp-owner-seq-" + uniqueSuffix();
    const std::string conversation_id = "dqp-conv-seq-" + uniqueSuffix();
    ASSERT_TRUE(context->repository->ensureConversation(owner_id, conversation_id, "seq"));

    for (int index = 1; index <= 3; ++index) {
        auto stored = context->repository->appendMessageAutoSequence(
            owner_id, conversation_id, "user", "message-" + std::to_string(index));
        ASSERT_TRUE(stored.has_value());
        EXPECT_EQ(stored->sequence_no, index);
    }

    // Unknown conversation / wrong owner is refused.
    EXPECT_FALSE(context->repository
                     ->appendMessageAutoSequence("dqp-stranger-" + uniqueSuffix(),
                                                 conversation_id, "user", "x")
                     .has_value());
}

TEST_F(DurableRepositoryTest, AppendMessageAutoSequenceSurvivesConcurrency) {
    auto context = makeContext(10);
    if (!context) {
        GTEST_SKIP() << "PostgreSQL test DSN is unavailable";
    }

    const std::string owner_id = "dqp-owner-conc-" + uniqueSuffix();
    const std::string conversation_id = "dqp-conv-conc-" + uniqueSuffix();
    ASSERT_TRUE(context->repository->ensureConversation(owner_id, conversation_id, "conc"));

    constexpr int kThreads = 8;
    constexpr int kPerThread = 5;
    std::mutex sequences_mutex;
    std::vector<std::int64_t> sequences;
    std::atomic<int> failures{0};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int worker = 0; worker < kThreads; ++worker) {
        workers.emplace_back([&] {
            for (int round = 0; round < kPerThread; ++round) {
                auto stored = context->repository->appendMessageAutoSequence(
                    owner_id, conversation_id, "user", "concurrent");
                if (!stored.has_value()) {
                    failures.fetch_add(1);
                    continue;
                }
                std::lock_guard<std::mutex> guard(sequences_mutex);
                sequences.push_back(stored->sequence_no);
            }
        });
    }
    for (auto& thread : workers) {
        thread.join();
    }

    EXPECT_EQ(failures.load(), 0);
    ASSERT_EQ(sequences.size(), static_cast<std::size_t>(kThreads * kPerThread));
    const std::set<std::int64_t> unique_sequences(sequences.begin(), sequences.end());
    EXPECT_EQ(unique_sequences.size(), sequences.size()) << "sequence numbers must never collide";
    EXPECT_EQ(*unique_sequences.begin(), 1);
    EXPECT_EQ(*unique_sequences.rbegin(), kThreads * kPerThread);
}

// Crash regression: concurrent first creation of the same conversation_id
// must not throw (the aborted-transaction re-check bug); both callers see
// success and exactly one owner-scoped row exists.
TEST_F(DurableRepositoryTest, EnsureConversationSurvivesConcurrentFirstCreation) {
    auto context = makeContext(4);
    if (!context) {
        GTEST_SKIP() << "PostgreSQL test DSN is unavailable";
    }

    const std::string owner_id = "dqp-owner-race-" + uniqueSuffix();
    const std::string conversation_id = "dqp-conv-race-" + uniqueSuffix();

    std::atomic<bool> gate{false};
    std::atomic<int> successes{0};
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(2);
    for (int worker = 0; worker < 2; ++worker) {
        workers.emplace_back([&] {
            // A regression of the old aborted-transaction bug must surface as
            // a failed assertion here, never as an exception escaping the
            // worker thread (which would terminate the whole test process).
            try {
                while (!gate.load()) {
                    std::this_thread::yield();
                }
                if (context->repository->ensureConversation(owner_id, conversation_id,
                                                            "race")) {
                    successes.fetch_add(1);
                }
            } catch (const std::exception&) {
                failures.fetch_add(1);
            } catch (...) {
                failures.fetch_add(1);
            }
        });
    }
    gate.store(true);
    for (auto& thread : workers) {
        thread.join();
    }

    EXPECT_EQ(failures.load(), 0) << "ensureConversation threw inside a worker";
    EXPECT_EQ(successes.load(), 2);
    EXPECT_EQ(context->repository->listConversations(owner_id).size(), 1u);
    const auto conversation =
        context->repository->getConversationById(owner_id, conversation_id);
    ASSERT_TRUE(conversation.has_value());
    EXPECT_EQ(conversation->owner_id, owner_id);
}

// 3. End-to-end pipeline (real RpcServer + PG + Redis + mock A2A agent)

// Minimal blocking HTTP server emulating an A2A agent for message/send and
// message/stream (SSE). Modes: "ok", "http500", "stream-slow".
class MockA2AHttpServer {
public:
    ~MockA2AHttpServer() {
        stop();
    }

    bool start(const std::string& mode) {
        mode_ = mode;
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return false;
        }
        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            return false;
        }
        socklen_t length = sizeof(address);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        if (::listen(listen_fd_, 16) != 0) {
            return false;
        }
        running_.store(true);
        thread_ = std::thread([this] { acceptLoop(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;
        }
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    int port() const {
        return port_;
    }

private:
    void acceptLoop() {
        while (running_.load()) {
            pollfd watched{listen_fd_, POLLIN, 0};
            const int ready = ::poll(&watched, 1, 200);
            if (ready <= 0) {
                continue;
            }
            const int client = ::accept(listen_fd_, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            handleConnection(client);
            ::close(client);
        }
    }

    static bool sendAll(int fd, const std::string& payload) {
        std::size_t sent = 0;
        while (sent < payload.size()) {
            const ssize_t written =
                ::send(fd, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
            if (written <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(written);
        }
        return true;
    }

    void handleConnection(int client) {
        std::string buffer;
        char chunk[4096];
        std::size_t header_end = std::string::npos;
        while (header_end == std::string::npos && buffer.size() < (1u << 20)) {
            const ssize_t received = ::recv(client, chunk, sizeof(chunk), 0);
            if (received <= 0) {
                return;
            }
            buffer.append(chunk, static_cast<std::size_t>(received));
            header_end = buffer.find("\r\n\r\n");
        }
        if (header_end == std::string::npos) {
            return;
        }
        std::string body = buffer.substr(header_end + 4);
        std::size_t content_length = 0;
        const std::string headers = buffer.substr(0, header_end);
        std::string lower_headers = headers;
        for (auto& character : lower_headers) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        const auto length_position = lower_headers.find("content-length:");
        if (length_position != std::string::npos) {
            content_length = std::strtoul(headers.c_str() + length_position + 15, nullptr, 10);
        }
        while (body.size() < content_length) {
            const ssize_t received = ::recv(client, chunk, sizeof(chunk), 0);
            if (received <= 0) {
                break;
            }
            body.append(chunk, static_cast<std::size_t>(received));
        }

        if (mode_ == "http500") {
            sendAll(client,
                    "HTTP/1.1 500 Internal Server Error\r\n"
                    "Content-Length: 0\r\nConnection: close\r\n\r\n");
            return;
        }

        if (body.find("message/stream") != std::string::npos) {
            respondStream(client);
        } else {
            respondSend(client);
        }
    }

    static void respondSend(int client) {
        const std::string body =
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"type\":\"message\","
            "\"message\":{\"message_id\":\"mock-1\",\"role\":\"agent\","
            "\"parts\":[{\"type\":\"text\",\"text\":\"mock-answer\"}]}}}";
        sendAll(client, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Content-Length: " + std::to_string(body.size()) +
                            "\r\nConnection: close\r\n\r\n" + body);
    }

    void respondStream(int client) {
        sendAll(client, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                        "Cache-Control: no-cache\r\nConnection: close\r\n\r\n");

        auto emit = [client](const std::string& state, const std::string& text) {
            std::string event =
                "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"type\":\"status\","
                "\"status\":{\"state\":\"" + state + "\",\"message\":{\"role\":\"agent\","
                "\"parts\":[{\"type\":\"text\",\"text\":\"" + text + "\"}]}}}}\n\n";
            return sendAll(client, event);
        };

        if (!emit("working", "thinking")) {
            return;
        }
        if (mode_ == "stream-slow") {
            // Keep the stream open long enough for the client to cancel it.
            for (int tick = 0; tick < 10 && running_.load(); ++tick) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                if (!emit("working", "still thinking")) {
                    return;
                }
            }
        }
        emit("completed", "mock-stream-answer");
    }

    std::string mode_ = "ok";
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

class DurableQueryPipelineTest : public ::testing::Test {
protected:
    struct Handles {
        std::unique_ptr<common_ns::PostgresStore> store;
        std::unique_ptr<common_ns::QueryDomainRepository> domain;
        std::unique_ptr<common_ns::PostgresBudgetRepository> budget;
    };

    struct User {
        std::string id;
        std::string token;
    };

    static int nextPort() {
        // Per-process randomized base (52160..53759): repeated full gates
        // within the same TIME_WAIT window used to re-bind the same static
        // ports and hit EADDRINUSE on the RpcServer listener (flaky Durable
        // runs under full ctest). Varying the base per process keeps each
        // test run clear of the previous process's TIME_WAIT sockets.
        static const int base = 52160 + (static_cast<int>(::getpid()) % 1600);
        static std::atomic<int> port{base};
        return port.fetch_add(1);
    }

    void SetUp() override {
        // Force the deterministic single-agent A2A path.
        ::unsetenv("LLM_API_KEY");
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
            server_.reset();
        }
        mock_.stop();
    }

    bool applyMigrations(common_ns::PostgresStore& store) {
        for (const std::string& name :
             {"V010__local_auth.sql", "V011__durable_domain.sql", "V012__postgres_budget.sql"}) {
            const std::string migration =
                readFileOrEmpty(rootPath() + "/db/migrations/" + name);
            if (migration.empty()) {
                return false;
            }
            store.executeTransaction([&migration](pqxx::work& transaction) {
                transaction.exec(migration);
            });
        }
        return true;
    }

    void startPipeline(const std::string& mock_mode) {
        ASSERT_TRUE(mock_.start(mock_mode)) << "mock A2A server failed to start";

        try {
            auto config = common_ns::PostgresConfig::fromEnvironment();
            config.pool_size = 6;
            handles_ = std::make_unique<Handles>();
            handles_->store = std::make_unique<common_ns::PostgresStore>(std::move(config));
        } catch (const std::exception&) {
            GTEST_SKIP() << "PostgreSQL test DSN is unavailable";
        }
        if (!handles_->store->healthCheck()) {
            GTEST_SKIP() << "PostgreSQL health check failed";
        }
        try {
            ASSERT_TRUE(applyMigrations(*handles_->store));
        } catch (const std::exception& error) {
            GTEST_SKIP() << "migrations failed: " << error.what();
        }
        handles_->domain =
            std::make_unique<common_ns::QueryDomainRepository>(*handles_->store);
        handles_->budget =
            std::make_unique<common_ns::PostgresBudgetRepository>(*handles_->store);

        server_ = std::make_unique<server_ns::RpcServer>();
        common_ns::RpcConfig rpc_config;
        rpc_config.server_address = "127.0.0.1:" + std::to_string(nextPort());
        rpc_config.enable_service_registry = false;

        agent_rpc::a2a_adapter::A2AConfig a2a_config;
        a2a_config.orchestrator_url = "http://127.0.0.1:" + std::to_string(mock_.port());
        a2a_config.orchestrator_port = mock_.port();
        a2a_config.max_retries = 1;
        a2a_config.retry_delay_ms = 20;
        a2a_config.request_timeout_seconds = 15;
        server_->setA2AConfig(a2a_config);

        if (!server_->initialize(rpc_config)) {
            GTEST_SKIP() << "RpcServer refused to initialize (environment unavailable)";
        }
        ASSERT_TRUE(server_->start());

        channel_ = grpc::CreateChannel(rpc_config.server_address,
                                       grpc::InsecureChannelCredentials());
        const auto connected_deadline =
            std::chrono::system_clock::now() + std::chrono::seconds(10);
        ASSERT_TRUE(channel_->WaitForConnected(connected_deadline))
            << "rpc server channel never connected";

        user_stub_ = agent_communication::auth::UserService::NewStub(channel_);
        query_stub_ = agent_communication::AIQueryService::NewStub(channel_);
    }

    User registerUser(const std::string& label) {
        const std::string username = "dqp-" + label + "-" + uniqueSuffix();
        agent_communication::auth::RegisterRequest register_request;
        register_request.set_username(username);
        register_request.set_password("durable-pipeline-password");
        register_request.set_display_name(username);
        agent_communication::auth::RegisterResponse register_response;
        grpc::ClientContext register_context;
        const auto register_status =
            user_stub_->Register(&register_context, register_request, &register_response);
        EXPECT_TRUE(register_status.ok()) << register_status.error_message();

        agent_communication::auth::LoginRequest login_request;
        login_request.set_username(username);
        login_request.set_password("durable-pipeline-password");
        agent_communication::auth::LoginResponse login_response;
        grpc::ClientContext login_context;
        const auto login_status =
            user_stub_->Login(&login_context, login_request, &login_response);
        EXPECT_TRUE(login_status.ok()) << login_status.error_message();
        EXPECT_FALSE(login_response.token().empty());
        return {login_response.user_id(), login_response.token()};
    }

    void applyAuth(grpc::ClientContext& context, const User& user) {
        context.AddMetadata("authorization", "Bearer " + user.token);
    }

    std::int64_t countRows(const std::string& table, const std::string& column,
                           const std::string& value) {
        std::int64_t count = 0;
        handles_->store->executeTransaction([&](pqxx::work& transaction) {
            const auto result = transaction.exec_params(
                "SELECT COUNT(*) FROM " + table + " WHERE " + column + " = $1", value);
            if (!result.empty()) {
                count = result[0][0].as<std::int64_t>();
            }
        });
        return count;
    }

    std::string queryLogStatus(const std::string& owner_id, const std::string& request_id) {
        const auto log = handles_->domain->getQueryLogById(owner_id, request_id);
        return log ? log->status : std::string{"<missing>"};
    }

    static agent_communication::AIQueryRequest makeRequest(const std::string& request_id,
                                                           const std::string& context_id) {
        agent_communication::AIQueryRequest request;
        request.set_request_id(request_id);
        request.set_context_id(context_id);
        request.set_question("durable pipeline question");
        return request;
    }

    MockA2AHttpServer mock_;
    std::unique_ptr<Handles> handles_;
    std::unique_ptr<server_ns::RpcServer> server_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<agent_communication::auth::UserService::Stub> user_stub_;
    std::unique_ptr<agent_communication::AIQueryService::Stub> query_stub_;
};

// Requirement F.3 (unauthenticated half): no rows without an authenticated owner.
TEST_F(DurableQueryPipelineTest, UnauthenticatedQueryCreatesNoRows) {
    startPipeline("ok");
    const std::string request_id = "dqp-unauth-" + uniqueSuffix();
    auto request = makeRequest(request_id, "dqp-ctx-unauth-" + uniqueSuffix());

    agent_communication::AIQueryResponse response;
    grpc::ClientContext context;  // no authorization metadata
    const auto status = query_stub_->Query(&context, request, &response);

    EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(countRows("query_logs", "id", request_id), 0);
    EXPECT_EQ(countRows("budget_reservations", "request_id", request_id), 0);
    EXPECT_EQ(countRows("conversations", "id", request.context_id()), 0);
}

// P22 (批次十一阶段②): Logout closes the revocation loop — PG revoke +
// cache DEL + 60s deny marker. After logout the same token is refused by
// ValidateToken (deny hit) and by protected RPCs (interceptor refuses the
// revoked session), so an in-flight cache entry can never resurrect it.
TEST_F(DurableQueryPipelineTest, LogoutRevokesSessionAndDenyMarkerBlocksRefill) {
    startPipeline("ok");
    const auto user = registerUser("logout");

    // Warm the session cache through the shared ValidateToken path.
    {
        agent_communication::auth::ValidateTokenRequest vt;
        vt.set_token(user.token);
        agent_communication::auth::ValidateTokenResponse vr;
        grpc::ClientContext context;
        ASSERT_TRUE(user_stub_->ValidateToken(&context, vt, &vr).ok());
        ASSERT_TRUE(vr.valid());
    }

    // Manual logout: bearer token rides the authorization metadata.
    agent_communication::auth::LogoutRequest lr;
    agent_communication::auth::LogoutResponse lr_response;
    grpc::ClientContext logout_context;
    applyAuth(logout_context, user);
    const auto logout_status =
        user_stub_->Logout(&logout_context, lr, &lr_response);
    ASSERT_TRUE(logout_status.ok()) << logout_status.error_message();

    // The same token is refused everywhere afterwards.
    {
        agent_communication::auth::ValidateTokenRequest vt;
        vt.set_token(user.token);
        agent_communication::auth::ValidateTokenResponse vr;
        grpc::ClientContext context;
        ASSERT_TRUE(user_stub_->ValidateToken(&context, vt, &vr).ok());
        EXPECT_FALSE(vr.valid()) << "revoked token must not validate";
    }
    {
        const std::string request_id = "dqp-logout-" + uniqueSuffix();
        auto request = makeRequest(request_id,
                                   "dqp-ctx-logout-" + uniqueSuffix());
        agent_communication::AIQueryResponse response;
        grpc::ClientContext context;
        applyAuth(context, user);
        const auto query_status = query_stub_->Query(&context, request, &response);
        EXPECT_EQ(query_status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
    }
}

// Logout is a protected endpoint: without a valid bearer token the handler
// refuses (UNAUTHENTICATED), it never answers OK for an anonymous caller.
TEST_F(DurableQueryPipelineTest, UnauthenticatedLogoutIsRefused) {
    startPipeline("ok");
    agent_communication::auth::LogoutRequest lr;
    agent_communication::auth::LogoutResponse lr_response;
    grpc::ClientContext context;  // no authorization metadata
    const auto status = user_stub_->Logout(&context, lr, &lr_response);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);
}

// P25 (批次十一): context_id entries carrying sanitize-lossy characters
// (colons would fold into "_" and collide with legal keys) are refused
// before any conversation row is touched.
TEST_F(DurableQueryPipelineTest, UnsafeContextIdIsRefusedWithInvalidArgument) {
    startPipeline("ok");
    const auto user = registerUser("ctxguard");
    const std::string evil = "evil:ctx:with:colons";
    const std::string request_id = "dqp-ctxguard-" + uniqueSuffix();
    auto request = makeRequest(request_id, evil);

    agent_communication::AIQueryResponse response;
    grpc::ClientContext context;
    applyAuth(context, user);
    const auto status = query_stub_->Query(&context, request, &response);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(countRows("conversations", "id", evil), 0);
    EXPECT_EQ(countRows("query_logs", "id", request_id), 0);
}

// Requirement F.1: the request body cannot spoof the owner.
TEST_F(DurableQueryPipelineTest, SpoofedBodyUserIdIsIgnoredAndOwnerIsAuthenticated) {
    startPipeline("ok");
    const auto user = registerUser("spoof");
    const std::string request_id = "dqp-spoof-" + uniqueSuffix();
    auto request = makeRequest(request_id, "dqp-ctx-spoof-" + uniqueSuffix());
    request.set_user_id("attacker-owner-" + uniqueSuffix());

    agent_communication::AIQueryResponse response;
    grpc::ClientContext context;
    applyAuth(context, user);
    const auto status = query_stub_->Query(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message();

    const auto log = handles_->domain->getQueryLogById(user.id, request_id);
    ASSERT_TRUE(log.has_value());
    EXPECT_EQ(log->owner_id, user.id);
    EXPECT_EQ(log->status, "completed");
    EXPECT_FALSE(
        handles_->domain->getQueryLogById("attacker-owner", request_id).has_value());

    // Messages were persisted under the real owner with auto sequences.
    const auto messages = handles_->domain->listMessages(user.id, request.context_id());
    ASSERT_GE(messages.size(), 2u);
    EXPECT_EQ(messages.front().role, "user");
    EXPECT_EQ(messages.front().sequence_no, 1);
}

// Requirement F.2: same request_id retries keep one reservation/ledger.
TEST_F(DurableQueryPipelineTest, RetryWithSameRequestIdKeepsSingleReservationAndLedger) {
    startPipeline("ok");
    const auto user = registerUser("retry");
    const std::string request_id = "dqp-retry-" + uniqueSuffix();
    const std::string context_id = "dqp-ctx-retry-" + uniqueSuffix();

    for (int attempt = 0; attempt < 2; ++attempt) {
        auto request = makeRequest(request_id, context_id);
        agent_communication::AIQueryResponse response;
        grpc::ClientContext context;
        applyAuth(context, user);
        const auto status = query_stub_->Query(&context, request, &response);
        ASSERT_TRUE(status.ok()) << "attempt " << attempt << ": " << status.error_message();
    }

    EXPECT_EQ(countRows("budget_reservations", "request_id", request_id), 1);
    EXPECT_EQ(countRows("token_usage_ledger", "query_log_id", request_id), 1);
    EXPECT_EQ(countRows("query_logs", "id", request_id), 1);
    // Retries must not duplicate conversation history (1 user + 1 assistant).
    EXPECT_EQ(countRows("conversation_messages", "conversation_id", context_id), 2);
    // Ledger entries must be clearly estimates.
    const auto ledger = handles_->domain->listTokenUsageLedgerByOwner(user.id);
    bool found_estimated = false;
    for (const auto& entry : ledger) {
        if (entry.query_log_id == request_id) {
            EXPECT_TRUE(entry.estimated);
            found_estimated = true;
        }
    }
    EXPECT_TRUE(found_estimated);
}

// Requirement F.2: cross-owner reuse of a request_id is refused.
TEST_F(DurableQueryPipelineTest, CrossOwnerReuseOfRequestIdIsRefused) {
    startPipeline("ok");
    const auto victim = registerUser("victim");
    const auto intruder = registerUser("intruder");
    const std::string request_id = "dqp-cross-" + uniqueSuffix();
    const std::string context_id = "dqp-ctx-cross-" + uniqueSuffix();

    {
        auto request = makeRequest(request_id, context_id);
        agent_communication::AIQueryResponse response;
        grpc::ClientContext context;
        applyAuth(context, victim);
        ASSERT_TRUE(query_stub_->Query(&context, request, &response).ok());
    }
    {
        auto request = makeRequest(request_id, context_id);
        agent_communication::AIQueryResponse response;
        grpc::ClientContext context;
        applyAuth(context, intruder);
        const auto status = query_stub_->Query(&context, request, &response);
        EXPECT_FALSE(status.ok());
    }

    // Still exactly one reservation, owned by the victim.
    EXPECT_EQ(countRows("budget_reservations", "request_id", request_id), 1);
    EXPECT_FALSE(handles_->domain->getQueryLogById(intruder.id, request_id).has_value());
    EXPECT_EQ(queryLogStatus(victim.id, request_id), "completed");
}

// Requirement F.3: budget rejection persists "rejected" before responding.
TEST_F(DurableQueryPipelineTest, BudgetRejectionPersistsRejectedTerminalState) {
    startPipeline("ok");
    const auto user = registerUser("budget");
    common_ns::BudgetLimits tiny_limits;
    tiny_limits.global = 0;
    tiny_limits.user_daily = 10;  // below the minimum stable estimate
    tiny_limits.user_monthly = 0;
    tiny_limits.session = 0;
    ASSERT_TRUE(handles_->budget->setOwnerPolicy(user.id, tiny_limits));

    const std::string request_id = "dqp-budget-" + uniqueSuffix();
    auto request = makeRequest(request_id, "dqp-ctx-budget-" + uniqueSuffix());

    agent_communication::AIQueryResponse response;
    grpc::ClientContext context;
    applyAuth(context, user);
    const auto status = query_stub_->Query(&context, request, &response);

    EXPECT_EQ(status.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);
    EXPECT_EQ(queryLogStatus(user.id, request_id), "rejected");
    const auto trace = handles_->domain->getTraceById(user.id, "trace-" + request_id);
    ASSERT_TRUE(trace.has_value());
    EXPECT_EQ(trace->status, "rejected");
    // No reservation row and no ledger entry for a rejected request.
    EXPECT_EQ(countRows("budget_reservations", "request_id", request_id), 0);
    EXPECT_EQ(countRows("token_usage_ledger", "query_log_id", request_id), 0);
}

// N3 regression: a request that was budget-rejected first must still record
// its conversation history and exactly one ledger entry when the SAME
// request_id is retried after the owner's quota is relaxed. The old
// first_attempt guard skipped the message/ledger writes on the successful
// retry and left a "completed" request with zero durable records.
TEST_F(DurableQueryPipelineTest, RetryAfterBudgetRejectionRecordsMessagesAndLedgerOnce) {
    startPipeline("ok");
    const auto user = registerUser("rejretry");
    common_ns::BudgetLimits tiny_limits;
    tiny_limits.global = 0;
    tiny_limits.user_daily = 10;  // below the minimum stable estimate
    tiny_limits.user_monthly = 0;
    tiny_limits.session = 0;
    ASSERT_TRUE(handles_->budget->setOwnerPolicy(user.id, tiny_limits));

    const std::string request_id = "dqp-rejretry-" + uniqueSuffix();
    const std::string context_id = "dqp-ctx-rejretry-" + uniqueSuffix();
    auto request = makeRequest(request_id, context_id);

    // Attempt 1: rejected before any history or ledger entry exists.
    {
        agent_communication::AIQueryResponse response;
        grpc::ClientContext context;
        applyAuth(context, user);
        const auto status = query_stub_->Query(&context, request, &response);
        ASSERT_EQ(status.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);
    }
    EXPECT_EQ(queryLogStatus(user.id, request_id), "rejected");
    EXPECT_EQ(countRows("conversation_messages", "conversation_id", context_id), 0);
    EXPECT_EQ(countRows("token_usage_ledger", "query_log_id", request_id), 0);

    // Relax the owner's quota, then retry with the same request_id.
    common_ns::BudgetLimits relaxed_limits;
    relaxed_limits.global = 0;
    relaxed_limits.user_daily = 1000000;
    relaxed_limits.user_monthly = 0;
    relaxed_limits.session = 0;
    ASSERT_TRUE(handles_->budget->setOwnerPolicy(user.id, relaxed_limits));

    {
        agent_communication::AIQueryResponse response;
        grpc::ClientContext context;
        applyAuth(context, user);
        const auto status = query_stub_->Query(&context, request, &response);
        ASSERT_TRUE(status.ok()) << status.error_message();
    }

    EXPECT_EQ(queryLogStatus(user.id, request_id), "completed");
    // The successful retry persists the history (1 user + 1 assistant) and
    // exactly one estimated ledger entry.
    EXPECT_EQ(countRows("conversation_messages", "conversation_id", context_id), 2);
    EXPECT_EQ(countRows("token_usage_ledger", "query_log_id", request_id), 1);
    EXPECT_EQ(countRows("budget_reservations", "request_id", request_id), 1);

    // A further retry still never duplicates anything.
    {
        agent_communication::AIQueryResponse response;
        grpc::ClientContext context;
        applyAuth(context, user);
        const auto status = query_stub_->Query(&context, request, &response);
        ASSERT_TRUE(status.ok()) << status.error_message();
    }
    EXPECT_EQ(countRows("conversation_messages", "conversation_id", context_id), 2);
    EXPECT_EQ(countRows("token_usage_ledger", "query_log_id", request_id), 1);
    EXPECT_EQ(countRows("budget_reservations", "request_id", request_id), 1);
}

// Requirement F.3: agent exception persists "failed".
TEST_F(DurableQueryPipelineTest, AgentFailurePersistsFailedTerminalState) {
    startPipeline("http500");
    const auto user = registerUser("agentfail");
    const std::string request_id = "dqp-agentfail-" + uniqueSuffix();
    auto request = makeRequest(request_id, "dqp-ctx-agentfail-" + uniqueSuffix());

    agent_communication::AIQueryResponse response;
    grpc::ClientContext context;
    applyAuth(context, user);
    const auto status = query_stub_->Query(&context, request, &response);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(queryLogStatus(user.id, request_id), "failed");
}

// Requirement F.3: gRPC cancellation (the SSE-close equivalent at the gRPC
// layer; the proxy forwards browser close as stream.cancel()) persists
// "cancelled".
TEST_F(DurableQueryPipelineTest, GrpcCancelPersistsCancelledTerminalState) {
    startPipeline("stream-slow");
    const auto user = registerUser("cancel");
    const std::string request_id = "dqp-cancel-" + uniqueSuffix();
    auto request = makeRequest(request_id, "dqp-ctx-cancel-" + uniqueSuffix());

    grpc::ClientContext context;
    applyAuth(context, user);
    auto reader = query_stub_->QueryStream(&context, request);

    agent_communication::AIStreamEvent event;
    bool received_any = false;
    if (reader->Read(&event)) {
        received_any = true;
    }
    EXPECT_TRUE(received_any) << "expected at least one stream event before cancelling";
    context.TryCancel();

    agent_communication::AIStreamEvent drained;
    while (reader->Read(&drained)) {
    }
    const auto finish_status = reader->Finish();
    EXPECT_EQ(finish_status.error_code(), grpc::StatusCode::CANCELLED);

    // The server persists the cancelled terminal once the relay unwinds.
    std::string observed_status;
    for (int attempt = 0; attempt < 50; ++attempt) {
        observed_status = queryLogStatus(user.id, request_id);
        if (observed_status == "cancelled") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    EXPECT_EQ(observed_status, "cancelled");
    const auto trace = handles_->domain->getTraceById(user.id, "trace-" + request_id);
    ASSERT_TRUE(trace.has_value());
    EXPECT_EQ(trace->status, "cancelled");
}

// Requirement F.4: the stream carries exactly one terminal complete event.
TEST_F(DurableQueryPipelineTest, QueryStreamEmitsExactlyOneCompleteEvent) {
    startPipeline("ok");
    const auto user = registerUser("once");
    const std::string request_id = "dqp-once-" + uniqueSuffix();
    auto request = makeRequest(request_id, "dqp-ctx-once-" + uniqueSuffix());

    grpc::ClientContext context;
    applyAuth(context, user);
    auto reader = query_stub_->QueryStream(&context, request);

    int complete_events = 0;
    int error_events = 0;
    std::string streamed_content;
    agent_communication::AIStreamEvent event;
    while (reader->Read(&event)) {
        if (event.event_type() == "complete") {
            ++complete_events;
        } else if (event.event_type() == "error") {
            ++error_events;
        } else if (event.event_type() == "partial") {
            streamed_content += event.content();
        }
    }
    const auto status = reader->Finish();
    ASSERT_TRUE(status.ok()) << status.error_message();

    EXPECT_EQ(complete_events, 1);
    EXPECT_EQ(error_events, 0);
    EXPECT_NE(streamed_content.find("mock-stream-answer"), std::string::npos);
    EXPECT_EQ(queryLogStatus(user.id, request_id), "completed");
}

// Requirement F.5: losing every cache entry owned by this pipeline leaves
// PostgreSQL data readable. Keys are wiped by test prefix (all ids carry the
// "dqp-" marker) instead of FLUSHALL, so the shared Redis instance used by
// other tests and services is left untouched.
TEST_F(DurableQueryPipelineTest, RedisFlushLeavesPostgresDataReadable) {
    startPipeline("ok");
    const auto user = registerUser("flush");
    // One suffix per run: every id and cache key created by this test shares
    // it, so the cleanup SCAN below only ever touches this test's own keys.
    const std::string run_suffix = uniqueSuffix();
    const std::string request_id = "dqp-flush-" + run_suffix;
    auto request = makeRequest(request_id, "dqp-ctx-flush-" + run_suffix);

    {
        agent_communication::AIQueryResponse response;
        grpc::ClientContext context;
        applyAuth(context, user);
        ASSERT_TRUE(query_stub_->Query(&context, request, &response).ok());
    }
    ASSERT_EQ(queryLogStatus(user.id, request_id), "completed");

    // Seed one pipeline-owned cache entry so the wipe below has a verifiable
    // target (trace span keys use random UUIDs and are left alone).
    const int redis_port =
        std::getenv("REDIS_PORT") ? std::atoi(std::getenv("REDIS_PORT")) : 6379;
    const std::string redis_host =
        std::getenv("REDIS_HOST") ? std::getenv("REDIS_HOST") : "127.0.0.1";
    redisContext* redis = redisConnect(redis_host.c_str(), redis_port);
    ASSERT_NE(redis, nullptr) << "redis connection required for this test";
    if (redis->err == 0) {
        auto* seed = static_cast<redisReply*>(
            redisCommand(redis, "SET cache:%s 1", ("dqp-flush-" + run_suffix).c_str()));
        ASSERT_NE(seed, nullptr);
        freeReplyObject(seed);
    }

    // Wipe only the cache entries created by THIS pipeline run (the pattern
    // embeds run_suffix, never FLUSHALL and never foreign test keys).
    if (redis->err == 0) {
        std::string cursor = "0";
        std::size_t deleted = 0;
        const std::string pattern = "*dqp-*" + run_suffix + "*";
        do {
            auto* reply = static_cast<redisReply*>(
                redisCommand(redis, "SCAN %s MATCH %s COUNT 1000", cursor.c_str(),
                             pattern.c_str()));
            ASSERT_NE(reply, nullptr);
            if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 2 &&
                reply->element[0]->str != nullptr) {
                cursor = reply->element[0]->str;
                redisReply* keys = reply->element[1];
                for (std::size_t index = 0; index < keys->elements; ++index) {
                    auto* del = static_cast<redisReply*>(
                        redisCommand(redis, "DEL %s", keys->element[index]->str));
                    if (del != nullptr) {
                        deleted += del->integer;
                        freeReplyObject(del);
                    }
                }
            }
            freeReplyObject(reply);
        } while (cursor != "0");
        EXPECT_GT(deleted, 0u) << "expected pipeline-owned cache keys to wipe";
    }
    redisFree(redis);

    // Durable rows survive the cache wipe...
    const auto log = handles_->domain->getQueryLogById(user.id, request_id);
    ASSERT_TRUE(log.has_value());
    EXPECT_EQ(log->status, "completed");
    EXPECT_FALSE(handles_->domain->listMessages(user.id, request.context_id()).empty());

    // ...and the pipeline keeps serving afterwards.
    const std::string second_request_id = "dqp-flush2-" + run_suffix;
    auto second_request = makeRequest(second_request_id, request.context_id());
    agent_communication::AIQueryResponse second_response;
    grpc::ClientContext second_context;
    applyAuth(second_context, user);
    const auto status = query_stub_->Query(&second_context, second_request, &second_response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(queryLogStatus(user.id, second_request_id), "completed");
}

// ── P10: single-intent fast path ──────────────────────────────────────────

#ifndef _WIN32
class FastPathEnvGuard {
public:
    explicit FastPathEnvGuard(const char* name, const char* value) : name_(name) {
        const char* old = ::getenv(name);
        had_old_ = (old != nullptr);
        if (had_old_) old_value_ = old;
        if (value) {
            ::setenv(name, value, 1);
        } else {
            ::unsetenv(name);
        }
    }
    ~FastPathEnvGuard() {
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

// P26 T3: HMAC-SHA256 (hex) — mirrors the auth interceptor's verification so
// the trust-mode tests sign headers exactly like the gateway does.
std::string testHmacSha256Hex(const std::string& secret,
                              const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    HMAC(EVP_sha256(), secret.data(), static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         digest, &digest_length);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < digest_length; ++index) {
        output << std::setw(2) << static_cast<int>(digest[index]);
    }
    return output.str();
}

// P26 T3 (批次十一阶段③): trusted-proxy dual mode — HMAC-signed
// x-nexusai-* headers authenticate the call and Logout revokes through the
// injected token hash (the Bearer was consumed by the gateway).
TEST_F(DurableQueryPipelineTest, TrustProxySignedHeadersAuthenticateAndRevoke) {
    FastPathEnvGuard trust("NEXUSAI_TRUST_PROXY", "1");
    FastPathEnvGuard secret("NEXUSAI_PROXY_HMAC_SECRET",
                            "durable-test-proxy-secret-0123456789abcdef");
    startPipeline("ok");
    const auto user = registerUser("trustlogout");

    const std::string token_hash =
        server_ns::AuthServiceImpl::hashToken(user.token);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string exp = std::to_string(now + 60);
    const std::string canonical = user.id + "|dqp-trust-logout|USER|" + exp +
                                  "|" + token_hash;
    const std::string signature = testHmacSha256Hex(
        "durable-test-proxy-secret-0123456789abcdef", canonical);

    grpc::ClientContext context;
    context.AddMetadata("x-nexusai-user-id", user.id);
    context.AddMetadata("x-nexusai-username", "dqp-trust-logout");
    context.AddMetadata("x-nexusai-role", "USER");
    context.AddMetadata("x-nexusai-exp", exp);
    context.AddMetadata("x-nexusai-token-hash", token_hash);
    context.AddMetadata("x-nexusai-signature", signature);

    agent_communication::auth::LogoutRequest request;
    agent_communication::auth::LogoutResponse response;
    const auto status = user_stub_->Logout(&context, request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message();

    // The session was revoked through the injected hash (Bearer never sent).
    agent_communication::auth::ValidateTokenRequest vt;
    vt.set_token(user.token);
    agent_communication::auth::ValidateTokenResponse vr;
    grpc::ClientContext validate_context;
    ASSERT_TRUE(user_stub_->ValidateToken(&validate_context, vt, &vr).ok());
    EXPECT_FALSE(vr.valid()) << "revoked via injected token hash";
}

// A forged signature next to a REAL bearer token must be refused WITHOUT
// falling back to Bearer validation (otherwise smuggling a fake header next
// to a stolen token would escalate or slip through the trust boundary).
TEST_F(DurableQueryPipelineTest, TrustProxyForgedSignatureNeverFallsBackToBearer) {
    FastPathEnvGuard trust("NEXUSAI_TRUST_PROXY", "1");
    FastPathEnvGuard secret("NEXUSAI_PROXY_HMAC_SECRET",
                            "durable-test-proxy-secret-0123456789abcdef");
    startPipeline("ok");
    const auto user = registerUser("trustforge");

    const std::string token_hash =
        server_ns::AuthServiceImpl::hashToken(user.token);
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    grpc::ClientContext context;
    context.AddMetadata("authorization", "Bearer " + user.token);
    context.AddMetadata("x-nexusai-user-id", user.id);
    context.AddMetadata("x-nexusai-username", "dqp-trust-forge");
    context.AddMetadata("x-nexusai-role", "ADMIN");  // escalation attempt
    context.AddMetadata("x-nexusai-exp", std::to_string(now + 60));
    context.AddMetadata("x-nexusai-token-hash", token_hash);
    context.AddMetadata("x-nexusai-signature", std::string(64, '0'));

    agent_communication::auth::LogoutRequest request;
    agent_communication::auth::LogoutResponse response;
    const auto status = user_stub_->Logout(&context, request, &response);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAUTHENTICATED);

    // The session is untouched: the Bearer was NOT used as a fallback.
    agent_communication::auth::ValidateTokenRequest vt;
    vt.set_token(user.token);
    agent_communication::auth::ValidateTokenResponse vr;
    grpc::ClientContext validate_context;
    ASSERT_TRUE(user_stub_->ValidateToken(&validate_context, vt, &vr).ok());
    EXPECT_TRUE(vr.valid()) << "forged headers must not degrade to Bearer";
}

TEST(SingleIntentFastPathTest, MultiIntentDetectorVetoesParallelSignals) {
    using server_ns::MultiAgentHandler;

    // Parallel/sequence signals must veto the fast path (strict-by-design).
    EXPECT_TRUE(MultiAgentHandler::hasMultiIntentSignals(
        "帮我翻译这段话，然后再总结一下"));
    EXPECT_TRUE(MultiAgentHandler::hasMultiIntentSignals("先部署服务，再运行测试"));
    EXPECT_TRUE(MultiAgentHandler::hasMultiIntentSignals(
        "请完成以下任务 1. 写代码 2. 写文档"));
    // No-space Chinese numbered list: the second marker's previous byte is
    // a CJK continuation byte, so the boundary check must not require
    // ASCII whitespace (regression for the relaxed-boundary fix).
    EXPECT_TRUE(MultiAgentHandler::hasMultiIntentSignals(
        "1.写代码，2.写文档"));
    EXPECT_TRUE(MultiAgentHandler::hasMultiIntentSignals(
        "1、翻译这段 2、润色一下"));
    EXPECT_TRUE(MultiAgentHandler::hasMultiIntentSignals(
        "Translate this paragraph and then summarize it"));
    EXPECT_TRUE(MultiAgentHandler::hasMultiIntentSignals(
        "First deploy the service, then run the tests"));
    EXPECT_TRUE(MultiAgentHandler::hasMultiIntentSignals(
        "解释量子计算；顺便翻译摘要"));

    // Plain single-intent queries carry no veto signal.
    EXPECT_FALSE(MultiAgentHandler::hasMultiIntentSignals(
        "帮我翻译这段话成英文"));
    EXPECT_FALSE(MultiAgentHandler::hasMultiIntentSignals(
        "What is the capital of France?"));
    EXPECT_FALSE(MultiAgentHandler::hasMultiIntentSignals(
        "解释一下量子计算的基本原理"));
}

TEST(SingleIntentFastPathTest, HighConfidenceSingleIntentSkipsPlanning) {
    FastPathEnvGuard guard("NEXUSAI_SINGLE_INTENT_FAST_PATH", "1");

    // All component pointers null: the fast path must build the plan
    // WITHOUT touching the planner (calling plan() would crash here).
    server_ns::MultiAgentHandler handler(nullptr, nullptr, nullptr,
                                         nullptr, nullptr, nullptr);
    handler.setFastPathSkillResolver(
        [](const std::string&) {
            return agent_rpc::orchestrator::AgentRouter::HighConfidenceSkill{
                "translation", 0.93};
        });

    agent_rpc::orchestrator::ExecutionPlan plan;
    ASSERT_TRUE(handler.tryBuildFastPathPlan("帮我翻译这段话成英文", plan));
    EXPECT_TRUE(plan.is_single_agent);
    EXPECT_EQ(plan.single_agent_skill, "translation");
    EXPECT_EQ(plan.original_query, "帮我翻译这段话成英文");
    EXPECT_TRUE(plan.tasks.empty());
}

TEST(SingleIntentFastPathTest, LowConfidenceOrMultiIntentGoesToPlanning) {
    FastPathEnvGuard guard("NEXUSAI_SINGLE_INTENT_FAST_PATH", "1");

    server_ns::MultiAgentHandler handler(nullptr, nullptr, nullptr,
                                         nullptr, nullptr, nullptr);

    // Low confidence / no hit → fast path declines, planning must run.
    handler.setFastPathSkillResolver([](const std::string&) {
        return std::nullopt;
    });
    agent_rpc::orchestrator::ExecutionPlan plan;
    EXPECT_FALSE(handler.tryBuildFastPathPlan("帮我翻译这段话成英文", plan));

    // High confidence but multi-intent signals → conservative veto.
    handler.setFastPathSkillResolver([](const std::string&) {
        return agent_rpc::orchestrator::AgentRouter::HighConfidenceSkill{
            "translation", 0.95};
    });
    EXPECT_FALSE(handler.tryBuildFastPathPlan(
        "帮我翻译这段话，然后再总结一下", plan));

    // Switch off (default state) → fast path never applies, even with a hit.
    FastPathEnvGuard off("NEXUSAI_SINGLE_INTENT_FAST_PATH", nullptr);
    EXPECT_FALSE(handler.tryBuildFastPathPlan("帮我翻译这段话成英文", plan));
}

// P26 T2 (批次十一阶段①): AuthCache logical-invalidation semantics against
// real PostgreSQL + real Redis. AuthServiceImpl methods are driven directly
// (no gRPC server / interceptor), so the ValidateToken RPC exercises the same
// AuthCache::validateTokenCached path the interceptor uses on every RPC.
class AuthCacheInvalidationTest : public ::testing::Test {
protected:
    static constexpr int kSessionTtlHours = 24;

    std::unique_ptr<common_ns::PostgresStore> store_;
    std::unique_ptr<server_ns::AuthServiceImpl> auth_;
    common_ns::RedisClient redis_;
    bool redis_available_ = false;
    common_ns::RedisClient* previous_redis_ = nullptr;
    std::string user_id_;
    std::string token_;

    void SetUp() override {
        previous_redis_ = server_ns::AuthCache::redis();

        try {
            auto config = common_ns::PostgresConfig::fromEnvironment();
            config.pool_size = 2;
            store_ = std::make_unique<common_ns::PostgresStore>(std::move(config));
            if (!store_->healthCheck()) {
                store_.reset();
            }
        } catch (const std::exception&) {
            store_.reset();
        }
        if (!store_) {
            GTEST_SKIP() << "PostgreSQL test DSN is unavailable";
        }
        const std::string migration =
            readFileOrEmpty(rootPath() + "/db/migrations/V010__local_auth.sql");
        if (migration.empty()) {
            GTEST_SKIP() << "V010__local_auth.sql unreadable";
        }
        try {
            store_->executeTransaction([&migration](pqxx::work& transaction) {
                transaction.exec(migration);
            });
        } catch (const std::exception&) {
            GTEST_SKIP() << "V010 migration failed";
        }
        auth_ = std::make_unique<server_ns::AuthServiceImpl>(*store_);

        redis_available_ = redis_.connect();
        if (redis_available_) {
            server_ns::AuthCache::setRedisClient(&redis_);
        }

        registerAndLogin();
    }

    void TearDown() override {
        server_ns::AuthCache::setRedisClient(previous_redis_);
    }

    void registerAndLogin() {
        const std::string username = "dqp-authcache-" + uniqueSuffix();
        agent_communication::auth::RegisterRequest register_request;
        register_request.set_username(username);
        register_request.set_password("authcache-password");
        agent_communication::auth::RegisterResponse register_response;
        grpc::ServerContext register_context;
        ASSERT_TRUE(auth_->Register(&register_context, &register_request,
                                    &register_response)
                        .ok());
        user_id_ = register_response.user_id();
        ASSERT_FALSE(user_id_.empty());

        agent_communication::auth::LoginRequest login_request;
        login_request.set_username(username);
        login_request.set_password("authcache-password");
        agent_communication::auth::LoginResponse login_response;
        grpc::ServerContext login_context;
        ASSERT_TRUE(auth_->Login(&login_context, &login_request, &login_response)
                        .ok());
        token_ = login_response.token();
        ASSERT_FALSE(token_.empty());
    }

    bool callValidateToken(const std::string& token) {
        agent_communication::auth::ValidateTokenRequest request;
        request.set_token(token);
        agent_communication::auth::ValidateTokenResponse response;
        grpc::ServerContext context;
        const auto status = auth_->ValidateToken(&context, &request, &response);
        EXPECT_TRUE(status.ok()) << status.error_message();
        return response.valid();
    }

    std::string tokenHash() const {
        return server_ns::AuthServiceImpl::hashToken(token_);
    }

    // Session-cache payload ("" when the key is absent).
    std::string cachedPayload() {
        std::string payload;
        if (!redis_.get(server_ns::AuthCache::sessionKey(tokenHash()), payload)) {
            return {};
        }
        return payload;
    }

    void revokeSessionInPostgres() {
        // token_hash is hex — safe to interpolate in a test UPDATE.
        store_->executeTransaction([&](pqxx::work& transaction) {
            transaction.exec(
                "UPDATE auth_sessions SET revoked_at = NOW(), updated_at = NOW() "
                "WHERE token_hash = '" + tokenHash() + "'");
        });
    }
};

TEST_F(AuthCacheInvalidationTest, RefillTtlEqualsTrueRemainingLifetime) {
    if (!redis_available_) {
        GTEST_SKIP() << "Redis not available";
    }
    ASSERT_TRUE(callValidateToken(token_));  // miss → PG → refill

    const std::string payload_text = cachedPayload();
    ASSERT_FALSE(payload_text.empty()) << "session cache must be refilled";
    const auto payload = nlohmann::json::parse(payload_text);
    EXPECT_EQ(payload.value("user_id", ""), user_id_);
    EXPECT_EQ(payload.value<std::int64_t>("epoch", -1), 0);  // no INCR ever fired

    const std::int64_t payload_expires =
        payload.value<std::int64_t>("expires_at", 0);
    const auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::int64_t full_ttl = kSessionTtlHours * 3600;
    EXPECT_GT(payload_expires, now_epoch + full_ttl - 10);
    EXPECT_LT(payload_expires, now_epoch + full_ttl + 10);

    // The key TTL is the session's true remaining lifetime (±2s between
    // refill and this read) — never the old fixed 300s approximation.
    std::int64_t key_ttl = 0;
    ASSERT_TRUE(redis_.ttl(server_ns::AuthCache::sessionKey(tokenHash()), key_ttl));
    EXPECT_GT(key_ttl, full_ttl - 12);
    EXPECT_LT(key_ttl, full_ttl + 2);
}

TEST_F(AuthCacheInvalidationTest, DenyMarkerOverridesExistingCacheEntry) {
    if (!redis_available_) {
        GTEST_SKIP() << "Redis not available";
    }
    ASSERT_TRUE(callValidateToken(token_));
    ASSERT_FALSE(cachedPayload().empty());

    // Session-level revocation bus fires (Logout will be the producer;
    // this test arms the marker directly): deny must refuse even while the
    // stale cache entry is still present.
    EXPECT_TRUE(redis_.setex(server_ns::AuthCache::denyKey(tokenHash()), 60, "1"));
    EXPECT_FALSE(callValidateToken(token_)) << "deny hit must refuse";

    // The entry was not deleted — the deny marker (not deletion) refuses.
    ASSERT_FALSE(cachedPayload().empty());

    // Marker expires → the session is served again (PG never revoked it).
    EXPECT_TRUE(redis_.del(server_ns::AuthCache::denyKey(tokenHash())));
    EXPECT_TRUE(callValidateToken(token_));
}

TEST_F(AuthCacheInvalidationTest, EpochIncrementInvalidatesStaleEntryAndRefills) {
    if (!redis_available_) {
        GTEST_SKIP() << "Redis not available";
    }
    ASSERT_TRUE(callValidateToken(token_));
    ASSERT_EQ(nlohmann::json::parse(cachedPayload())
                  .value<std::int64_t>("epoch", -1),
              0);

    // User-level revocation bus fires (ban/demote/change-password path — no
    // production RPC yet; simulated like a DBA maintenance step).
    std::int64_t incremented = 0;
    ASSERT_TRUE(redis_.incrby(server_ns::AuthCache::epochKey(user_id_), 1,
                              incremented));
    ASSERT_EQ(incremented, 1);

    // Stale entry (epoch 0) no longer matches → PG re-validation, which
    // still accepts the session → refill with the new epoch.
    EXPECT_TRUE(callValidateToken(token_));
    EXPECT_EQ(nlohmann::json::parse(cachedPayload())
                  .value<std::int64_t>("epoch", -1),
              1);
}

TEST_F(AuthCacheInvalidationTest, RevokedSessionNeverResurrectsAfterRevokePlusEpoch) {
    if (!redis_available_) {
        GTEST_SKIP() << "Redis not available";
    }
    ASSERT_TRUE(callValidateToken(token_));
    ASSERT_FALSE(cachedPayload().empty());

    // DBA-style revocation: PG revoke + INCR epoch (documented operator
    // constraint). The stale entry is refused by the epoch mismatch and the
    // re-validation hits PG's revoked_at — invalid forever (no negative
    // cache: every call re-asks PG, and PG is the final judge).
    revokeSessionInPostgres();
    std::int64_t incremented = 0;
    ASSERT_TRUE(redis_.incrby(server_ns::AuthCache::epochKey(user_id_), 1,
                              incremented));
    ASSERT_EQ(incremented, 1);

    EXPECT_FALSE(callValidateToken(token_));
    EXPECT_FALSE(callValidateToken(token_));
    ASSERT_FALSE(cachedPayload().empty());  // stale entry stays, always refused
}

TEST_F(AuthCacheInvalidationTest, RedisUnavailableDegradesToPostgres) {
    // Simulate Redis down: every call is the authoritative PG JOIN check
    // (fail-safe, only the allowed false-negative/refusal direction).
    server_ns::AuthCache::setRedisClient(nullptr);
    EXPECT_TRUE(callValidateToken(token_));

    revokeSessionInPostgres();
    EXPECT_FALSE(callValidateToken(token_));
    EXPECT_FALSE(callValidateToken(token_));  // never resurrects after revoke

    if (redis_available_) {
        server_ns::AuthCache::setRedisClient(&redis_);
    }
}
#endif  // !_WIN32

}  // namespace
