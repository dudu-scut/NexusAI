#include <a2a/models/agent_message.hpp>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>

namespace a2a {

std::string AgentMessage::to_json() const {
    nlohmann::json j;
    j["messageId"] = message_id_;
    j["role"] = to_string(role_);
    if (context_id_) {
        j["contextId"] = *context_id_;
    }
    if (task_id_) {
        j["taskId"] = *task_id_;
    }
    nlohmann::json parts = nlohmann::json::array();
    for (const auto& part : parts_) {
        nlohmann::json part_json = nlohmann::json::parse(part->to_json(), nullptr, false);
        if (!part_json.is_discarded()) {
            parts.push_back(std::move(part_json));
        }
    }
    j["parts"] = std::move(parts);
    return j.dump();
}

AgentMessage AgentMessage::from_json(const std::string& json) {
    AgentMessage msg;

    nlohmann::json parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return msg;
    }

    // Accept camelCase (v1.0) or snake_case field names
    auto str_field = [&parsed](const char* camel, const char* snake) -> std::optional<std::string> {
        auto it = parsed.contains(camel) ? parsed.find(camel) : parsed.find(snake);
        if (it != parsed.end() && it->is_string()) {
            return it->get<std::string>();
        }
        return std::nullopt;
    };

    if (auto id = str_field("messageId", "message_id")) {
        msg.message_id_ = *id;
    }
    if (auto ctx = str_field("contextId", "context_id")) {
        msg.context_id_ = *ctx;
    }
    if (auto tid = str_field("taskId", "task_id")) {
        msg.task_id_ = *tid;
    }

    const auto role_it = parsed.find("role");
    if (role_it != parsed.end() && role_it->is_string()) {
        msg.role_ = message_role_from_string(role_it->get<std::string>());
    }

    const auto parts_it = parsed.find("parts");
    if (parts_it != parsed.end() && parts_it->is_array()) {
        for (const auto& part_json : *parts_it) {
            auto part = Part::from_json(part_json.dump());
            if (part) {
                msg.parts_.push_back(std::move(part));
            }
        }
    }

    return msg;
}

} // namespace a2a
