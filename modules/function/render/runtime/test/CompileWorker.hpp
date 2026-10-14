#pragma once
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

// Host fixture only: one fixed cold worker, bounded task storage, no IPC or Runtime worker creation.
class CompileWorker final
{
public:
    CompileWorker() : worker_([this] { work(); }) {}
    ~CompileWorker()
    {
        join();
        { std::lock_guard lock(mutex_); stopping_ = true; }
        available_.notify_one();
        worker_.join();
    }
    lux::render::RenderServerConfig configure(lux::render::RenderServerConfig config)
    {
        config.compile_user = this;
        config.enqueue_compile = [](void* user, void (*run)(void*) noexcept, void* value) noexcept {
            return static_cast<CompileWorker*>(user)->submit(run, value);
        };
        config.join_compile = [](void* user) noexcept { static_cast<CompileWorker*>(user)->join(); };
        return config;
    }
    void reject(bool value) noexcept { reject_.store(value); }
private:
    struct Task { void (*run)(void*) noexcept; void* value; };
    bool submit(void (*run)(void*) noexcept, void* value) noexcept
    {
        if (reject_.load()) return false;
        {
            std::lock_guard lock(mutex_);
            if (stopping_ || count_ == tasks_.size()) return false;
            tasks_[(first_ + count_) % tasks_.size()] = {run, value};
            ++count_;
            ++outstanding_;
        }
        available_.notify_one();
        return true;
    }
    void join() noexcept
    {
        std::unique_lock lock(mutex_);
        settled_.wait(lock, [this] { return outstanding_ == 0; });
    }
    void work() noexcept
    {
        for (;;)
        {
            Task task;
            {
                std::unique_lock lock(mutex_);
                available_.wait(lock, [this] { return stopping_ || count_ != 0; });
                if (stopping_ && count_ == 0) return;
                task = tasks_[first_];
                first_ = (first_ + 1) % tasks_.size();
                --count_;
            }
            task.run(task.value);
            {
                std::lock_guard lock(mutex_);
                --outstanding_;
            }
            settled_.notify_all();
        }
    }
    std::mutex mutex_;
    std::condition_variable available_, settled_;
    std::array<Task, 4> tasks_{};
    std::size_t first_ = 0, count_ = 0, outstanding_ = 0;
    bool stopping_ = false;
    std::atomic<bool> reject_{false};
    std::thread worker_;
};

inline lux::render::RenderResult<std::unique_ptr<lux::render::RenderServer>>
createTestServer(lux::render::RenderServerConfig config)
{
    // Existing behavioral fixtures use servers sequentially, with run joined before the next publisher starts.
    static CompileWorker worker;
    return lux::render::RenderServer::create(worker.configure(std::move(config)));
}
