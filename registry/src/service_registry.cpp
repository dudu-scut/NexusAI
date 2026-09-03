#include "agent_rpc/registry/service_registry.h"
#include "agent_rpc/common/logger.h"
#include <curl/curl.h>
#include <json/json.h>
#include <sstream>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <cmath>

namespace agent_rpc {
namespace registry {

// Agent Live Metrics
namespace {
    std::unordered_map<std::string, AgentLiveMetrics> live_metrics_;
    std::mutex live_metrics_mutex_;
    const double kEMALatencyAlpha = 0.1;
    const size_t kSuccessBufferSize = 100;
    // Minimum recorded samples before metric-based health evaluation is
    // trusted (cold-start protection: a mostly-uninitialized ring buffer is
    // noise and must not take an agent down).
    const int kMinHealthSamples = 20;

    HealthStatus classifyHealth(double success_rate, double latency_ms) {
        if (success_rate >= 0.95 && latency_ms < 5000) {
            return HealthStatus::HEALTHY;
        }
        if (success_rate >= 0.80 && latency_ms < 30000) {
            return HealthStatus::DEGRADED;
        }
        return HealthStatus::UNHEALTHY;
    }
} // anonymous namespace

void ServiceRegistry::recordAgentCall(const std::string& agent_id,
                                      bool success, double latency_ms) {
    std::lock_guard<std::mutex> lock(live_metrics_mutex_);
    auto& m = live_metrics_[agent_id];

    // Circular buffer
    m.recent_results[m.buffer_idx] = success;
    m.buffer_idx = (m.buffer_idx + 1) % kSuccessBufferSize;
    m.total_writes++;

    // EMA latency
    if (m.ema_latency_ms == 0.0) {
        m.ema_latency_ms = latency_ms;
    } else {
        m.ema_latency_ms = (1.0 - kEMALatencyAlpha) * m.ema_latency_ms
                         + kEMALatencyAlpha * latency_ms;
    }

    m.last_heartbeat = std::chrono::steady_clock::now();
}

HealthStatus ServiceRegistry::evaluateHealth(const std::string& agent_id) {
    std::lock_guard<std::mutex> lock(live_metrics_mutex_);
    auto it = live_metrics_.find(agent_id);
    if (it == live_metrics_.end()) {
        return HealthStatus::UNKNOWN;
    }

    const auto& m = it->second;

    // Count successes in circular buffer
    int total = 0;
    int successes = 0;
    for (size_t i = 0; i < kSuccessBufferSize; ++i) {
        total += m.recent_results[i] ? 1 : 0; // bool→int implicitly, but we count entries
        // Actually we need to count all entries, not just true ones.
        // But we can't distinguish uninitialized=false from failure=false.
        // Instead count the number of entries written so far.
    }
    // A cleaner approach: count from buffer_idx backwards for entries that were written.
    // For simplicity, count only non-zero entries up to buffer_idx.
    // Actually, let's use a simpler method: for all 100 slots, if the slot was written to,
    // it's either true or false. Since we can't tell uninitialized from false,
    // we use a different metric: success rate across written entries using buffer_idx.
    // But buffer_idx wraps around. So we look at the full 100 - all are valid after 100 calls.

    // Use total_writes to determine actual valid entries after wrap-around
    int valid_count = (m.total_writes >= 100) ? 100 : m.buffer_idx;
    if (valid_count == 0) valid_count = 1; // avoid div by zero

    int success_count = 0;
    for (int i = 0; i < valid_count; ++i) {
        if (m.recent_results[i]) success_count++;
    }

    double success_rate = static_cast<double>(success_count) / valid_count;
    double latency = m.ema_latency_ms;

    // Classification
    return classifyHealth(success_rate, latency);
}

void ServiceRegistry::evaluateAllHealth() {
    // No-argument overload keeps the logging-only behavior; the callback
    // overload below shares the same evaluation loop.
    evaluateAllHealth(nullptr);
}

void ServiceRegistry::evaluateAllHealth(
    const std::function<void(const std::string&, HealthStatus)>& callback) {
    // Snapshot (agent_id, status, dashboard fields) under the metrics lock,
    // then invoke the callback AFTER releasing the lock. The callback is
    // expected to take router locks (agents_mutex_) — holding live_metrics_
    // mutex_ while doing so would create an AB-BA deadlock with any path
    // that takes router locks before calling recordAgentCall.
    struct Snapshot {
        std::string agent_id;
        HealthStatus status;
        double success_rate;
        double ema_latency_ms;
        int active_requests;
    };
    std::vector<Snapshot> snapshots;
    {
        std::lock_guard<std::mutex> lock(live_metrics_mutex_);
        for (const auto& pair : live_metrics_) {
            const auto& m = pair.second;
            // Cold-start protection: skip agents without enough samples.
            if (m.total_writes < kMinHealthSamples) {
                continue;
            }
            int valid_count = (m.total_writes >= 100) ? 100 : m.buffer_idx;
            if (valid_count <= 0) valid_count = 1;
            int success_count = 0;
            for (int i = 0; i < valid_count && i < 100; ++i) {
                if (m.recent_results[i]) success_count++;
            }
            double success_rate = static_cast<double>(success_count) / valid_count;
            snapshots.push_back(Snapshot{
                pair.first,
                classifyHealth(success_rate, m.ema_latency_ms),
                success_rate,
                m.ema_latency_ms,
                m.active_requests.load()});
        }
    }

    for (const auto& s : snapshots) {
        LOG_INFO("[HealthDashboard] Agent " + s.agent_id +
                 " status=" + (s.status == HealthStatus::HEALTHY ? "HEALTHY" :
                               s.status == HealthStatus::DEGRADED ? "DEGRADED" : "UNHEALTHY") +
                 " success_rate=" + std::to_string(s.success_rate) +
                 " ema_latency=" + std::to_string(s.ema_latency_ms) + "ms" +
                 " active_requests=" + std::to_string(s.active_requests));
        if (callback) {
            callback(s.agent_id, s.status);
        }
    }
}

void ServiceRegistry::recordHeartbeat(const std::string& agent_id) {
    std::lock_guard<std::mutex> lock(live_metrics_mutex_);
    live_metrics_[agent_id].last_heartbeat = std::chrono::steady_clock::now();
}

void ServiceRegistry::evaluateHeartbeatTimeouts(
    std::chrono::seconds timeout,
    const std::function<void(const std::string&, bool timed_out)>& callback) {
    if (!callback) {
        return;
    }

    std::vector<std::pair<std::string, bool>> results;
    {
        std::lock_guard<std::mutex> lock(live_metrics_mutex_);
        auto now = std::chrono::steady_clock::now();
        for (const auto& pair : live_metrics_) {
            const auto& m = pair.second;
            // Entries created by recordAgentCall before heartbeat wiring
            // existed always carry a heartbeat timestamp; treat the epoch
            // default (never stamped) as "not a timeout" to avoid killing
            // agents based on a missing timestamp.
            if (m.last_heartbeat == std::chrono::steady_clock::time_point{}) {
                continue;
            }
            bool timed_out = (now - m.last_heartbeat) > timeout;
            results.emplace_back(pair.first, timed_out);
        }
    }

    for (const auto& [agent_id, timed_out] : results) {
        callback(agent_id, timed_out);
    }
}

ServiceRegistry& ServiceRegistry::instance() {
    static MemoryServiceRegistry reg;
    return reg;
}

ConsulServiceRegistry::ConsulServiceRegistry() {
    // curl_global_init is now called once in server/src/main.cpp
}

ConsulServiceRegistry::~ConsulServiceRegistry() {
    stopHealthCheck();
    // curl_global_cleanup is now called once in server/src/main.cpp
}

bool ConsulServiceRegistry::initialize(const std::string& consul_address) {
    consul_address_ = consul_address;
    LOG_INFO("Consul service registry initialized with address: " + consul_address);
    return true;
}

bool ConsulServiceRegistry::registerService(const common::ServiceEndpoint& endpoint) {
    std::string service_id = getServiceId(endpoint);

    Json::Value service_json;
    service_json["ID"] = service_id;
    service_json["Name"] = endpoint.service_name;
    service_json["Address"] = endpoint.host;
    service_json["Port"] = endpoint.port;
    service_json["Tags"] = Json::Value(Json::arrayValue);
    service_json["Meta"] = Json::Value(Json::objectValue);

    for (const auto& pair : endpoint.metadata) {
        service_json["Meta"][pair.first] = pair.second;
    }

    Json::StreamWriterBuilder builder;
    std::string json_string = Json::writeString(builder, service_json);

    std::string url = "http://" + consul_address_ + "/v1/agent/service/register";
    std::string response = makeHttpRequest("PUT", url, json_string);

    if (response.empty()) {
        LOG_ERROR("Failed to register service: " + service_id);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(services_mutex_);
        registered_services_[service_id] = endpoint;
    }

    LOG_INFO("Service registered: " + service_id);
    return true;
}

bool ConsulServiceRegistry::unregisterService(const std::string& service_id) {
    std::string url = "http://" + consul_address_ + "/v1/agent/service/deregister/" + service_id;
    std::string response = makeHttpRequest("PUT", url);

    {
        std::lock_guard<std::mutex> lock(services_mutex_);
        registered_services_.erase(service_id);
    }

    LOG_INFO("Service unregistered: " + service_id);
    return true;
}

std::vector<common::ServiceEndpoint> ConsulServiceRegistry::discoverServices(const std::string& service_name) {
    std::string url = "http://" + consul_address_ + "/v1/health/service/" + service_name;
    std::string response = makeHttpRequest("GET", url);

    if (response.empty()) {
        LOG_ERROR("Failed to discover services: " + service_name);
        return {};
    }

    std::vector<common::ServiceEndpoint> services = parseServiceList(response);

    {
        std::lock_guard<std::mutex> lock(services_mutex_);
        discovered_services_[service_name] = services;
    }

    return services;
}

bool ConsulServiceRegistry::isServiceHealthy(const std::string& service_id) {
    std::string url = "http://" + consul_address_ + "/v1/agent/health/service/id/" + service_id;
    std::string response = makeHttpRequest("GET", url);

    if (response.empty()) {
        return false;
    }

    Json::Value root;
    Json::Reader reader;
    if (reader.parse(response, root)) {
        return root["Status"].asString() == "passing";
    }

    return false;
}

bool ConsulServiceRegistry::updateHeartbeat(const std::string& service_id) {
    std::string url = "http://" + consul_address_ + "/v1/agent/check/pass/service:" + service_id;
    std::string response = makeHttpRequest("PUT", url);

    return !response.empty();
}

void ConsulServiceRegistry::watchServices(const std::string& service_name,
                                        std::function<void(const std::vector<common::ServiceEndpoint>&)> callback) {
    std::lock_guard<std::mutex> lock(watchers_mutex_);
    watchers_[service_name] = callback;
}

void ConsulServiceRegistry::startHealthCheck() {
    if (health_check_running_) {
        return;
    }

    health_check_running_ = true;
    health_check_thread_ = std::thread([this]() {
        healthCheckLoop();
    });
}

void ConsulServiceRegistry::stopHealthCheck() {
    if (health_check_running_) {
        health_check_running_ = false;
        if (health_check_thread_.joinable()) {
            health_check_thread_.join();
        }
    }
}

std::string ConsulServiceRegistry::getServiceId(const common::ServiceEndpoint& endpoint) const {
    return endpoint.service_name + "-" + endpoint.host + "-" + std::to_string(endpoint.port);
}

void ConsulServiceRegistry::healthCheckLoop() {
    while (health_check_running_) {
        {
            std::lock_guard<std::mutex> lock(services_mutex_);
            for (const auto& pair : registered_services_) {
                updateHeartbeat(pair.first);
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
}

std::string ConsulServiceRegistry::makeHttpRequest(const std::string& method,
                                                  const std::string& url,
                                                  const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return "";
    }

    std::string response_data;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, [](void* contents, size_t size, size_t nmemb, std::string* data) {
        data->append((char*)contents, size * nmemb);
        return size * nmemb;
    });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    if (method == "PUT") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        }
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_ERROR("HTTP request failed: " + std::string(curl_easy_strerror(res)));
        return "";
    }

    return response_data;
}

std::vector<common::ServiceEndpoint> ConsulServiceRegistry::parseServiceList(const std::string& json_response) {
    std::vector<common::ServiceEndpoint> services;

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(json_response, root)) {
        LOG_ERROR("Failed to parse service list JSON");
        return services;
    }

    for (const auto& service : root) {
        common::ServiceEndpoint endpoint;
        endpoint.host = service["Service"]["Address"].asString();
        endpoint.port = service["Service"]["Port"].asInt();
        endpoint.service_name = service["Service"]["Name"].asString();
        endpoint.is_healthy = service["Checks"][0]["Status"].asString() == "passing";

        const auto& meta = service["Service"]["Meta"];
        for (const auto& key : meta.getMemberNames()) {
            endpoint.metadata[key] = meta[key].asString();
        }

        services.push_back(endpoint);
    }

    return services;
}

common::ServiceEndpoint ConsulServiceRegistry::parseServiceEndpoint(const std::string& json_service) {
    common::ServiceEndpoint endpoint;

    Json::Value root;
    Json::Reader reader;
    if (reader.parse(json_service, root)) {
        endpoint.host = root["Address"].asString();
        endpoint.port = root["Port"].asInt();
        endpoint.service_name = root["Name"].asString();

        const auto& meta = root["Meta"];
        for (const auto& key : meta.getMemberNames()) {
            endpoint.metadata[key] = meta[key].asString();
        }
    }

    return endpoint;
}

EtcdServiceRegistry::EtcdServiceRegistry() = default;

EtcdServiceRegistry::~EtcdServiceRegistry() {
    if (watch_running_) {
        watch_running_ = false;
        if (watch_thread_.joinable()) {
            watch_thread_.join();
        }
    }
}

bool EtcdServiceRegistry::initialize(const std::string& etcd_address) {
    etcd_address_ = etcd_address;
    LOG_INFO("Etcd service registry initialized with address: " + etcd_address);
    return true;
}

bool EtcdServiceRegistry::registerService(const common::ServiceEndpoint& endpoint) {
    // Simplified etcd registration implementation
    std::string service_key = "/services/" + endpoint.service_name + "/" +
                             endpoint.host + ":" + std::to_string(endpoint.port);

    Json::Value service_json;
    service_json["host"] = endpoint.host;
    service_json["port"] = endpoint.port;
    service_json["service_name"] = endpoint.service_name;
    service_json["version"] = endpoint.version;
    service_json["metadata"] = Json::Value(Json::objectValue);

    for (const auto& pair : endpoint.metadata) {
        service_json["metadata"][pair.first] = pair.second;
    }

    Json::StreamWriterBuilder builder;
    std::string json_string = Json::writeString(builder, service_json);

    std::string response = makeEtcdRequest("PUT", service_key, json_string);

    {
        std::lock_guard<std::mutex> lock(services_mutex_);
        registered_services_[service_key] = endpoint;
    }

    LOG_INFO("Service registered in etcd: " + service_key);
    return !response.empty();
}

bool EtcdServiceRegistry::unregisterService(const std::string& service_id) {
    std::string service_key = "/services/" + service_id;
    std::string response = makeEtcdRequest("DELETE", service_key);

    {
        std::lock_guard<std::mutex> lock(services_mutex_);
        registered_services_.erase(service_key);
    }

    LOG_INFO("Service unregistered from etcd: " + service_key);
    return !response.empty();
}

std::vector<common::ServiceEndpoint> EtcdServiceRegistry::discoverServices(const std::string& service_name) {
    std::string service_prefix = "/services/" + service_name + "/";
    std::string response = makeEtcdRequest("GET", service_prefix);

    std::vector<common::ServiceEndpoint> services = parseEtcdResponse(response);

    {
        std::lock_guard<std::mutex> lock(services_mutex_);
        discovered_services_[service_name] = services;
    }

    return services;
}

bool EtcdServiceRegistry::isServiceHealthy(const std::string& service_id) {
    // Simplified health check implementation
    return true;
}

bool EtcdServiceRegistry::updateHeartbeat(const std::string& service_id) {
    // Simplified heartbeat implementation
    return true;
}

void EtcdServiceRegistry::watchServices(const std::string& service_name,
                                       std::function<void(const std::vector<common::ServiceEndpoint>&)> callback) {
    std::lock_guard<std::mutex> lock(watchers_mutex_);
    watchers_[service_name] = callback;
}

void EtcdServiceRegistry::watchLoop() {
    // Simplified watch implementation
    while (watch_running_) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }
}

std::string EtcdServiceRegistry::makeEtcdRequest(const std::string& method,
                                                const std::string& key,
                                                const std::string& value) {
    // Etcd registry is not implemented; throw an explicit error instead of
    // silently returning "OK" which masks the failure.
    throw std::runtime_error(
        "Etcd service registry is not implemented. "
        "Use ConsulServiceRegistry or MemoryServiceRegistry instead.");
}

std::vector<common::ServiceEndpoint> EtcdServiceRegistry::parseEtcdResponse(const std::string& response) {
    // Simplified response parsing implementation
    return {};
}

bool MemoryServiceRegistry::registerService(const common::ServiceEndpoint& endpoint) {
    std::string service_id = endpoint.host + ":" + std::to_string(endpoint.port);
    std::vector<common::ServiceEndpoint> snapshot;
    std::function<void(const std::vector<common::ServiceEndpoint>&)> watcher;

    {
        std::lock_guard<std::mutex> lock(services_mutex_);
        services_[service_id] = endpoint;
        for (const auto& pair : services_) {
            if (pair.second.service_name == endpoint.service_name) {
                snapshot.push_back(pair.second);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(watchers_mutex_);
        auto it = watchers_.find(endpoint.service_name);
        if (it != watchers_.end()) {
            watcher = it->second;
        }
    }

    LOG_INFO("Service registered in memory: " + service_id);
    if (watcher) {
        watcher(snapshot);
    }
    return true;
}

bool MemoryServiceRegistry::unregisterService(const std::string& service_id) {
    std::string service_name;
    std::vector<common::ServiceEndpoint> snapshot;
    std::function<void(const std::vector<common::ServiceEndpoint>&)> watcher;

    {
        std::lock_guard<std::mutex> lock(services_mutex_);

        auto it = services_.find(service_id);
        if (it == services_.end()) {
            return false;
        }

        service_name = it->second.service_name;
        services_.erase(it);
        for (const auto& pair : services_) {
            if (pair.second.service_name == service_name) {
                snapshot.push_back(pair.second);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(watchers_mutex_);
        auto it = watchers_.find(service_name);
        if (it != watchers_.end()) {
            watcher = it->second;
        }
    }

    LOG_INFO("Service unregistered from memory: " + service_id);
    if (watcher) {
        watcher(snapshot);
    }
    return true;
}

std::vector<common::ServiceEndpoint> MemoryServiceRegistry::discoverServices(const std::string& service_name) {
    std::lock_guard<std::mutex> lock(services_mutex_);

    std::vector<common::ServiceEndpoint> result;
    for (const auto& pair : services_) {
        if (pair.second.service_name == service_name) {
            result.push_back(pair.second);
        }
    }

    return result;
}

bool MemoryServiceRegistry::isServiceHealthy(const std::string& service_id) {
    std::lock_guard<std::mutex> lock(services_mutex_);

    auto it = services_.find(service_id);
    return it != services_.end() && it->second.is_healthy;
}

bool MemoryServiceRegistry::updateHeartbeat(const std::string& service_id) {
    std::lock_guard<std::mutex> lock(services_mutex_);

    auto it = services_.find(service_id);
    if (it != services_.end()) {
        it->second.last_heartbeat = std::chrono::steady_clock::now();
        return true;
    }

    return false;
}

void MemoryServiceRegistry::watchServices(const std::string& service_name,
                                        std::function<void(const std::vector<common::ServiceEndpoint>&)> callback) {
    std::lock_guard<std::mutex> lock(watchers_mutex_);
    watchers_[service_name] = callback;
}

} // namespace registry
} // namespace agent_rpc
