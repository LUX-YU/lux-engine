#pragma once

#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/resource/AsyncWork.hpp>
#include <chrono>
#include <memory>
#include <vector>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <utility>

namespace lux::render::test
{
    // Test host only. Production Render borrows CpuScheduler and never links Engine.
    // Accepted continuations must be drained before their native owners are destroyed.
    class CpuFixture final
    {
    public:
        using BeforeWork = void (*)(void*) noexcept;
        static constexpr std::size_t notification_capacity = 128;

        explicit CpuFixture(std::size_t concurrency = 2, std::size_t capacity = 128) noexcept
            : owner_thread_(std::this_thread::get_id()), runtime_(create(concurrency, capacity)),
              concurrency_(concurrency), capacity_(capacity)
        {}

        ~CpuFixture() noexcept
        {
            requireOwner();
            reject(false);
            runtime_.requestStop();
            if (!runtime_.join()) fail("ExecutionRuntime join failed");
            if (hook_fixture_ == this) hook_fixture_ = nullptr;
            std::lock_guard lock(mutex_);
            if (count_ != 0) fail("undrained continuation at CpuFixture destruction");
        }

        CpuFixture(const CpuFixture&) = delete;
        CpuFixture& operator=(const CpuFixture&) = delete;
        CpuFixture(CpuFixture&&) = delete;
        CpuFixture& operator=(CpuFixture&&) = delete;

        [[nodiscard]] execution::CpuScheduler cpu() noexcept
        {
            return runtime_.cpu();
        }

        [[nodiscard]] execution::ExecutionRuntime& runtime() noexcept { return runtime_; }

        void reject(bool value) noexcept
        {
            if (!value)
            {
                hold_.store(false, std::memory_order_release);
                waitFor([&] { return completed_.load(std::memory_order_acquire) == blockers_.size(); });
                return;
            }
            if (hold_.exchange(true)) fail("overlapping rejection window");
            const auto previous = blockers_.size();
            for (std::size_t i = 0; i < concurrency_; ++i) blockWorker();
            waitFor([&] { return started_.load(std::memory_order_acquire) == previous + concurrency_; });
            for (std::size_t i = 0; i < capacity_; ++i) blockWorker();
        }

        // Install only while no CPU job is in flight. The hook runs on the real
        // ExecutionRuntime worker immediately before the requested CPU work.
        void setBeforeWork(void* owner, BeforeWork hook) noexcept
        {
            hook_owner_ = owner;
            hook_ = hook;
            hook_fixture_ = this;
        }

        // Called only by the existing instrumented Native preparation seam.
        static void runBeforeWork() noexcept
        {
            auto* fixture = hook_fixture_;
            if (fixture && fixture->hook_) fixture->hook_(fixture->hook_owner_);
        }

        [[nodiscard]] CompletionPost post() noexcept
        {
            return {this, [](void* owner, void* continuation, CpuResume resume, RenderResult<void> result) noexcept {
                static_cast<CpuFixture*>(owner)->publish(continuation, resume, std::move(result));
            }};
        }

        // Consume only the notifications present at entry, outside the queue
        // lock. A resumed operation may publish its next event for the next drain.
        [[nodiscard]] std::size_t drain() noexcept
        {
            requireOwner();
            std::size_t ready;
            {
                std::lock_guard lock(mutex_);
                ready = count_;
            }
            for (std::size_t i = 0; i < ready; ++i)
            {
                Event event;
                {
                    std::lock_guard lock(mutex_);
                    event = std::move(events_[head_]);
                    events_[head_] = {};
                    head_ = (head_ + 1) % notification_capacity;
                    --count_;
                }
                event.resume(event.continuation, std::move(event.result));
            }
            return ready;
        }

    private:
        struct BlockReceiver final
        {
            using receiver_concept = stdexec::receiver_t;
            CpuFixture* owner;

            [[nodiscard]] stdexec::env<> get_env() const noexcept { return {}; }

            void set_value() && noexcept
            {
                owner->started_.fetch_add(1, std::memory_order_release);
                waitFor([&] { return !owner->hold_.load(std::memory_order_acquire); });
                owner->completed_.fetch_add(1, std::memory_order_release);
            }

            void set_error(execution::EExecutionError) && noexcept { fail("capacity blocker rejected"); }
            void set_stopped() && noexcept { fail("capacity blocker stopped"); }
        };

        using BlockOperation = stdexec::connect_result_t<
            decltype(stdexec::schedule(execution::CpuScheduler{})), BlockReceiver
        >;

        void blockWorker() noexcept
        {
            auto operation = std::unique_ptr<BlockOperation>(new BlockOperation(
                stdexec::connect(stdexec::schedule(runtime_.cpu()), BlockReceiver{this})
            ));
            auto* stable = operation.get();
            blockers_.push_back(std::move(operation));
            stdexec::start(*stable);
        }

        template<class Predicate> static void waitFor(Predicate predicate) noexcept
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!predicate())
            {
                if (std::chrono::steady_clock::now() >= deadline) fail("CPU control timeout");
                std::this_thread::yield();
            }
        }

        struct Event final
        {
            void* continuation = nullptr;
            CpuResume resume = nullptr;
            RenderResult<void> result;
        };

        [[nodiscard]] static execution::ExecutionRuntime create(std::size_t concurrency, std::size_t capacity) noexcept
        {
            auto created = execution::ExecutionRuntime::create({concurrency, capacity, 128, {128}});
            if (!created) fail("ExecutionRuntime creation failed");
            return std::move(*created);
        }

        void publish(void* continuation, CpuResume resume, RenderResult<void> result) noexcept
        {
            std::lock_guard lock(mutex_);
            if (count_ == notification_capacity) fail("notification reservation exceeded 128 slots");
            const auto tail = (head_ + count_) % notification_capacity;
            events_[tail] = {continuation, resume, std::move(result)};
            ++count_;
        }

        void requireOwner() const noexcept
        {
            if (std::this_thread::get_id() != owner_thread_) fail("wrong CpuFixture owner thread");
        }

        [[noreturn]] static void fail(const char* message) noexcept
        {
            std::fprintf(stderr, "CpuFixture: %s\n", message);
            std::fflush(stderr);
            std::_Exit(1);
        }

        const std::thread::id owner_thread_;
        execution::ExecutionRuntime runtime_;
        const std::size_t concurrency_, capacity_;
        std::atomic<bool> hold_{false};
        std::atomic<std::size_t> started_{0}, completed_{0};
        // Retain connected states until Host join, including the receiver tail.
        std::vector<std::unique_ptr<BlockOperation>> blockers_;
        void* hook_owner_ = nullptr;
        BeforeWork hook_ = nullptr;
        inline static CpuFixture* hook_fixture_ = nullptr;
        std::mutex mutex_;
        std::array<Event, notification_capacity> events_;
        std::size_t head_ = 0;
        std::size_t count_ = 0;
    };
}
