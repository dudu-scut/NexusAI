#include "agent_rpc/common/cost_tracker.h"
#include "agent_rpc/common/logger.h"
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace agent_rpc {
namespace common {

std::unordered_map<std::string, ModelPrice> CostTracker::pricing_;
std::once_flag CostTracker::pricing_init_;

CostTracker& CostTracker::instance() {
    static CostTracker ct;
    return ct;
}

void CostTracker::initPricing() {
    // DeepSeek models ($0.14/1M prompt, $0.28/1M completion → per-1k)
    pricing_["deepseek-v4-pro"] = {0.00014, 0.00028};
    pricing_["deepseek-v4"] = {0.00014, 0.00028};
    pricing_["deepseek-chat"] = {0.00014, 0.00028};

    // OpenAI models (per-1k pricing, derived from per-1M rates)
    pricing_["gpt-4"] = {0.03, 0.06};           // $30/1M prom, $60/1M comp
    pricing_["gpt-4-turbo"] = {0.01, 0.03};     // $10/1M prom, $30/1M comp
    pricing_["gpt-3.5-turbo"] = {0.0005, 0.0015};
    pricing_["gpt-4o"] = {0.0025, 0.01};

    // Default pricing for unknown models ($0.50/1M prom, $1.50/1M comp)
    pricing_["default"] = {0.0005, 0.0015};
}

void CostTracker::initialize(RedisClient* redis) {
    redis_ = redis;
    std::call_once(pricing_init_, initPricing);
}

double CostTracker::calculateCost(const std::string& model,
                                   int prompt_tokens,
                                   int completion_tokens) {
    ModelPrice price = getModelPrice(model);
    return (prompt_tokens / 1000.0) * price.prompt_price_per_1k +
           (completion_tokens / 1000.0) * price.completion_price_per_1k;
}

ModelPrice CostTracker::getModelPrice(const std::string& model) {
    std::call_once(pricing_init_, initPricing);
    auto it = pricing_.find(model);
    if (it != pricing_.end()) return it->second;
    return pricing_["default"];
}

int64_t CostTracker::toMicroDollars(double cost_usd) {
    return static_cast<int64_t>(std::llround(cost_usd * 1'000'000.0));
}

void CostTracker::recordLLMCall(
    const std::string& trace_id, const std::string& user_id,
    const std::string& context_id, const std::string& agent_id,
    const std::string& component, int prompt_tokens, int completion_tokens,
    const std::string& model, int64_t latency_ms) {

    (void)context_id;
    (void)agent_id;

    // Skip budget update for sandbox queries
    if (component == "sandbox") {
        LOG_DEBUG("CostTracker: skipping budget update for sandbox component");
        return;
    }

    double cost = calculateCost(model, prompt_tokens, completion_tokens);

    updateRedisBudget(user_id, cost);

    LOG_DEBUG("CostTracker: trace=" + trace_id + " component=" + component +
              " prompt=" + std::to_string(prompt_tokens) +
              " completion=" + std::to_string(completion_tokens) +
              " cost=$" + std::to_string(cost) +
              " latency=" + std::to_string(latency_ms) + "ms");
}

void CostTracker::updateRedisBudget(const std::string& user_id, double cost_usd) {
    if (!redis_) return;
    int64_t micro = toMicroDollars(cost_usd);

    std::time_t now = std::time(nullptr);
    std::tm tm_buf;
    localtime_r(&now, &tm_buf);
    std::ostringstream date_key;
    date_key << std::setfill('0')
             << (tm_buf.tm_year + 1900) << "-"
             << std::setw(2) << (tm_buf.tm_mon + 1) << "-"
             << std::setw(2) << tm_buf.tm_mday;

    // Sanitize user_id: replace ':' to prevent Redis key namespace injection
    std::string safe_user_id = user_id;
    for (auto& c : safe_user_id) {
        if (c == ':') c = '_';
    }

    std::string key = "cost:" + safe_user_id + ":" + date_key.str();
    int64_t new_total = 0;
    // The Redis key is an observability projection (budget enforcement is
    // PG-side); a fault here must at least be visible.
    if (!redis_->incrby(key, micro, new_total) || !redis_->expire(key, 90000)) {
        LOG_WARN("cost projection update failed for " + key);
    }
}

}  // namespace common
}  // namespace agent_rpc
