#pragma once

#include <lux/engine/process/ExecutionRuntime.hpp>
#include <lux/engine/render/resource/CpuSchedule.hpp>
#include <exec/variant_sender.hpp>

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
    // Test host only. Production Render borrows CpuSchedule and never links Engine.
    // Accepted continuations must be drained before their native owners are destroyed.
    class CpuFixture final
    {
    public:
        using BeforeWork = void (*)(void*) noexcept;
        static constexpr std::size_t notification_capacity = 128;

        explicit CpuFixture(std::size_t concurrency = 2, std::size_t capacity = 128) noexcept
            : owner_thread_(std::this_thread::get_id()), runtime_(create(concurrency, capacity)),
              scheduler_{runtime_.cpu(), &control_}
        {}

        ~CpuFixture() noexcept
        {
            requireOwner();
            runtime_.requestStop();
            if (!runtime_.join()) fail("ExecutionRuntime join failed");
            std::lock_guard lock(mutex_);
            if (count_ != 0) fail("undrained continuation at CpuFixture destruction");
        }

        CpuFixture(const CpuFixture&) = delete;
        CpuFixture& operator=(const CpuFixture&) = delete;
        CpuFixture(CpuFixture&&) = delete;
        CpuFixture& operator=(CpuFixture&&) = delete;

        [[nodiscard]] CpuSchedule cpu() noexcept
        {
            return CpuSchedule::bind<mapError>(scheduler_);
        }

        void reject(bool value) noexcept
        {
            control_.reject.store(value, std::memory_order_release);
        }

        // Install only while no CPU job is in flight. The hook runs on the real
        // ExecutionRuntime worker immediately before the requested CPU work.
        void setBeforeWork(void* owner, BeforeWork hook) noexcept
        {
            control_.hook_owner = owner;
            control_.hook = hook;
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
        struct Control final
        {
            std::atomic<bool> reject{false};
            void* hook_owner = nullptr;
            BeforeWork hook = nullptr;
        };

        struct Scheduler final
        {
            process::CpuScheduler real;
            Control* control;

            [[nodiscard]] auto schedule() const noexcept
            {
                auto normal = stdexec::then(stdexec::schedule(real), [control = control]() noexcept {
                    if (control->hook) control->hook(control->hook_owner);
                });
                auto rejected = stdexec::just_error(process::EExecutionError::CAPACITY_EXCEEDED);
                using VSchedule = exec::variant_sender<decltype(normal), decltype(rejected)>;
                // Controlled rejection is an explicit test branch. Normal work
                // always uses the actual Engine scheduler, including its genuine
                // queue-capacity and stopped errors; there is no second worker.
                if (control->reject.load(std::memory_order_acquire)) return VSchedule{std::move(rejected)};
                return VSchedule{std::move(normal)};
            }

            [[nodiscard]] bool operator==(const Scheduler&) const noexcept = default;
        };

        struct Event final
        {
            void* continuation = nullptr;
            CpuResume resume = nullptr;
            RenderResult<void> result;
        };

        [[nodiscard]] static RenderError mapError(process::EExecutionError error) noexcept
        {
            return {kCpuScheduleFailure, {static_cast<std::uint64_t>(error)}};
        }

        [[nodiscard]] static process::ExecutionRuntime create(std::size_t concurrency, std::size_t capacity) noexcept
        {
            auto created = process::ExecutionRuntime::create({concurrency, capacity, 128, {128}});
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
        process::ExecutionRuntime runtime_;
        Control control_;
        Scheduler scheduler_;
        std::mutex mutex_;
        std::array<Event, notification_capacity> events_;
        std::size_t head_ = 0;
        std::size_t count_ = 0;
    };
}
