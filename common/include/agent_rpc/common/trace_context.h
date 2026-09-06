#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace agent_rpc {
namespace common {

struct Span {
    std::string name;
    std::string span_id;
    std::string parent_span_id;
    std::string component;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point end_time;
    int duration_ms = 0;
    std::string status = "ok";
    std::string error_message;
    std::string metadata_json;
};

class TraceContext {
public:
    TraceContext(const std::string& user_id, const std::string& context_id)
        : trace_id_(generateUUID()), user_id_(user_id), context_id_(context_id) {}

    static void init(const std::string& user_id, const std::string& context_id) {
        auto& tls = threadInstance();
        tls.user_id_ = user_id;
        tls.context_id_ = context_id;
        tls.trace_id_ = generateUUID();
        tls.spans_.clear();
        tls.span_stack_.clear();
        tls.depth_ = 0;
    }

    // Cross-thread propagation overload: when existing_trace_id is non-empty
    // the caller's trace id is reused so child threads attach to the same
    // logical trace; when empty, behaves like the two-argument init and
    // generates a fresh trace id. Independent overload on purpose — the
    // two-argument version above is a byte-stable contract asserted by
    // source-scanning tests and must keep its exact behavior/text.
    static void init(const std::string& user_id, const std::string& context_id,
                     const std::string& existing_trace_id) {
        auto& tls = threadInstance();
        tls.user_id_ = user_id;
        tls.context_id_ = context_id;
        tls.trace_id_ = existing_trace_id.empty() ? generateUUID() : existing_trace_id;
        tls.spans_.clear();
        tls.span_stack_.clear();
        tls.depth_ = 0;
    }

    static TraceContext* current() {
        return &threadInstance();
    }

    // RAII pairing for startSpan/endSpan: guarantees the span is closed and
    // the delegation-depth counter restored on every exit path, including
    // exceptions thrown between the manual start/end pair (the A2A adapter's
    // failure paths used to leak the agent_call span, which desynchronized
    // the caller's span stack and let depth grow per failed attempt).
    class SpanGuard {
    public:
        SpanGuard(const std::string& name, const std::string& component) {
            ctx_ = TraceContext::current();
            if (ctx_) {
                depth_before_ = ctx_->depth();
                ctx_->startSpan(name, component);
            }
        }
        ~SpanGuard() {
            if (ctx_) {
                ctx_->endSpan();
                ctx_->setDepth(depth_before_);
            }
        }
        SpanGuard(const SpanGuard&) = delete;
        SpanGuard& operator=(const SpanGuard&) = delete;

    private:
        TraceContext* ctx_ = nullptr;
        int depth_before_ = 0;
    };

    const std::string& traceId() const { return trace_id_; }
    const std::string& userId() const { return user_id_; }

    // Span management
    void startSpan(const std::string& name, const std::string& component) {
        Span s;
        s.name = name;
        s.component = component;
        s.span_id = generateUUID();
        s.parent_span_id = span_stack_.empty() ? "" : span_stack_.back();
        s.start_time = std::chrono::steady_clock::now();
        spans_.push_back(std::move(s));
        span_stack_.push_back(spans_.back().span_id);
    }

    void endSpan() {
        if (span_stack_.empty()) return;
        // Pop FIRST (by value): the exporter below may re-enter init/
        // startSpan or throw — neither may desynchronize the span stack
        // (endSpan runs inside SpanGuard's destructor, which must not
        // unwind an exception either).
        const std::string span_id = span_stack_.back();
        span_stack_.pop_back();
        for (auto it = spans_.rbegin(); it != spans_.rend(); ++it) {
            if (it->span_id == span_id) {
                it->end_time = std::chrono::steady_clock::now();
                it->duration_ms = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        it->end_time - it->start_time).count());
                // Notify global span exporter (used by span_batch_flush task)
                SpanExporter exporter_copy;
                {
                    std::lock_guard<std::mutex> lock(exporterMutex());
                    exporter_copy = spanExporter();
                }
                if (exporter_copy) {
                    try {
                        exporter_copy(*it, trace_id_, user_id_);
                    } catch (...) {
                        // Telemetry is best-effort; never let it escape
                        // (SpanGuard's destructor would terminate).
                    }
                }
                break;
            }
        }
    }

    int currentDepth() const { return static_cast<int>(span_stack_.size()); }
    int depth() const { return depth_; }
    void incrementDepth() { depth_++; }
    void setDepth(int d) { depth_ = d; }

    std::string newChildSpanId() const { return generateUUID(); }

    const std::vector<Span>& completedSpans() const { return spans_; }
    std::vector<Span>& mutableSpans() { return spans_; }

    // Generate human-readable trace summary
    std::string traceSummary() const {
        std::ostringstream oss;
        for (size_t i = 0; i < spans_.size(); ++i) {
            if (i > 0) oss << " -> ";
            oss << spans_[i].name << " " << spans_[i].duration_ms << "ms";
        }
        return oss.str();
    }

    // Global span export callback: (span, trace_id, user_id) -> void
    using SpanExporter = std::function<void(const Span&, const std::string&, const std::string&)>;
    static void setSpanExporter(SpanExporter fn) {
        std::lock_guard<std::mutex> lock(exporterMutex());
        spanExporter() = std::move(fn);
    }

private:
    static SpanExporter& spanExporter() {
        static SpanExporter fn;
        return fn;
    }
    static std::mutex& exporterMutex() {
        static std::mutex m;
        return m;
    }

    static TraceContext& threadInstance() {
        thread_local TraceContext ctx("", "");
        return ctx;
    }

    static std::string generateUUID() {
        // Platform-independent UUID v4 generation
        // Uses simple random hex -- sufficient for tracing, not security-critical
        static thread_local std::mt19937_64 rng(
            std::chrono::steady_clock::now().time_since_epoch().count() ^
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
        static thread_local std::uniform_int_distribution<uint64_t> dist;

        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        uint64_t a = dist(rng);
        uint64_t b = dist(rng);
        oss << std::setw(8) << ((a >> 32) & 0xFFFFFFFF)
            << "-" << std::setw(4) << ((a >> 16) & 0xFFFF)
            << "-4" << std::setw(3) << (a & 0xFFF)   // version 4
            << "-" << std::setw(4) << (((b >> 48) & 0x3FFF) | 0x8000) // variant
            << "-" << std::setw(12) << (b & 0xFFFFFFFFFFFFULL);
        return oss.str();
    }

    std::string trace_id_;
    std::string user_id_;
    std::string context_id_;
    std::vector<Span> spans_;
    std::vector<std::string> span_stack_;  // stack of span_ids for parent tracking
    int depth_ = 0;  // delegation depth counter
};

}  // namespace common
}  // namespace agent_rpc
