#pragma once
/**
 * @file detail/MainThreadMailbox.hpp
 * @brief stdexec-free main-thread completion queue and wake binding.
 */

#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/runtime/execution/AsyncStatistics.hpp>

#include <moodycamel/concurrentqueue.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

namespace lux::exec
{
    class MainThreadMailbox
    {
    public:
        using Task = lux::cxx::move_only_function<void()>;

        struct Statistics final
        {
            std::size_t depth{0u};
            std::size_t high_water{0u};
            std::uint64_t oldest_age_ns{0u};
            std::uint64_t adoption_samples{0u};
            std::uint64_t adoption_total_ns{0u};
            std::uint64_t adoption_max_ns{0u};
            AsyncLatencyHistogram adoption_histogram{};
        };

        struct WakeBinding final
        {
            std::shared_ptr<void> owner;
            void (*notify)(void*) noexcept{nullptr};

            void operator()() const noexcept
            {
                if (owner && notify)
                    notify(owner.get());
            }
        };

        static_assert(
            std::atomic<const WakeBinding*>::is_always_lock_free,
            "MainThreadMailbox producer wake observation must remain lock-free");

        void enqueue(Task task)
        {
            const auto enqueued_ns = nowNs();
            if (!queue_.enqueue(QueuedTask{std::move(task), enqueued_ns}))
                std::abort();
            const auto previous_depth = depth_.fetch_add(
                1u,
                std::memory_order_acq_rel
            );
            updateHighWater(previous_depth + 1u);
            if (previous_depth == 0u)
                oldest_enqueued_ns_.store(
                    enqueued_ns,
                    std::memory_order_release
                );
            work_epoch_.fetch_add(1u, std::memory_order_release);
            work_epoch_.notify_one();
            const auto* wake = external_wake_.load(
                std::memory_order_acquire);
            if (wake)
                (*wake)();
        }

        void bindExternalWake(
            std::shared_ptr<const WakeBinding> binding) noexcept
        {
            if (!binding)
            {
                external_wake_.store(nullptr, std::memory_order_release);
                return;
            }
            const auto* active = binding.get();

            // Binding is a composition-owner cold path. Keep every distinct
            // pointee alive until MainThreadMailbox itself is destroyed, so
            // producer enqueue needs only a lock-free raw observer load. Retaining old
            // bindings also prevents allocator address reuse and therefore
            // closes the ABA window during bind/unbind races.
            bool retained = false;
            for (const auto& current : retained_external_wakes_)
            {
                if (current.get() == binding.get())
                {
                    retained = true;
                    break;
                }
            }
            if (!retained)
                retained_external_wakes_.push_back(std::move(binding));
            external_wake_.store(active, std::memory_order_release);
        }

        void unbindExternalWake(
            const std::shared_ptr<const WakeBinding>& binding) noexcept
        {
            const auto* current = binding.get();
            (void)external_wake_.compare_exchange_strong(
                current,
                nullptr,
                std::memory_order_acq_rel,
                std::memory_order_acquire);
        }

        std::size_t pump(std::size_t max_n)
        {
            struct PumpGuard final
            {
                std::size_t& depth;
                explicit PumpGuard(std::size_t& value) noexcept : depth(value)
                {
                    ++depth;
                }
                ~PumpGuard() noexcept { --depth; }
            } guard{pump_depth_};

            std::size_t ran = 0;
            while (ran < max_n)
            {
                QueuedTask queued;
                if (!queue_.try_dequeue(queued))
                    break;
                const auto adoption_ns = nowNs() - queued.enqueued_ns;
                adoption_samples_.fetch_add(1u, std::memory_order_relaxed);
                adoption_total_ns_.fetch_add(
                    adoption_ns,
                    std::memory_order_relaxed
                );
                updateMax(adoption_max_ns_, adoption_ns);
                if (histograms_enabled_)
                {
                    adoption_histogram_[detail::asyncLatencyBucket(adoption_ns)]
                        .fetch_add(1u, std::memory_order_relaxed);
                }
                const auto previous_depth = depth_.fetch_sub(
                    1u,
                    std::memory_order_acq_rel
                );
                if (previous_depth == 1u)
                    oldest_enqueued_ns_.store(0u, std::memory_order_release);
                if (queued.task)
                    queued.task();
                ++ran;
            }
            return ran;
        }

        [[nodiscard]] bool isPumping() const noexcept
        {
            return pump_depth_ != 0;
        }

        [[nodiscard]] bool emptyApprox() const noexcept
        {
            return depth_.load(std::memory_order_acquire) == 0u;
        }

        [[nodiscard]] Statistics statistics() const noexcept
        {
            const auto oldest = oldest_enqueued_ns_.load(
                std::memory_order_acquire
            );
            Statistics result{
                .depth = depth_.load(std::memory_order_acquire),
                .high_water = high_water_.load(std::memory_order_relaxed),
                // The timestamp is deliberately conservative while a drain
                // leaves queued work behind: it may overstate age, never hide
                // a starved main-thread adoption.
                .oldest_age_ns = oldest == 0u ? 0u : nowNs() - oldest,
                .adoption_samples = adoption_samples_.load(
                    std::memory_order_relaxed),
                .adoption_total_ns = adoption_total_ns_.load(
                    std::memory_order_relaxed),
                .adoption_max_ns = adoption_max_ns_.load(
                    std::memory_order_relaxed),
            };
            for (std::size_t index = 0u;
                 index < result.adoption_histogram.size();
                 ++index)
            {
                result.adoption_histogram[index] =
                    adoption_histogram_[index].load(
                        std::memory_order_relaxed);
            }
            return result;
        }

        void enableLatencyHistograms(bool enabled) noexcept
        {
            histograms_enabled_ = enabled;
        }

        [[nodiscard]] std::uint64_t workEpoch() const noexcept
        {
            return work_epoch_.load(std::memory_order_acquire);
        }

        void waitForWork(std::uint64_t observed) const noexcept
        {
            work_epoch_.wait(observed, std::memory_order_acquire);
        }

    private:
        struct QueuedTask final
        {
            Task task;
            std::uint64_t enqueued_ns{0u};
        };

        [[nodiscard]] static std::uint64_t nowNs() noexcept
        {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count()
            );
        }

        void updateHighWater(std::size_t value) noexcept
        {
            auto high = high_water_.load(std::memory_order_relaxed);
            while (high < value &&
                   !high_water_.compare_exchange_weak(
                       high,
                       value,
                       std::memory_order_relaxed,
                       std::memory_order_relaxed))
            {}
        }

        static void updateMax(
            std::atomic<std::uint64_t>& target,
            std::uint64_t value) noexcept
        {
            auto maximum = target.load(std::memory_order_relaxed);
            while (maximum < value &&
                   !target.compare_exchange_weak(
                       maximum,
                       value,
                       std::memory_order_relaxed,
                       std::memory_order_relaxed))
            {}
        }

        moodycamel::ConcurrentQueue<QueuedTask> queue_;
        std::size_t pump_depth_{0};
        std::atomic<std::size_t> depth_{0u};
        std::atomic<std::size_t> high_water_{0u};
        std::atomic<std::uint64_t> oldest_enqueued_ns_{0u};
        std::atomic<std::uint64_t> adoption_samples_{0u};
        std::atomic<std::uint64_t> adoption_total_ns_{0u};
        std::atomic<std::uint64_t> adoption_max_ns_{0u};
        std::array<
            std::atomic<std::uint64_t>,
            kAsyncLatencyBucketCount> adoption_histogram_{};
        bool histograms_enabled_{false};
        mutable std::atomic<std::uint64_t> work_epoch_{0u};
        // bindExternalWake is main/composition-thread-only. Producers only
        // read external_wake_; old pointees remain owned here until teardown.
        std::vector<std::shared_ptr<const WakeBinding>> retained_external_wakes_;
        std::atomic<const WakeBinding*> external_wake_{nullptr};
    };
}
