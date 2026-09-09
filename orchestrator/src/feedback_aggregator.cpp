#include "agent_rpc/orchestrator/feedback_aggregator.h"
#include "agent_rpc/common/agent_runtime_repository.h"
#include "agent_rpc/common/logger.h"

#include <string>

namespace agent_rpc {
namespace orchestrator {

agent_rpc::common::AgentRuntimeRepository* FeedbackAggregator::runtime_repository_ = nullptr;

void FeedbackAggregator::setRuntimeRepository(
    agent_rpc::common::AgentRuntimeRepository* repository) {
    runtime_repository_ = repository;
}

void FeedbackAggregator::recalculate() {
    if (runtime_repository_ == nullptr) {
        LOG_WARN("FeedbackAggregator::recalculate skipped: runtime repository not initialized");
        return;
    }

    LOG_INFO("FeedbackAggregator::recalculate — aggregating owner-scoped route quality from feedback");

    int updated_count = 0;
    try {
        // Every distinct (owner, agent, skill) triple gets its own quality
        // row; one owner's feedback can never shift another owner's routing
        // weights. aggregateRouteQuality applies the Beta(2,2) prior.
        for (const auto& key : runtime_repository_->listFeedbackKeys()) {
            if (runtime_repository_->aggregateRouteQuality(
                    key.owner_id, key.agent_id, key.skill_name)) {
                ++updated_count;
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN(std::string("FeedbackAggregator::recalculate failed: ") + e.what());
        return;
    }

    LOG_INFO("FeedbackAggregator::recalculate — refreshed " +
             std::to_string(updated_count) + " owner/agent/skill quality rows in PostgreSQL");
}

} // namespace orchestrator
} // namespace agent_rpc
