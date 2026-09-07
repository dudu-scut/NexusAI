#include "agent_rpc/server/rpc_server.h"
#include "agent_rpc/server/agent_service.h"
#include "agent_rpc/server/ai_query_service.h"
#include "agent_rpc/server/auth_service.h"
#include "agent_rpc/server/auth_interceptor.h"
#include "agent_rpc/server/sharing_service.h"
#include "agent_rpc/server/agent_lifecycle_service.h"
#include "agent_rpc/server/user_experience_service.h"
#include "agent_rpc/server/observability_service.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/metrics.h"
#include "agent_rpc/common/message_converter.h"
#include "agent_rpc/common/serializer.h"
#include "agent_rpc/common/cost_tracker.h"
#include "agent_rpc/orchestrator/feedback_aggregator.h"
#include "agent_rpc/orchestrator/export_service.h"
#include "agent_rpc/orchestrator/replay_service.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <sstream>

namespace agent_rpc {
namespace server {

namespace {

// Factory that creates AuthInterceptor instances for each RPC
class AuthInterceptorFactory
    : public grpc::experimental::ServerInterceptorFactoryInterface {
public:
    explicit AuthInterceptorFactory(AuthServiceImpl* auth_service)
        : auth_service_(auth_service) {}

    grpc::experimental::Interceptor* CreateServerInterceptor(
        grpc::experimental::ServerRpcInfo* info) override {
        return new AuthInterceptor(auth_service_, info->method());
    }

private:
    AuthServiceImpl* auth_service_;
};

std::string stripScheme(const std::string& address, const std::string& scheme) {
    const auto prefix = scheme + "://";
    if (address.rfind(prefix, 0) == 0) {
        return address.substr(prefix.size());
    }
    return address;
}

common::ServiceEndpoint buildSelfEndpoint(const std::string& address) {
    common::ServiceEndpoint endpoint;
    const auto separator = address.rfind(':');
    endpoint.host = separator == std::string::npos ? address : address.substr(0, separator);
    endpoint.port = separator == std::string::npos ? 0 : std::stoi(address.substr(separator + 1));
    if (endpoint.host.empty() || endpoint.host == "0.0.0.0") {
        endpoint.host = "127.0.0.1";
    }
    endpoint.service_name = "rpc_server";
    endpoint.version = "1.0.0";
    endpoint.is_healthy = true;
    endpoint.last_heartbeat = std::chrono::steady_clock::now();
    return endpoint;
}

std::string buildRegisteredServiceId(const std::shared_ptr<registry::ServiceRegistry>& registry_service,
                                     const common::ServiceEndpoint& endpoint) {
    if (auto consul = std::dynamic_pointer_cast<registry::ConsulServiceRegistry>(registry_service)) {
        return consul->getServiceId(endpoint);
    }
    if (std::dynamic_pointer_cast<registry::EtcdServiceRegistry>(registry_service)) {
        return endpoint.service_name + "/" + endpoint.host + ":" + std::to_string(endpoint.port);
    }
    return endpoint.host + ":" + std::to_string(endpoint.port);
}

}  // namespace

RpcServer::RpcServer() {
    // MCP server path reserved for the upcoming MCP client
    mcp_server_path_ = "";
    mcp_server_args_ = {};
    sharing_service_impl_ = std::make_unique<SharingServiceImpl>();
    user_experience_service_impl_ = std::make_unique<UserExperienceServiceImpl>();
}

RpcServer::~RpcServer() {
    stop();

    // deep-review lc-R1: the gRPC Server must outlive every registered
    // service object. ~Server drains in-flight handlers (a synchronous
    // handler that outlived the 5s Shutdown deadline still holds pointers
    // into the service impls), so release server_ FIRST and only then the
    // service members. The previous order (services first, server_ last)
    // destroyed service impls while their handlers were still executing.
    server_.reset();

    // Explicitly release members to guarantee destructor order
    service_impl_.reset();
    health_service_impl_.reset();
    ai_query_service_impl_.reset();
    auth_service_impl_.reset();
    sharing_service_impl_.reset();
    agent_lifecycle_service_impl_.reset();
    user_experience_service_impl_.reset();
    observability_service_impl_.reset();
    runtime_repository_.reset();
    query_domain_repository_.reset();
    budget_repository_.reset();
    auth_repository_.reset();
    postgres_store_.reset();
    builders_.clear();
}

bool RpcServer::initialize(const common::RpcConfig& config) {
    config_ = config;
    address_ = config.server_address;

    auth_service_impl_.reset();
    auth_repository_.reset();
    runtime_repository_.reset();
    query_domain_repository_.reset();
    budget_repository_.reset();
    postgres_store_.reset();

    // Authentication and the durable query pipeline are PostgreSQL-backed and
    // fail closed when the database is unavailable.
    try {
        postgres_store_ = std::make_unique<common::PostgresStore>(
            common::PostgresConfig::fromEnvironment());
        if (!postgres_store_->healthCheck()) {
            throw common::PostgresUnavailable("PostgreSQL health check failed");
        }
        auth_repository_ = std::make_unique<common::AuthRepository>(*postgres_store_);
        query_domain_repository_ =
            std::make_unique<common::QueryDomainRepository>(*postgres_store_);
        budget_repository_ =
            std::make_unique<common::PostgresBudgetRepository>(*postgres_store_);
        runtime_repository_ =
            std::make_unique<common::AgentRuntimeRepository>(*postgres_store_);
    } catch (const common::PostgresUnavailable& error) {
        LOG_ERROR("Failed to initialize PostgreSQL auth store: " + std::string(error.what()));
        auth_repository_.reset();
        runtime_repository_.reset();
        query_domain_repository_.reset();
        budget_repository_.reset();
        postgres_store_.reset();
        return false;
    } catch (const std::exception& error) {
        LOG_ERROR("Failed to initialize PostgreSQL auth store: " + std::string(error.what()));
        auth_repository_.reset();
        runtime_repository_.reset();
        query_domain_repository_.reset();
        budget_repository_.reset();
        postgres_store_.reset();
        return false;
    }

    // Redis: connect to Redis server
    redis_client_ = std::make_unique<common::RedisClient>();
    std::string redis_host = std::getenv("REDIS_HOST") ? std::getenv("REDIS_HOST") : "127.0.0.1";
    int redis_port = std::getenv("REDIS_PORT") ? std::atoi(std::getenv("REDIS_PORT")) : 6379;
    if (!redis_client_->connect(redis_host, redis_port)) {
        LOG_ERROR("Failed to connect to Redis at " + redis_host + ":" + std::to_string(redis_port));
        return false;
    }

    // Initialize CostTracker with Redis for budget counters
    agent_rpc::common::CostTracker::instance().initialize(redis_client_.get());

    // Initialize FeedbackAggregator: Redis is only a metrics cache; the
    // durable facts live in PostgreSQL.
    agent_rpc::orchestrator::FeedbackAggregator::initialize(redis_client_.get());
    agent_rpc::orchestrator::FeedbackAggregator::setRuntimeRepository(runtime_repository_.get());

    // Initialize lifecycle service AFTER Redis is connected; feedback itself
    // is persisted to PostgreSQL (owner-scoped), Redis stays optional.
    agent_lifecycle_service_impl_ = std::make_unique<AgentLifecycleServiceImpl>(redis_client_.get());
    agent_lifecycle_service_impl_->setAgentRuntimeRepository(runtime_repository_.get());
    agent_lifecycle_service_impl_->setQueryDomainRepository(query_domain_repository_.get());

    // Initialize observability service (reads traces + cost ledger from PostgreSQL)
    observability_service_impl_ = std::make_unique<ObservabilityServiceImpl>(redis_client_.get());
    observability_service_impl_->setAgentRuntimeRepository(runtime_repository_.get());
    observability_service_impl_->setQueryDomainRepository(query_domain_repository_.get());

    service_impl_ = std::make_shared<AgentCommunicationServiceImpl>();
    // Agent registration/heartbeat writes the durable registry to PostgreSQL
    // and keeps only liveness in Redis.
    service_impl_->setAgentRuntimeRepository(runtime_repository_.get());
    service_impl_->setRedisClient(redis_client_.get());
    health_service_impl_ = std::make_shared<HealthServiceImpl>();
    ai_query_service_impl_ = std::make_shared<AIQueryServiceImpl>();
    auth_service_impl_ = std::make_shared<AuthServiceImpl>(auth_repository_.get());
    // P26 T2: the cache read-modify-write lives in the shared AuthCache
    // helper (injected here through AuthInterceptor::setRedisClient) so
    // AuthServiceImpl stays Redis-free (PostgreSQL remains the sole session
    // fact source, contract-locked). Null/disconnected Redis degrades to the
    // authoritative JOIN lookup.
    AuthInterceptor::setRedisClient(redis_client_.get());

    // P26 T3 (trusted proxy, dual-mode transition): NEXUSAI_TRUST_PROXY=1
    // switches the interceptor to accept HMAC-signed x-nexusai-* identity
    // headers injected by the gateway. The shared secret is mandatory in
    // trust mode — missing/too short refuses startup (fail-fast) rather than
    // silently trusting unsigned headers.
    const char* trust_env = std::getenv("NEXUSAI_TRUST_PROXY");
    const bool trust_proxy =
        trust_env != nullptr && std::string(trust_env) == "1";
    const char* hmac_env = std::getenv("NEXUSAI_PROXY_HMAC_SECRET");
    const std::string hmac_secret =
        hmac_env == nullptr ? std::string{} : std::string{hmac_env};
    if (trust_proxy && hmac_secret.size() < 32) {
        LOG_ERROR("NEXUSAI_TRUST_PROXY=1 requires NEXUSAI_PROXY_HMAC_SECRET "
                  "(>= 32 chars); refusing to start in trust mode");
        return false;
    }
    AuthInterceptor::setTrustedProxy(hmac_secret, trust_proxy);
    // Trust mode requireAdmin() re-resolves the real role from PostgreSQL
    // (the injected role header is never trusted for admin gates).
    AuthInterceptor::setAuthService(auth_service_impl_.get());
    
    common::MessageSerializer::getInstance().initialize(common::SerializerFactory::PROTOBUF_BINARY);
    
    // The durable query pipeline depends on PostgreSQL; fail startup rather
    // than silently degrade (no Query service registered on failure).
    if (!ai_query_service_impl_->initialize(config_, a2a_config_, redis_client_.get(),
                                            *postgres_store_,
                                            *query_domain_repository_,
                                            *budget_repository_)) {
        LOG_ERROR("Failed to initialize AI Query Service; refusing to start without it");
        return false;
    }

    // agent_invocations producer wiring (final wrap-up): Query/QueryStream
    // become the table's production writer. Facts are owner-scoped via the
    // authenticated session; write failures are logged only and never
    // affect the query outcome (observability data, not the source of truth).
    ai_query_service_impl_->setInvocationRepository(runtime_repository_.get());

    // Wire the durable Replay/Export/Share services to the PostgreSQL source of
    // truth. SharingServiceImpl gets the store + domain repository; the
    // ReplayService gets the repository, the current route provider and a
    // pipeline executor that invokes the already-initialized durable Query
    // pipeline (owner still comes from the thread-local auth context, never
    // from the request body).
    sharing_service_impl_->setStore(postgres_store_.get());
    sharing_service_impl_->setQueryDomainRepository(query_domain_repository_.get());

    orchestrator::ExportService::instance().configure(query_domain_repository_.get());

    std::shared_ptr<AIQueryServiceImpl> pipeline = ai_query_service_impl_;
    orchestrator::ReplayService::instance().configure(
        query_domain_repository_.get(),
        [pipeline]() -> std::string {
            // Same source of truth as the Query pipeline's route label —
            // a non-null router alone does not mean orchestration is on.
            return (pipeline && pipeline->isOrchestratorEnabled())
                       ? "multi-agent"
                       : "single-agent-a2a";
        },
        [pipeline](const std::string& request_id, const std::string& context_id,
                   const std::string& question, std::string& answer,
                   std::string& error) -> bool {
            if (!pipeline) {
                error = "AI Query pipeline unavailable";
                return false;
            }
            agent_communication::AIQueryRequest replay_request;
            replay_request.set_request_id(request_id);
            replay_request.set_context_id(context_id);
            replay_request.set_question(question);
            agent_communication::AIQueryResponse replay_response;
            grpc::ServerContext replay_context;
            const auto replay_status =
                pipeline->Query(&replay_context, &replay_request, &replay_response);
            if (!replay_status.ok()) {
                error = replay_status.error_message();
                return false;
            }
            if (replay_response.status().code() != 0) {
                error = replay_response.status().message();
                return false;
            }
            answer = replay_response.answer();
            return true;
        });

    // Wire the Sandbox / Compare / Intervention / Undo / Autonomy workflows to the
    // PostgreSQL source of truth and to one shared pipeline executor. The
    // executor invokes the already-initialized durable Query pipeline with
    // the sandbox flag set (skips long-term memory writes); budget, owner,
    // trace, cost, cancellation and finalization all stay inside the
    // pipeline — nothing is duplicated here. The owner still comes from the
    // thread-local auth context (compare worker threads propagate it via
    // AuthInterceptor::propagateAuth before invoking the executor).
    user_experience_service_impl_->setQueryDomainRepository(query_domain_repository_.get());
    auto sandbox_executor = [pipeline](const std::string& request_id,
                                       const std::string& context_id,
                                       const std::string& question, std::string& answer,
                                       std::string& error) -> bool {
        if (!pipeline) {
            error = "AI Query pipeline unavailable";
            return false;
        }
        agent_communication::AIQueryRequest sandbox_request;
        sandbox_request.set_request_id(request_id);
        sandbox_request.set_context_id(context_id);
        sandbox_request.set_question(question);
        sandbox_request.set_sandbox(true);
        agent_communication::AIQueryResponse sandbox_response;
        grpc::ServerContext sandbox_context;
        const auto sandbox_status =
            pipeline->Query(&sandbox_context, &sandbox_request, &sandbox_response);
        if (!sandbox_status.ok()) {
            error = sandbox_status.error_message();
            return false;
        }
        if (sandbox_response.status().code() != 0) {
            error = sandbox_response.status().message();
            return false;
        }
        answer = sandbox_response.answer();
        return true;
    };
    user_experience_service_impl_->setExecutor(sandbox_executor);
    agent_lifecycle_service_impl_->setExecutor(sandbox_executor);

    // Wire AgentRouter to AgentCommunicationService for registration sync
    if (ai_query_service_impl_ && service_impl_) {
        auto* router = ai_query_service_impl_->getAgentRouter();
        if (router) {
            service_impl_->setAgentRouter(router);
            // Owner-aware routing quality. The provider runs on the
            // serving thread, so the authenticated owner comes from the
            // thread-local auth context set by AuthInterceptor. It reads the
            // PRE-AGGREGATED agent_route_quality row (kept fresh by
            // SubmitFeedback and the hourly aggregator) instead of scanning
            // the raw feedback table on the routing hot path. A negative
            // return means "no quality data for this owner" — the router then
            // uses its neutral default and never borrows another owner's
            // ratings.
            common::AgentRuntimeRepository* runtime_repo = runtime_repository_.get();
            router->setQualityProvider(
                [runtime_repo](const std::string& agent_id,
                               const std::string& skill_name) -> double {
                    if (runtime_repo == nullptr) {
                        return -1.0;
                    }
                    const std::string owner = AuthInterceptor::currentUserId();
                    if (owner.empty()) {
                        return -1.0;
                    }
                    try {
                        const auto quality = runtime_repo->getRouteQuality(
                            owner, agent_id, skill_name);
                        if (!quality.has_value()) {
                            return -1.0;
                        }
                        const double weight = std::stod(quality->routing_weight);
                        return weight > 0.0 ? weight : -1.0;
                    } catch (const std::exception&) {
                        return -1.0;
                    }
                });
        }
    }

    setupServer();
    initializeServiceRegistry();
    
    LOG_INFO("RPC server initialized on " + address_);
    return true;
}

bool RpcServer::start() {
    if (running_) {
        LOG_WARN("RPC server is already running");
        return true;
    }
    
    if (!server_) {
        LOG_ERROR("RPC server not initialized");
        return false;
    }
    
    try {
        // Start the server on a background thread
        server_thread_ = std::thread([this]() {
            server_->Wait();
        });
        
        running_ = true;
        LOG_INFO("RPC server started successfully");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to start RPC server: " + std::string(e.what()));
        return false;
    }
}

void RpcServer::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    if (server_) {
        // Bound the shutdown wait
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
        server_->Shutdown(deadline);
    }
    
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    
    // Shut down the query service after the gRPC server has fully stopped
    if (ai_query_service_impl_) {
        ai_query_service_impl_->shutdown();
    }

    unregisterService();
    
    LOG_INFO("RPC server stopped");
}

void RpcServer::wait() {
    if (server_) {
        server_->Wait();
    }
}

std::shared_ptr<AgentCommunicationServiceImpl> RpcServer::getService() {
    return service_impl_;
}

std::shared_ptr<HealthServiceImpl> RpcServer::getHealthService() {
    return health_service_impl_;
}

std::shared_ptr<AIQueryServiceImpl> RpcServer::getAIQueryService() {
    return ai_query_service_impl_;
}

std::shared_ptr<AuthServiceImpl> RpcServer::getAuthService() {
    return auth_service_impl_;
}

void RpcServer::setA2AConfig(const a2a_adapter::A2AConfig& config) {
    a2a_config_ = config;
}

void RpcServer::setMCPServerPath(const std::string& path) {
    mcp_server_path_ = path;
}

void RpcServer::setMCPServerArgs(const std::vector<std::string>& args) {
    mcp_server_args_ = args;
}

void RpcServer::initializeServiceRegistry() {
    if (!config_.enable_service_registry || config_.registry_address.empty()) {
        return;
    }

    if (config_.registry_address == "memory" || config_.registry_address.rfind("memory://", 0) == 0) {
        service_registry_ = std::make_shared<registry::MemoryServiceRegistry>();
    } else if (config_.registry_address.rfind("consul://", 0) == 0) {
        auto consul = std::make_shared<registry::ConsulServiceRegistry>();
        consul->initialize(stripScheme(config_.registry_address, "consul"));
        service_registry_ = consul;
    } else if (config_.registry_address.rfind("etcd://", 0) == 0) {
        // etcd is outside the supported local deployment: this branch is only
        // reachable when an operator explicitly points RPC_REGISTRY_ADDRESS
        // at etcd://. The supported local path is the in-memory registry plus
        // the PostgreSQL agent_registry table.
        LOG_WARN("etcd service registry is not part of the supported local "
                 "deployment; expect no maintenance for this backend");
        // deep-review reg-R2: EtcdServiceRegistry is a stub whose every call
        // throws "not implemented" — an uncaught throw here would escape
        // initialize() and std::terminate the process with no log line.
        // Catch and degrade to the in-memory registry (consistent with the
        // fail-soft "registration is optional" style of this function).
        try {
            auto etcd = std::make_shared<registry::EtcdServiceRegistry>();
            etcd->initialize(stripScheme(config_.registry_address, "etcd"));
            service_registry_ = etcd;
        } catch (const std::exception& error) {
            LOG_ERROR("etcd registry backend unavailable (" +
                      std::string(error.what()) +
                      "); falling back to the in-memory registry");
            service_registry_ = std::make_shared<registry::MemoryServiceRegistry>();
        }
    } else {
        auto consul = std::make_shared<registry::ConsulServiceRegistry>();
        consul->initialize(config_.registry_address);
        service_registry_ = consul;
    }

    const auto endpoint = buildSelfEndpoint(address_);
    if (service_registry_->registerService(endpoint)) {
        registered_service_id_ = buildRegisteredServiceId(service_registry_, endpoint);
        LOG_INFO("RPC server registered in service registry as: " + registered_service_id_);
    } else {
        LOG_WARN("Failed to register RPC server in service registry");
    }
}

void RpcServer::unregisterService() {
    if (!service_registry_ || registered_service_id_.empty()) {
        return;
    }

    service_registry_->unregisterService(registered_service_id_);
    LOG_INFO("RPC server unregistered from service registry: " + registered_service_id_);
    registered_service_id_.clear();
}

void RpcServer::setupServer() {
    grpc::ServerBuilder builder;
    // The local WSL/Compose deployment intentionally uses plaintext gRPC.
    // Authentication is handled by the RPC interceptor, not transport TLS.
    builder.AddListeningPort(address_, grpc::InsecureServerCredentials());
    LOG_INFO("RPC server listening on " + address_ + " (local transport)");
    builder.SetMaxReceiveMessageSize(config_.max_receive_message_size);
    builder.SetMaxSendMessageSize(config_.max_message_size);
    
    if (ai_query_service_impl_ && ai_query_service_impl_->isAvailable()) {
        builder.RegisterService(
            static_cast<agent_communication::AIQueryService::Service*>(
                ai_query_service_impl_.get()));
        LOG_INFO("AI Query Service registered");

        // Also register the OrchestrationService (DAG execution) on the same
        // service implementation via the second base class.
        builder.RegisterService(
            static_cast<agent_communication::OrchestrationService::Service*>(
                ai_query_service_impl_.get()));
        LOG_INFO("Orchestration Service (DAG) registered");
    }

    if (service_impl_) {
        builder.RegisterService(service_impl_.get());
        LOG_INFO("Agent Communication Service registered");
    }

    if (health_service_impl_) {
        builder.RegisterService(health_service_impl_.get());
        LOG_INFO("Health Service registered");
    }

    if (auth_service_impl_) {
        builder.RegisterService(auth_service_impl_.get());
        LOG_INFO("User Auth Service registered");
    }

    if (sharing_service_impl_) {
        builder.RegisterService(sharing_service_impl_.get());
        LOG_INFO("Sharing Service registered");
    }

    if (agent_lifecycle_service_impl_) {
        builder.RegisterService(agent_lifecycle_service_impl_.get());
        LOG_INFO("Agent Lifecycle Service registered");
    }

    if (user_experience_service_impl_) {
        builder.RegisterService(user_experience_service_impl_.get());
        LOG_INFO("User Experience Service registered");
    }

    if (observability_service_impl_) {
        builder.RegisterService(observability_service_impl_.get());
        LOG_INFO("Observability Service registered");
    }

    if (auth_service_impl_) {
        AuthInterceptor::setAuthEnabled(true);
        std::vector<std::unique_ptr<grpc::experimental::ServerInterceptorFactoryInterface>>
            interceptor_creators;
        interceptor_creators.push_back(
            std::make_unique<AuthInterceptorFactory>(auth_service_impl_.get()));
        builder.experimental().SetInterceptorCreators(
            std::move(interceptor_creators));
        LOG_INFO("Auth interceptor registered");
    }
    
    grpc::EnableDefaultHealthCheckService(true);
    
    grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 30000);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 5000);
    builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, true);
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
    builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 10000);
    
    server_ = builder.BuildAndStart();
    
    if (!server_) {
        throw std::runtime_error("Failed to build gRPC server");
    }
}

} // namespace server
} // namespace agent_rpc
