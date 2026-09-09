#include "agent_rpc/server/agent_service.h"
#include "agent_rpc/server/auth_interceptor.h"
#include "agent_rpc/common/logger.h"
#include "agent_rpc/common/metrics.h"
#include "agent_rpc/common/key_validation.h"
#include "agent_rpc/orchestrator/agent_router.h"
#include "agent_rpc/orchestrator/agent_info.h"
#include "agent_rpc/registry/service_registry.h"
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace agent_rpc {
namespace server {

AgentCommunicationServiceImpl::AgentCommunicationServiceImpl()
    : cleanup_running_(false) {
    cleanup_running_ = true;
    cleanup_thread_ = std::thread([this]() {
        while (cleanup_running_) {
            for (int i = 0; i < 30 && cleanup_running_; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (cleanup_running_) {
                cleanupOfflineAgents();
            }
        }
    });
}

AgentCommunicationServiceImpl::~AgentCommunicationServiceImpl() {
    cleanup_running_ = false;
    if (cleanup_thread_.joinable()) {
        cleanup_thread_.join();
    }
}

void AgentCommunicationServiceImpl::setMessageHandler(common::MessageHandler handler) {
    message_handler_ = handler;
}

void AgentCommunicationServiceImpl::setErrorHandler(common::ErrorHandler handler) {
    error_handler_ = handler;
}

void AgentCommunicationServiceImpl::setHealthCheckHandler(common::HealthCheckHandler handler) {
    health_check_handler_ = handler;
}

void AgentCommunicationServiceImpl::setAgentRouter(orchestrator::AgentRouter* router) {
    router_ = router;
    if (router_) {
        LOG_INFO("AgentRouter connected to AgentCommunicationService");
    }
}

void AgentCommunicationServiceImpl::setAgentRuntimeRepository(
    common::AgentRuntimeRepository* repository) {
    runtime_repository_ = repository;
    if (runtime_repository_) {
        LOG_INFO("AgentRuntimeRepository connected to AgentCommunicationService");
    }
}

void AgentCommunicationServiceImpl::setRedisClient(common::RedisClient* redis) {
    redis_ = redis;
}

namespace {
// Liveness cache TTL: three missed heartbeats, never below 5 minutes.
constexpr int kDefaultLivenessTtlSeconds = 300;

// Per-agent inbox cap (deep-review): an offline agent must not grow an
// unbounded in-memory queue. New messages are rejected once the cap is hit;
// dropping old messages could discard commands the agent never consumed.
constexpr size_t kMaxInboxMessages = 5000;

int livenessTtlSeconds(int heartbeat_interval) {
    if (heartbeat_interval <= 0) {
        return kDefaultLivenessTtlSeconds;
    }
    return std::max(heartbeat_interval * 3, kDefaultLivenessTtlSeconds);
}
}  // namespace

std::string AgentCommunicationServiceImpl::generateMessageId() {
    return std::to_string(++message_id_counter_);
}

bool AgentCommunicationServiceImpl::isAgentOnline(const std::string& agent_id) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    return agents_.find(agent_id) != agents_.end();
}

void AgentCommunicationServiceImpl::updateAgentHeartbeat(const std::string& agent_id) {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    auto it = agents_.find(agent_id);
    if (it != agents_.end()) {
        it->second.last_heartbeat = std::chrono::steady_clock::now();
    }
}

void AgentCommunicationServiceImpl::cleanupOfflineAgents() {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    auto now = std::chrono::steady_clock::now();

    auto it = agents_.begin();
    while (it != agents_.end()) {
        // deep-review R4: the grace period must honor the TTL negotiated at
        // registration (max(3*interval, 300s), stored in agent_liveness_ttl_
        // by RegisterAgent and read back by Heartbeat) — a fixed 300s here
        // would delete perfectly healthy agents whose legal heartbeat
        // interval exceeds 100s, dropping their queued inbox messages.
        int ttl = kDefaultLivenessTtlSeconds;
        const auto ttl_it = agent_liveness_ttl_.find(it->first);
        if (ttl_it != agent_liveness_ttl_.end()) {
            ttl = ttl_it->second;
        }
        const auto timeout = std::chrono::seconds(ttl);
        if (now - it->second.last_heartbeat > timeout) {
            LOG_WARN("Agent offline, removing: " + it->first);
            removeFromIndexes(it->first);
            agent_message_queues_.erase(it->first);
            agent_queue_owners_.erase(it->first);
            common::Metrics::getInstance().recordDisconnection(it->first);
            // Also remove from the router to prevent routing to dead agents
            if (router_) {
                router_->removeAgent(it->first);
            }
            it = agents_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<common::ServiceEndpoint> AgentCommunicationServiceImpl::getAgentsList() const {
    std::lock_guard<std::mutex> lock(agents_mutex_);
    std::vector<common::ServiceEndpoint> result;
    for (const auto& pair : agents_) {
        result.push_back(pair.second);
    }
    return result;
}

grpc::Status AgentCommunicationServiceImpl::SendMessage(
    grpc::ServerContext* /*context*/,
    const agent_communication::SendMessageRequest* request,
    agent_communication::SendMessageResponse* response) {

    if (!AuthInterceptor::isAuthenticated()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Valid authentication token required");
    }

    auto start = std::chrono::steady_clock::now();
    const auto& target = request->target_agent();

    std::lock_guard<std::mutex> lock(agents_mutex_);
    auto it = agent_message_queues_.find(target);
    if (it != agent_message_queues_.end() &&
        it->second.size() >= kMaxInboxMessages) {
        // Bounded inbox (deep-review): reject new messages once the cap is
        // hit; dropping old messages could discard commands the agent never
        // consumed while it was offline.
        auto* status = response->mutable_status();
        status->set_code(1);
        status->set_message("Target agent inbox full: " + target);
    } else if (it != agent_message_queues_.end()) {
        // Stamp the authenticated sender so the receiving side can attribute
        // messages (the Message proto has no dedicated sender field; headers
        // keep the proto contract untouched).
        agent_communication::Message stamped = request->message();
        (*stamped.mutable_headers())["x-sender-owner"] =
            AuthInterceptor::currentUserId();
        it->second.push(std::move(stamped));
        auto* status = response->mutable_status();
        status->set_code(0);
        status->set_message("OK");
        response->set_message_id(generateMessageId());
        response->set_timestamp(std::chrono::system_clock::now().time_since_epoch().count());
    } else {
        auto* status = response->mutable_status();
        status->set_code(1);
        status->set_message("Target agent not found: " + target);
    }

    auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    common::Metrics::getInstance().recordRpcRequest("AgentCommunicationService", "SendMessage", dur.count());
    return grpc::Status::OK;
}

grpc::Status AgentCommunicationServiceImpl::ReceiveMessage(
    grpc::ServerContext* /*context*/,
    const agent_communication::ReceiveMessageRequest* request,
    agent_communication::ReceiveMessageResponse* response) {

    if (!AuthInterceptor::isAuthenticated()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Valid authentication token required");
    }

    const auto& agent_id = request->agent_id();
    int max_messages = request->max_messages();
    if (max_messages <= 0) max_messages = 10;

    std::lock_guard<std::mutex> lock(agents_mutex_);
    auto it = agent_message_queues_.find(agent_id);
    if (it == agent_message_queues_.end()) {
        auto* status = response->mutable_status();
        status->set_code(1);
        status->set_message("Agent not found: " + agent_id);
        return grpc::Status::OK;
    }

    // Tenant isolation: the inbox belongs to the agent's registrant — a
    // caller that is not the queue owner may not drain another tenant's
    // messages.
    const std::string caller = AuthInterceptor::currentUserId();
    auto owner_it = agent_queue_owners_.find(agent_id);
    if (owner_it == agent_queue_owners_.end() || owner_it->second != caller) {
        auto* status = response->mutable_status();
        status->set_code(7);  // PERMISSION_DENIED
        status->set_message("Only the agent's registrant may read its inbox");
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                            "Only the agent's registrant may read its inbox");
    }

    agent_communication::Message msg;
    int count = 0;
    while (count < max_messages && it->second.try_pop(msg, std::chrono::milliseconds(0))) {
        *response->add_messages() = msg;
        count++;
    }

    auto* status = response->mutable_status();
    status->set_code(0);
    status->set_message("OK");
    return grpc::Status::OK;
}

grpc::Status AgentCommunicationServiceImpl::BroadcastMessage(
    grpc::ServerContext* /*context*/,
    const agent_communication::BroadcastMessageRequest* request,
    agent_communication::BroadcastMessageResponse* response) {

    if (!AuthInterceptor::isAuthenticated()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Valid authentication token required");
    }

    std::lock_guard<std::mutex> lock(agents_mutex_);
    int success_count = 0;
    int failure_count = 0;

    if (request->target_agents_size() == 0) {
        for (auto& pair : agent_message_queues_) {
            if (pair.second.size() >= kMaxInboxMessages) {
                // Full inboxes are reported as undelivered, like missing
                // targets; the message must not be silently dropped.
                failure_count++;
                response->add_failed_agents(pair.first);
                continue;
            }
            pair.second.push(request->message());
            success_count++;
        }
    } else {
        for (const auto& agent_id : request->target_agents()) {
            auto it = agent_message_queues_.find(agent_id);
            if (it == agent_message_queues_.end() ||
                it->second.size() >= kMaxInboxMessages) {
                failure_count++;
                response->add_failed_agents(agent_id);
            } else {
                it->second.push(request->message());
                success_count++;
            }
        }
    }

    auto* status = response->mutable_status();
    status->set_code(0);
    status->set_message("OK");
    response->set_success_count(success_count);
    response->set_failure_count(failure_count);
    return grpc::Status::OK;
}

grpc::Status AgentCommunicationServiceImpl::GetAgents(
    grpc::ServerContext* /*context*/,
    const agent_communication::GetAgentsRequest* request,
    agent_communication::GetAgentsResponse* response) {

    if (!AuthInterceptor::isAuthenticated()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Valid authentication token required");
    }

    std::lock_guard<std::mutex> lock(agents_mutex_);
    int offset = request->offset();
    if (offset < 0) offset = 0;
    int limit = request->limit();
    if (limit <= 0) limit = 100;

    // deep-review: apply the filter before offset/limit and report the
    // filtered total. The legacy loop consumed offset inside the filter scan
    // (filtered-out agents still advanced the page) and set total_count to
    // the unfiltered registry size, which broke paging whenever a filter was
    // active.
    std::vector<const common::ServiceEndpoint*> matches;
    matches.reserve(agents_.size());
    for (const auto& pair : agents_) {
        if (!request->filter().empty() &&
            pair.second.service_name.find(request->filter()) == std::string::npos) {
            continue;
        }
        matches.push_back(&pair.second);
    }
    response->set_total_count(static_cast<int>(matches.size()));

    int added = 0;
    for (size_t i = static_cast<size_t>(offset); i < matches.size(); ++i) {
        if (added >= limit) break;
        const auto& ep = *matches[i];

        auto* info = response->add_agents();
        info->set_service_name(ep.service_name);
        info->set_version(ep.version);
        info->set_host(ep.host);
        info->set_port(ep.port);
        for (const auto& t : ep.tags) {
            info->add_tags(t);
        }
        for (const auto& m : ep.metadata) {
            (*info->mutable_metadata())[m.first] = m.second;
        }
        for (const auto& s : ep.skills) {
            info->add_skills(s);
        }
        info->set_agent_card(ep.agent_card);
        added++;
    }

    auto* status = response->mutable_status();
    status->set_code(0);
    status->set_message("OK");
    return grpc::Status::OK;
}

grpc::Status AgentCommunicationServiceImpl::RegisterAgent(
    grpc::ServerContext* /*context*/,
    const agent_communication::RegisterAgentRequest* request,
    agent_communication::RegisterAgentResponse* response) {

    // Agent lifecycle management is an admin capability. The local
    // deployment gates it on the statically configured ADMIN user
    // (NEXUSAI_ADMIN_USERNAME; documented in .env.example).
    const grpc::Status admin_check = AuthInterceptor::requireAdmin();
    if (!admin_check.ok()) {
        return admin_check;
    }

    const auto& info = request->agent_info();

    // Validate required fields + P25(b): agent 注册原料（service_name/host）
    // 入口白名单 — service_name 段参与 agent_id 拼接与 Redis 键构造，若含
    // sanitize 有损字符（冒号/控制符）会与合法键同形（a:b vs a_b 碰撞伪冒），
    // 这里直接拒绝；host 按 hostname[:port] 结构校验。
    if (info.service_name().empty() ||
        !common::isSafeKeyComponent(info.service_name(), 64)) {
        auto* status = response->mutable_status();
        status->set_code(3); // INVALID_ARGUMENT
        status->set_message(
            "agent_info.service_name is required and must be 1-64 safe characters");
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            "agent_info.service_name is required and must be 1-64 safe characters");
    }
    if (info.host().empty() || !common::isSafeHostComponent(info.host()) ||
        info.port() <= 0) {
        auto* status = response->mutable_status();
        status->set_code(3);
        status->set_message(
            "agent_info.host must be a hostname[:port] of safe characters and port is required");
        return grpc::Status(
            grpc::StatusCode::INVALID_ARGUMENT,
            "agent_info.host must be a hostname[:port] of safe characters and port is required");
    }

    std::string agent_id = info.service_name() + "-" + info.host() + "-" + std::to_string(info.port());
    const int liveness_ttl = livenessTtlSeconds(request->heartbeat_interval());

    common::ServiceEndpoint endpoint;
    endpoint.host = info.host();
    endpoint.port = info.port();
    endpoint.service_name = info.service_name();
    endpoint.version = info.version();
    endpoint.is_healthy = true;
    endpoint.last_heartbeat = std::chrono::steady_clock::now();
    for (const auto& m : info.metadata()) {
        endpoint.metadata[m.first] = m.second;
    }
    for (const auto& t : info.tags()) {
        endpoint.tags.push_back(t);
    }
    for (const auto& s : info.skills()) {
        endpoint.skills.push_back(s);
    }
    endpoint.agent_card = info.agent_card();

    {
        std::lock_guard<std::mutex> lock(agents_mutex_);
        agents_[agent_id] = endpoint;
        agent_message_queues_.try_emplace(agent_id);
        agent_queue_owners_[agent_id] = AuthInterceptor::currentUserId();
        agent_liveness_ttl_[agent_id] = liveness_ttl;
        addToIndexes(agent_id, endpoint);
    }

    common::Metrics::getInstance().recordConnection(agent_id, true);
    LOG_INFO("Agent registered: " + agent_id);

    // Persist the durable registry fact (PostgreSQL is the source of
    // truth; re-registration after a restart rewrites this row). Failures are
    // logged but do not block the in-memory registration path.
    if (runtime_repository_) {
        try {
            nlohmann::json capabilities;
            capabilities["skills"] = endpoint.skills;
            capabilities["tags"] = endpoint.tags;
            capabilities["version"] = endpoint.version;
            const common::AgentRegistryRecord registry_record{
                .id = "registry-" + agent_id,
                .owner_id = "system",
                .agent_id = agent_id,
                .display_name = endpoint.service_name,
                .capabilities = capabilities.dump(),
                .health_status = "healthy",
            };
            if (!runtime_repository_->upsertAgentRegistry(registry_record)) {
                LOG_WARN("Failed to persist agent_registry row for " + agent_id);
            }
        } catch (const std::exception& error) {
            LOG_WARN("agent_registry persistence failed for " + agent_id + ": " +
                     error.what());
        }
    }
    // Redis carries only the short-lived liveness cache.
    if (redis_) {
        redis_->setex("agent:liveness:" + agent_id, liveness_ttl, "1");
    }

    // Sync to AgentRouter for orchestrator routing
    if (router_) {
        orchestrator::AgentInfo info;
        info.id = agent_id;
        info.name = endpoint.service_name;
        info.url = "http://" + endpoint.host + ":" + std::to_string(endpoint.port);
        info.skills = endpoint.skills;
        info.tags = endpoint.tags;
        info.is_healthy = true;
        info.last_heartbeat = std::chrono::steady_clock::now();
        // Extract description from metadata if available
        auto desc_it = endpoint.metadata.find("description");
        if (desc_it != endpoint.metadata.end()) {
            info.description = desc_it->second;
        }
        auto ver_it = endpoint.metadata.find("version");
        if (ver_it != endpoint.metadata.end()) {
            info.version = ver_it->second;
        } else {
            info.version = endpoint.version;
        }
        // Parse skill descriptions from AgentCard JSON for routing accuracy
        if (!endpoint.agent_card.empty()) {
            try {
                auto card = nlohmann::json::parse(endpoint.agent_card);
                if (card.contains("skills") && card["skills"].is_array()) {
                    for (const auto& skill : card["skills"]) {
                        if (skill.contains("name") && skill.contains("description")) {
                            info.skill_descriptions[skill["name"].get<std::string>()] =
                                skill["description"].get<std::string>();
                        }
                    }
                }
            } catch (const nlohmann::json::exception&) {
                // AgentCard JSON parse failed — skills will route by name only
            }
        }
        router_->addAgent(info);
        LOG_INFO("Agent synced to AgentRouter: " + agent_id);
    }

    auto* status = response->mutable_status();
    status->set_code(0);
    status->set_message("OK");
    response->set_agent_id(agent_id);
    response->set_registration_time(
        std::chrono::system_clock::now().time_since_epoch().count());
    return grpc::Status::OK;
}

grpc::Status AgentCommunicationServiceImpl::UnregisterAgent(
    grpc::ServerContext* /*context*/,
    const agent_communication::UnregisterAgentRequest* request,
    agent_communication::UnregisterAgentResponse* response) {

    const grpc::Status admin_check = AuthInterceptor::requireAdmin();
    if (!admin_check.ok()) {
        return admin_check;
    }

    const auto& agent_id = request->agent_id();
    {
        std::lock_guard<std::mutex> lock(agents_mutex_);
        removeFromIndexes(agent_id);
        agents_.erase(agent_id);
        agent_message_queues_.erase(agent_id);
        agent_queue_owners_.erase(agent_id);
        agent_liveness_ttl_.erase(agent_id);
    }

    common::Metrics::getInstance().recordDisconnection(agent_id);
    LOG_INFO("Agent unregistered: " + agent_id + " reason: " + request->reason());

    // Keep the registry row (history/metrics stay queryable) but mark
    // the agent offline; drop the Redis liveness cache.
    if (runtime_repository_) {
        try {
            runtime_repository_->markAgentStatus(agent_id, "offline");
        } catch (const std::exception& error) {
            LOG_WARN("agent_registry update failed for " + agent_id + ": " + error.what());
        }
    }
    if (redis_) {
        redis_->del("agent:liveness:" + agent_id);
    }

    // Remove from AgentRouter
    if (router_) {
        router_->removeAgent(agent_id);
    }

    auto* status = response->mutable_status();
    status->set_code(0);
    status->set_message("OK");
    response->set_unregistration_time(
        std::chrono::system_clock::now().time_since_epoch().count());
    return grpc::Status::OK;
}

grpc::Status AgentCommunicationServiceImpl::Heartbeat(
    grpc::ServerContext* /*context*/,
    const agent_communication::HeartbeatRequest* request,
    agent_communication::HeartbeatResponse* response) {

    if (!AuthInterceptor::isAuthenticated()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                            "Valid authentication token required");
    }

    // Only agents registered through this process may be refreshed: a bare
    // heartbeat for an unknown id must not mint a durable "healthy" row
    // (registration is the ADMIN-gated path).
    // deep-review R1: heartbeats are also owner-scoped — an arbitrary logged-
    // in user must not be able to keep another registrant's agent alive
    // (same tenant-isolation rule as ReceiveMessage's inbox ownership).
    const std::string heartbeat_caller = AuthInterceptor::currentUserId();
    const bool known_here = [this, &request]() {
        std::lock_guard<std::mutex> lock(agents_mutex_);
        return agent_liveness_ttl_.find(request->agent_id()) !=
               agent_liveness_ttl_.end();
    }();
    if (!known_here) {
        auto* status = response->mutable_status();
        status->set_code(1);
        status->set_message("Agent not registered: " + request->agent_id());
        return grpc::Status::OK;
    }
    {
        std::lock_guard<std::mutex> lock(agents_mutex_);
        const auto owner_it = agent_queue_owners_.find(request->agent_id());
        if (owner_it == agent_queue_owners_.end() ||
            owner_it->second != heartbeat_caller) {
            return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                "Only the agent's registrant may send heartbeats");
        }
    }

    updateAgentHeartbeat(request->agent_id());

    // Refresh the durable registry fact and the liveness cache. The
    // repository call uses upsert semantics, so a heartbeat heals a registry
    // row that was lost when PostgreSQL was down during RegisterAgent.
    if (runtime_repository_) {
        try {
            runtime_repository_->updateAgentHeartbeat(request->agent_id());
        } catch (const std::exception& error) {
            LOG_WARN("agent_registry heartbeat failed for " + request->agent_id() +
                     ": " + error.what());
        }
    }
    if (redis_) {
        // Align the liveness TTL with the interval negotiated at registration
        // time (max(3*interval, 300s)).
        int ttl = kDefaultLivenessTtlSeconds;
        {
            std::lock_guard<std::mutex> lock(agents_mutex_);
            const auto ttl_it = agent_liveness_ttl_.find(request->agent_id());
            if (ttl_it != agent_liveness_ttl_.end()) {
                ttl = ttl_it->second;
            }
        }
        redis_->setex("agent:liveness:" + request->agent_id(), ttl, "1");
    }

    // Propagate heartbeat to AgentRouter
    if (router_) {
        router_->updateHeartbeat(request->agent_id());
    }

    // Feed the health-evaluation loop's heartbeat-timeout guard (90s):
    // every Heartbeat RPC refreshes the live-metrics heartbeat timestamp so
    // silent agents are excluded and recovering agents come back.
    agent_rpc::registry::ServiceRegistry::recordHeartbeat(request->agent_id());

    auto* status = response->mutable_status();
    status->set_code(0);
    status->set_message("OK");
    response->set_server_time(
        std::chrono::system_clock::now().time_since_epoch().count());
    return grpc::Status::OK;
}

grpc::Status AgentCommunicationServiceImpl::ListenMessages(
    grpc::ServerContext* context,
    const agent_communication::ReceiveMessageRequest* request,
    grpc::ServerWriter<agent_communication::Message>* writer) {

    if (!AuthInterceptor::isAuthenticated()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Valid authentication token required");
    }

    const auto& agent_id = request->agent_id();
    int timeout_seconds = request->timeout_seconds();
    if (timeout_seconds <= 0) timeout_seconds = 30;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);

    while (!context->IsCancelled() && std::chrono::steady_clock::now() < deadline) {
        agent_communication::Message msg;
        bool got_message = false;
        {
            std::lock_guard<std::mutex> lock(agents_mutex_);
            auto it = agent_message_queues_.find(agent_id);
            if (it == agent_message_queues_.end()) {
                return grpc::Status(grpc::StatusCode::NOT_FOUND, "Agent not found");
            }
            auto owner_it = agent_queue_owners_.find(agent_id);
            if (owner_it == agent_queue_owners_.end() ||
                owner_it->second != AuthInterceptor::currentUserId()) {
                return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                    "Only the agent's registrant may read its inbox");
            }
            // Use non-blocking pop to avoid holding the global lock during wait
            got_message = it->second.try_pop(msg, std::chrono::milliseconds(0));
        }
        if (got_message) {
            if (!writer->Write(msg)) break;
        } else {
            // Sleep outside the lock to avoid blocking all agent operations
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    return grpc::Status::OK;
}

grpc::Status AgentCommunicationServiceImpl::BatchSendMessages(
    grpc::ServerContext* /*context*/,
    grpc::ServerReader<agent_communication::SendMessageRequest>* reader,
    agent_communication::SendMessageResponse* response) {

    if (!AuthInterceptor::isAuthenticated()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Valid authentication token required");
    }

    agent_communication::SendMessageRequest req;
    int count = 0;
    while (reader->Read(&req)) {
        std::lock_guard<std::mutex> lock(agents_mutex_);
        auto it = agent_message_queues_.find(req.target_agent());
        if (it != agent_message_queues_.end() &&
            it->second.size() < kMaxInboxMessages) {
            it->second.push(req.message());
            count++;
        }
    }

    auto* status = response->mutable_status();
    status->set_code(0);
    status->set_message("Batch processed " + std::to_string(count) + " messages");
    response->set_message_id(generateMessageId());
    return grpc::Status::OK;
}

grpc::Status AgentCommunicationServiceImpl::RealTimeCommunication(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<agent_communication::Message,
                             agent_communication::Message>* stream) {

    // Local delivery boundary: this bidirectional channel never grew
    // beyond an echo placeholder (it did not route messages to any agent and
    // persisted nothing). Instead of returning a successful no-op it now
    // reports UNIMPLEMENTED explicitly. Supported messaging paths are the
    // unary SendMessage/ReceiveMessage/BroadcastMessage RPCs and the
    // server-streaming ListenMessages RPC.
    (void)context;
    (void)stream;
    return grpc::Status(
        grpc::StatusCode::UNIMPLEMENTED,
        "RealTimeCommunication is not implemented in the local delivery "
        "boundary; use SendMessage/ReceiveMessage/BroadcastMessage or "
        "ListenMessages instead");
}

void AgentCommunicationServiceImpl::addToIndexes(
    const std::string& agent_id, const common::ServiceEndpoint& endpoint) {
    for (const auto& tag : endpoint.tags) {
        tags_index_[tag].insert(agent_id);
    }
    for (const auto& skill : endpoint.skills) {
        skills_index_[skill].insert(agent_id);
    }
}

void AgentCommunicationServiceImpl::removeFromIndexes(const std::string& agent_id) {
    auto ait = agents_.find(agent_id);
    if (ait == agents_.end()) return;

    for (const auto& tag : ait->second.tags) {
        auto it = tags_index_.find(tag);
        if (it != tags_index_.end()) {
            it->second.erase(agent_id);
            if (it->second.empty()) tags_index_.erase(it);
        }
    }
    for (const auto& skill : ait->second.skills) {
        auto it = skills_index_.find(skill);
        if (it != skills_index_.end()) {
            it->second.erase(agent_id);
            if (it->second.empty()) skills_index_.erase(it);
        }
    }
}

grpc::Status AgentCommunicationServiceImpl::FindAgents(
    grpc::ServerContext* /*context*/,
    const agent_communication::FindAgentsRequest* request,
    agent_communication::FindAgentsResponse* response) {

    if (!AuthInterceptor::isAuthenticated()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Valid authentication token required");
    }

    std::lock_guard<std::mutex> lock(agents_mutex_);
    int limit = request->limit();
    if (limit <= 0) limit = 100;

    // Collect candidate agent ids
    std::set<std::string> candidates;
    bool has_filter = false;

    if (!request->tag().empty()) {
        has_filter = true;
        auto it = tags_index_.find(request->tag());
        if (it != tags_index_.end()) {
            candidates = it->second;
        }
    }

    if (!request->skill().empty()) {
        has_filter = true;
        auto it = skills_index_.find(request->skill());
        if (it != skills_index_.end()) {
            if (candidates.empty() && !request->tag().empty()) {
                // tag matched but skill did not → intersection is empty
            } else if (!candidates.empty()) {
                // intersect with tag candidates
                std::set<std::string> intersection;
                for (const auto& id : it->second) {
                    if (candidates.count(id)) intersection.insert(id);
                }
                candidates = intersection;
            } else {
                candidates = it->second;
            }
        } else if (!candidates.empty()) {
            candidates.clear();
        }
    }

    int added = 0;
    for (const auto& pair : agents_) {
        if (added >= limit) break;

        if (has_filter && candidates.find(pair.first) == candidates.end()) {
            continue;
        }

        if (!request->keyword().empty() &&
            pair.second.service_name.find(request->keyword()) == std::string::npos) {
            continue;
        }

        auto* info = response->add_agents();
        info->set_service_name(pair.second.service_name);
        info->set_version(pair.second.version);
        info->set_host(pair.second.host);
        info->set_port(pair.second.port);
        for (const auto& t : pair.second.tags) {
            info->add_tags(t);
        }
        for (const auto& m : pair.second.metadata) {
            (*info->mutable_metadata())[m.first] = m.second;
        }
        for (const auto& s : pair.second.skills) {
            info->add_skills(s);
        }
        info->set_agent_card(pair.second.agent_card);
        added++;
    }

    response->set_total_count(added);
    auto* status = response->mutable_status();
    status->set_code(0);
    status->set_message("OK");
    return grpc::Status::OK;
}

HealthServiceImpl::HealthServiceImpl() = default;

void HealthServiceImpl::setHealthCheckHandler(common::HealthCheckHandler handler) {
    health_check_handler_ = handler;
}

grpc::Status HealthServiceImpl::Check(
    grpc::ServerContext* /*context*/,
    const agent_communication::common::HealthCheckRequest* /*request*/,
    agent_communication::common::HealthCheckResponse* response) {

    bool healthy = health_check_handler_ ? health_check_handler_() : true;
    response->set_status(healthy
        ? agent_communication::common::HealthCheckResponse::SERVING
        : agent_communication::common::HealthCheckResponse::NOT_SERVING);
    return grpc::Status::OK;
}

grpc::Status HealthServiceImpl::Watch(
    grpc::ServerContext* context,
    const agent_communication::common::HealthCheckRequest* /*request*/,
    grpc::ServerWriter<agent_communication::common::HealthCheckResponse>* writer) {

    while (!context->IsCancelled()) {
        agent_communication::common::HealthCheckResponse response;
        bool healthy = health_check_handler_ ? health_check_handler_() : true;
        response.set_status(healthy
            ? agent_communication::common::HealthCheckResponse::SERVING
            : agent_communication::common::HealthCheckResponse::NOT_SERVING);
        if (!writer->Write(response)) break;
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    return grpc::Status::OK;
}

} // namespace server
} // namespace agent_rpc
