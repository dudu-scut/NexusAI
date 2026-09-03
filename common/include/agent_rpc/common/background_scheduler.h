#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <list>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace agent_rpc {
namespace common {

class BackgroundScheduler {
public:
    using Task = std::function<void()>;

    static BackgroundScheduler& instance() {
        static BackgroundScheduler s;
        return s;
    }

    struct ScheduledTask {
        int id;
        std::string name;
        Task fn;
        std::chrono::milliseconds interval;
        std::chrono::steady_clock::time_point next_run;
        std::atomic<bool> running{false};
        std::atomic<bool> cancelled{false};
    };

    int scheduleAtFixedRate(std::string name, Task fn,
                            std::chrono::milliseconds interval) {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        int id = next_id_++;
        tasks_.emplace_back();
        auto& t = tasks_.back();
        t.id = id;
        t.name = std::move(name);
        t.fn = std::move(fn);
        t.interval = interval;
        t.next_run = std::chrono::steady_clock::now() + interval;
        return id;
    }

    void cancel(int task_id) {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        for (auto& t : tasks_) {
            if (t.id == task_id) {
                t.cancelled.store(true);
            }
        }
    }

    void start(size_t worker_count = 2) {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return;

        workers_.reserve(worker_count);
        for (size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this]() { workerLoop(); });
        }
        coordinator_ = std::thread([this]() { coordinatorLoop(); });
    }

    void stop() {
        running_.store(false);
        queue_cv_.notify_all();

        if (coordinator_.joinable()) coordinator_.join();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
        workers_.clear();

        // Clear all state so the singleton is clean for the next use.
        // All threads are joined, so no one races with these clears.
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            tasks_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            ready_queue_ = {};
        }
    }

private:
    BackgroundScheduler() = default;

    void coordinatorLoop() {
        while (running_.load()) {
            {
                std::lock_guard<std::mutex> lock(tasks_mutex_);
                auto now = std::chrono::steady_clock::now();
                for (auto& t : tasks_) {
                    if (now >= t.next_run && !t.running.load() && !t.cancelled.load()) {
                        // Claim the task here — before it even queues — so a
                        // task that waits longer than one interval (all
                        // workers busy) cannot be enqueued twice and then run
                        // concurrently on two workers.
                        t.running.store(true);
                        t.next_run = now + t.interval;
                        {
                            std::lock_guard<std::mutex> q_lock(queue_mutex_);
                            ready_queue_.push(&t);
                        }
                        queue_cv_.notify_one();
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void workerLoop() {
        while (running_.load()) {
            ScheduledTask* task = nullptr;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock, [this]() {
                    return !ready_queue_.empty() || !running_.load();
                });
                if (!running_.load()) return;
                if (!ready_queue_.empty()) {
                    task = ready_queue_.front();
                    ready_queue_.pop();
                }
            }
            if (task) {
                // Cancelled tasks may linger in ready_queue_ at cancel time.
                // The cancelled flag is checked here before execution instead of
                // draining the queue, which avoids a lock ordering hazard.
                // running was already claimed by the coordinator at enqueue.
                if (task->cancelled.load()) {
                    task->running.store(false);
                    continue;
                }
                try {
                    task->fn();
                } catch (...) {
                    // Log and swallow — don't crash the worker thread
                }
                task->running.store(false);
            }
        }
    }

    std::list<ScheduledTask> tasks_;
    std::mutex tasks_mutex_;
    int next_id_ = 0;

    std::queue<ScheduledTask*> ready_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    std::thread coordinator_;
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
};

}  // namespace common
}  // namespace agent_rpc
