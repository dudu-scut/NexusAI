#pragma once

#include "agent_rpc/common/types.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/metrics.h"
#include "agent_rpc/common/redis_client.h"
#include "agent_rpc/common/postgres_store.h"
#include "agent_rpc/common/auth_repository.h"
#include "agent_rpc/common/query_domain_repository.h"
#include "agent_rpc/common/postgres_budget_repository.h"
#include "agent_rpc/common/agent_runtime_repository.h"
#include "agent_rpc/a2a_adapter/a2a_config.h"
#include "agent_rpc/registry/service_registry.h"
#include "agent_rpc/server/agent_lifecycle_service.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>

namespace agent_rpc {
namespace server {

// Forward declarations
class AgentCommunicationServiceImpl;
class HealthServiceImpl;
class AIQueryServiceImpl;
class AuthServiceImpl;
class AgentLifecycleServiceImpl;
class SharingServiceImpl;
class UserExperienceServiceImpl;
class ObservabilityServiceImpl;

// RPC server class
class RpcServer {
public:
    RpcServer();
    ~RpcServer();
    
    bool initialize(const common::RpcConfig& config);
    
    bool start();
    
    void stop();
    
    void wait();
    
    std::shared_ptr<AgentCommunicationServiceImpl> getService();
    
    std::shared_ptr<HealthServiceImpl> getHealthService();
    
    std::shared_ptr<AIQueryServiceImpl> getAIQueryService();

    std::shared_ptr<AuthServiceImpl> getAuthService();

    // P17(m): the profile summarizer's source-material injection point
    // (PostgreSQL conversation history instead of the empty Redis tier).
    common::QueryDomainRepository* getQueryDomainRepository() {
        return query_domain_repository_.get();
    }

    // Redis client (liveness/metrics cache only)
    common::RedisClient* getRedisClient() { return redis_client_.get(); }
    
    void setA2AConfig(const a2a_adapter::A2AConfig& config);
    
    bool isRunning() const { return running_; }
    
    std::string getAddress() const { return address_; }
    
    void setMCPServerPath(const std::string& path);
    void setMCPServerArgs(const std::vector<std::string>& args);

private:
    void setupServer();
    void initializeMCPClient();
    void initializeServiceRegistry();
    void unregisterService();
    
    common::RpcConfig config_;
    std::string address_;
    std::atomic<bool> running_{false};
    
    std::unique_ptr<grpc::Server> server_;
    std::thread server_thread_;
    std::shared_ptr<AgentCommunicationServiceImpl> service_impl_;
    std::shared_ptr<HealthServiceImpl> health_service_impl_;
    std::shared_ptr<AIQueryServiceImpl> ai_query_service_impl_;
    std::shared_ptr<AuthServiceImpl> auth_service_impl_;
    std::unique_ptr<SharingServiceImpl> sharing_service_impl_;
    std::unique_ptr<AgentLifecycleServiceImpl> agent_lifecycle_service_impl_;
    std::unique_ptr<UserExperienceServiceImpl> user_experience_service_impl_;
    std::unique_ptr<ObservabilityServiceImpl> observability_service_impl_;
    std::unique_ptr<common::RedisClient> redis_client_;
    std::unique_ptr<common::PostgresStore> postgres_store_;
    std::unique_ptr<common::AuthRepository> auth_repository_;
    // RpcServer is the single owner of the durable PostgreSQL repositories;
    // AIQueryServiceImpl only keeps non-owning references to them.
    std::unique_ptr<common::QueryDomainRepository> query_domain_repository_;
    std::unique_ptr<common::PostgresBudgetRepository> budget_repository_;
    // Owner-scoped runtime facts (registry/feedback/route quality/costs).
    std::unique_ptr<common::AgentRuntimeRepository> runtime_repository_;
    
    // A2A configuration
    a2a_adapter::A2AConfig a2a_config_;
    
    // MCP config (reserved; MCP client not yet implemented)
    std::string mcp_server_path_;
    std::vector<std::string> mcp_server_args_;

    std::shared_ptr<registry::ServiceRegistry> service_registry_;
    std::string registered_service_id_;
    std::vector<std::unique_ptr<grpc::ServerBuilder>> builders_;
};

} // namespace server
} // namespace agent_rpc
