#pragma once

#include "agent_rpc/common/types.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/metrics.h"
#include "agent_rpc/common/redis_client.h"
#include "agent_rpc/common/agent_runtime_repository.h"
#include "agent_service.grpc.pb.h"
#include "agent_service.pb.h"
#include "common.pb.h"
#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <set>
#include <unordered_map>

namespace agent_rpc {
namespace orchestrator {
    class AgentRouter;
}
}

namespace agent_rpc {
namespace server {

// Agent communication service gRPC implementation
class AgentCommunicationServiceImpl final
    : public agent_communication::AgentCommunicationService::Service {
public:
    AgentCommunicationServiceImpl();
    ~AgentCommunicationServiceImpl();

    grpc::Status SendMessage(
        grpc::ServerContext* context,
        const agent_communication::SendMessageRequest* request,
        agent_communication::SendMessageResponse* response) override;

    grpc::Status ReceiveMessage(
        grpc::ServerContext* context,
        const agent_communication::ReceiveMessageRequest* request,
        agent_communication::ReceiveMessageResponse* response) override;

    grpc::Status BroadcastMessage(
        grpc::ServerContext* context,
        const agent_communication::BroadcastMessageRequest* request,
        agent_communication::BroadcastMessageResponse* response) override;

    grpc::Status GetAgents(
        grpc::ServerContext* context,
        const agent_communication::GetAgentsRequest* request,
        agent_communication::GetAgentsResponse* response) override;

    grpc::Status FindAgents(
        grpc::ServerContext* context,
        const agent_communication::FindAgentsRequest* request,
        agent_communication::FindAgentsResponse* response) override;

    grpc::Status RegisterAgent(
        grpc::ServerContext* context,
        const agent_communication::RegisterAgentRequest* request,
        agent_communication::RegisterAgentResponse* response) override;

    grpc::Status UnregisterAgent(
        grpc::ServerContext* context,
        const agent_communication::UnregisterAgentRequest* request,
        agent_communication::UnregisterAgentResponse* response) override;

    grpc::Status Heartbeat(
        grpc::ServerContext* context,
        const agent_communication::HeartbeatRequest* request,
        agent_communication::HeartbeatResponse* response) override;

    grpc::Status ListenMessages(
        grpc::ServerContext* context,
        const agent_communication::ReceiveMessageRequest* request,
        grpc::ServerWriter<agent_communication::Message>* writer) override;

    grpc::Status BatchSendMessages(
        grpc::ServerContext* context,
        grpc::ServerReader<agent_communication::SendMessageRequest>* reader,
        agent_communication::SendMessageResponse* response) override;

    grpc::Status RealTimeCommunication(
        grpc::ServerContext* context,
        grpc::ServerReaderWriter<agent_communication::Message,
                                 agent_communication::Message>* stream) override;

    void setMessageHandler(common::MessageHandler handler);
    void setErrorHandler(common::ErrorHandler handler);
    void setHealthCheckHandler(common::HealthCheckHandler handler);

    /**
     * @brief Set AgentRouter for registry unification
     *
     * When set, RegisterAgent/UnregisterAgent/Heartbeat will sync
     * agent state to the orchestrator's AgentRouter.
     */
    void setAgentRouter(orchestrator::AgentRouter* router);

    /**
     * @brief Durable registry + liveness cache wiring.
     *
     * RegisterAgent/UnregisterAgent/Heartbeat persist agent_registry rows
     * through the runtime repository (PostgreSQL is the source of truth);
     * Redis only carries the short-lived liveness key.
     */
    void setAgentRuntimeRepository(common::AgentRuntimeRepository* repository);
    void setRedisClient(common::RedisClient* redis);

    std::vector<common::ServiceEndpoint> getAgentsList() const;

private:
    std::string generateMessageId();
    bool isAgentOnline(const std::string& agent_id);
    void updateAgentHeartbeat(const std::string& agent_id);
    void cleanupOfflineAgents();
    void addToIndexes(const std::string& agent_id, const common::ServiceEndpoint& endpoint);
    void removeFromIndexes(const std::string& agent_id);

    mutable std::mutex agents_mutex_;
    std::map<std::string, common::ServiceEndpoint> agents_;
    std::map<std::string, common::MessageQueue<agent_communication::Message>> agent_message_queues_;
    // Registrant owner per queue: the agent's inbox belongs to whoever
    // registered it — reads are owner-scoped (tenant isolation).
    std::map<std::string, std::string> agent_queue_owners_;
    // Redis liveness TTL negotiated at registration time (3x heartbeat interval,
    // never below 5 minutes); Heartbeat reuses it so both paths stay aligned.
    std::unordered_map<std::string, int> agent_liveness_ttl_;

    // Tag/skill inverted index (agent_id sets) for fast FindAgents queries
    std::unordered_map<std::string, std::set<std::string>> tags_index_;
    std::unordered_map<std::string, std::set<std::string>> skills_index_;

    common::MessageHandler message_handler_;
    common::ErrorHandler error_handler_;
    common::HealthCheckHandler health_check_handler_;

    std::atomic<int> message_id_counter_{0};
    std::thread cleanup_thread_;
    std::atomic<bool> cleanup_running_{false};

    // Optional pointer to the orchestrator's AgentRouter for registration sync
    orchestrator::AgentRouter* router_ = nullptr;

    // Durable registry persistence (PostgreSQL) + Redis liveness cache
    common::AgentRuntimeRepository* runtime_repository_ = nullptr;
    common::RedisClient* redis_ = nullptr;
};

// Health check service gRPC implementation
class HealthServiceImpl final
    : public agent_communication::HealthService::Service {
public:
    HealthServiceImpl();
    ~HealthServiceImpl() = default;

    grpc::Status Check(
        grpc::ServerContext* context,
        const agent_communication::common::HealthCheckRequest* request,
        agent_communication::common::HealthCheckResponse* response) override;

    grpc::Status Watch(
        grpc::ServerContext* context,
        const agent_communication::common::HealthCheckRequest* request,
        grpc::ServerWriter<agent_communication::common::HealthCheckResponse>* writer) override;

    void setHealthCheckHandler(common::HealthCheckHandler handler);

private:
    common::HealthCheckHandler health_check_handler_;
};

} // namespace server
} // namespace agent_rpc
