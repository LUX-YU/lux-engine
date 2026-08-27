#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace lux::cxx
{
    /**
     * @brief Bounded single-producer / single-consumer frame ring with
     *        persistent slots.
     *
     * This container is designed for frame-style handoff between exactly one
     * producer thread and exactly one consumer thread, for example:
     *   - game thread   -> producer
     *   - render thread -> consumer
     *
     * Unlike a general-purpose SPSC queue, this class stores a fixed array of
     * persistent T objects and transfers ownership by sequence/index rather
     * than moving objects in and out of the container.  This makes it suitable
     * for heavyweight per-frame packets such as command buffers, frame
     * programs, reply batches, or objects that internally cache large dynamic
     * allocations which should be reused across frames.
     *
     * Semantics:
     *   - The consumer always owns a current read slot.
     *   - The producer may own one current write slot.
     *   - Additional published frames may be queued between producer and
     *     consumer, up to a configurable bound.
     *   - Slots are never moved; they are reused cyclically.
     *
     * This container therefore models a bounded frame pipeline rather than a
     * disposable message queue.
     *
     * Sequence model:
     *   - Slots are identified by monotonically increasing frame sequence
     *     numbers.
     *   - Physical storage index is sequence % SlotCount.
     *   - consumer sequence = current readable frame sequence.
     *   - published sequence = last successfully published frame sequence.
     *   - producer write sequence = frame sequence currently being written.
     *
     * Capacity model:
     *   - SlotCount must be at least 3.
     *   - At most SlotCount - 1 frames can exist ahead of the consumer's
     *     current read frame, because one slot is always occupied by that
     *     current read frame.
     *   - maxPendingFrames controls how many published-but-not-yet-acquired
     *     frames are allowed ahead of the consumer.
     *
     * Threading contract:
     *   - All producer methods must be called only from the producer thread.
     *   - All consumer methods must be called only from the consumer thread.
     *   - Cross-calling producer/consumer methods from the wrong thread is
     *     undefined behavior.
     *
     * Type requirements:
     *   - T must be default constructible because the ring stores SlotCount
     *     persistent objects internally.
     *
     * @tparam T         Persistent frame payload type.
     * @tparam SlotCount Number of persistent slots in the ring. Must be >= 3.
     */
    template <typename T, std::size_t SlotCount = 3> class BoundedSpscFrameRing
    {
        static_assert(SlotCount >= 3, "BoundedSpscFrameRing<T, SlotCount> requires SlotCount >= 3.");
        static_assert(
            std::is_default_constructible_v<T>,
            "BoundedSpscFrameRing<T, SlotCount> requires T to be default constructible.");

    private:
#if defined(__cpp_lib_hardware_interference_size)
        static constexpr std::size_t cacheLineSize = std::hardware_destructive_interference_size;
#else
        static constexpr std::size_t cacheLineSize = 64;
#endif

        /**
         * @brief Shared state exchanged between producer and consumer.
         *
         * publishedSequence:
         *   Last successfully published frame sequence.
         *
         * consumedSequence:
         *   Consumer's current read sequence.  Advancing this value releases
         *   older slots back to the producer.
         */
        struct alignas(cacheLineSize) SharedState
        {
            std::atomic<std::uint64_t> publishedSequence{0};
            std::atomic<std::uint64_t> consumedSequence{0};
        };

        /**
         * @brief Producer-local state.
         *
         * nextWriteSequence:
         *   Absolute frame sequence the producer will write next when a slot is
         *   available.
         *
         * hasWriteSlot:
         *   Whether the producer currently owns the slot for nextWriteSequence.
         *
         * Notes:
         *   - The producer may begin writing a new frame even if publication is
         *     not yet allowed by maxPendingFrames(), as long as a physical slot
         *     is available.
         *   - In that case publishWrite() will return false until the consumer
         *     advances enough.
         */
        struct alignas(cacheLineSize) ProducerState
        {
            std::uint64_t nextWriteSequence{1};
            bool hasWriteSlot{true};
        };

        /**
         * @brief Consumer-local state.
         *
         * currentReadSequence:
         *   Absolute sequence number of the currently readable frame.
         */
        struct alignas(cacheLineSize) ConsumerState
        {
            std::uint64_t currentReadSequence{0};
        };

    public:
        /**
         * @brief Construct the frame ring.
         *
         * Initial state:
         *   - consumer current read = slot 0, sequence 0
         *   - producer current write = slot 1, sequence 1
         *   - all other slots are free
         *
         * Slot 0 initially contains a default-constructed T.  This guarantees
         * currentRead() is always valid, even before the first published frame
         * is acquired.
         *
         * @param maxPendingFrames Maximum number of published-but-not-yet-
         *        acquired frames allowed ahead of the consumer.  Must satisfy
         *        1 <= maxPendingFrames <= SlotCount - 1.
         *
         * Recommended values:
         *   - SlotCount = 3, maxPendingFrames = 1  -> low-latency bounded lag
         *   - SlotCount = 4, maxPendingFrames = 1/2 -> slightly deeper pipeline
         */
        explicit BoundedSpscFrameRing(std::size_t maxPendingFrames = SlotCount - 2) noexcept
            : maxPendingFrames_(normalizeMaxPendingFrames(maxPendingFrames))
        {
        }

        BoundedSpscFrameRing(const BoundedSpscFrameRing&) = delete;
        BoundedSpscFrameRing& operator=(const BoundedSpscFrameRing&) = delete;
        BoundedSpscFrameRing(BoundedSpscFrameRing&&) = delete;
        BoundedSpscFrameRing& operator=(BoundedSpscFrameRing&&) = delete;

        ~BoundedSpscFrameRing() = default;

        // ---------------------------------------------------------------------
        // Producer API
        // ---------------------------------------------------------------------

        /**
         * @brief Try to obtain the current writable slot.
         *
         * If the producer already owns a write slot, this function returns it.
         * Otherwise it checks whether the next cyclic slot is safe to reuse.
         *
         * A slot is writable iff reusing it would not overwrite:
         *   - the consumer's current read slot, or
         *   - any published frame the consumer has not yet acquired.
         *
         * This condition is expressed by comparing absolute frame sequences.
         * The producer may reuse nextWriteSequence only when:
         *
         *   nextWriteSequence - consumedSequence < SlotCount
         *
         * Memory ordering:
         *   - An acquire load of consumedSequence synchronizes with the
         *     consumer's release store in tryAcquireRead().
         *   - This guarantees the producer does not reuse a slot until the
         *     consumer has logically advanced past it.
         *
         * @return Pointer to the writable slot, or nullptr if no slot can be
         *         reused yet.
         *
         * @warning Must be called only from the producer thread.
         */
        [[nodiscard]] T* tryBeginWrite() noexcept
        {
            if (producerState_.hasWriteSlot)
            {
                return &slots_[toIndex(producerState_.nextWriteSequence)];
            }

            const std::uint64_t consumed = sharedState_.consumedSequence.load(std::memory_order_acquire);

            if (!isSequenceWritable(producerState_.nextWriteSequence, consumed))
            {
                return nullptr;
            }

            producerState_.hasWriteSlot = true;
            return &slots_[toIndex(producerState_.nextWriteSequence)];
        }

        /**
         * @brief Publish the currently written slot.
         *
         * Publication succeeds only if the resulting number of published-but-
         * not-yet-acquired frames does not exceed maxPendingFrames().
         *
         * Specifically, publishing sequence S is allowed only when:
         *
         *   S - consumedSequence <= maxPendingFrames
         *
         * If publication is rejected due to the pending-frame bound, the
         * producer retains ownership of the current write slot and may retry
         * publishWrite() later without rebuilding the frame.
         *
         * Memory ordering:
         *   - A release store to publishedSequence publishes all prior writes
         *     made to the slot.
         *   - The consumer's acquire load/exchange in tryAcquireRead() ensures
         *     it sees the fully written frame contents after acquiring.
         *
         * @return true if the frame was published; false if publication is not
         *         currently allowed.
         *
         * @warning Must be called only from the producer thread.
         */
        [[nodiscard]] bool publishWrite() noexcept
        {
            if (!producerState_.hasWriteSlot)
            {
                return false;
            }

            const std::uint64_t consumed = sharedState_.consumedSequence.load(std::memory_order_acquire);

            const std::uint64_t pendingAfterPublish = producerState_.nextWriteSequence - consumed;

            if (pendingAfterPublish > maxPendingFrames_)
            {
                return false;
            }

            sharedState_.publishedSequence.store(producerState_.nextWriteSequence, std::memory_order_release);

            ++producerState_.nextWriteSequence;
            producerState_.hasWriteSlot = false;
            return true;
        }

        /**
         * @brief Check whether the producer currently owns a writable slot.
         *
         * @return true if the producer already owns a write slot, false
         *         otherwise.
         *
         * @warning Must be called only from the producer thread if used for
         *          control flow.
         */
        [[nodiscard]] bool hasWriteSlot() const noexcept
        {
            return producerState_.hasWriteSlot;
        }

        /**
         * @brief Return the configured maximum number of published pending
         *        frames allowed ahead of the consumer.
         */
        [[nodiscard]] std::size_t maxPendingFrames() const noexcept
        {
            return static_cast<std::size_t>(maxPendingFrames_);
        }

        // ---------------------------------------------------------------------
        // Consumer API
        // ---------------------------------------------------------------------

        /**
         * @brief Try to acquire the next published frame in sequence order.
         *
         * If one or more published frames are available ahead of the consumer,
         * this function advances the consumer by exactly one frame sequence.
         *
         * Important:
         *   - This class preserves frame order.  The consumer acquires
         *     currentReadSequence + 1, never jumps directly to the latest.
         *   - If the producer published multiple frames ahead, multiple
         *     tryAcquireRead() calls are required to catch up.
         *
         * Memory ordering:
         *   - An acquire load of publishedSequence synchronizes with the
         *     producer's release store in publishWrite().
         *   - A release store to consumedSequence synchronizes with the
         *     producer's acquire load in tryBeginWrite()/publishWrite().
         *
         * @return true if a new frame was acquired; false if no next frame is
         *         pending.
         *
         * @warning Must be called only from the consumer thread.
         */
        [[nodiscard]] bool tryAcquireRead() noexcept
        {
            const std::uint64_t published = sharedState_.publishedSequence.load(std::memory_order_acquire);

            const std::uint64_t next = consumerState_.currentReadSequence + 1;
            if (next > published)
            {
                return false;
            }

            consumerState_.currentReadSequence = next;
            sharedState_.consumedSequence.store(next, std::memory_order_release);
            return true;
        }

        /**
         * @brief Get the slot currently owned by the consumer for reading.
         *
         * This is always valid.  If no new frame has been acquired, it refers
         * to the previously current read slot.
         *
         * @return Reference to the current consumer-readable slot.
         *
         * @warning Must be called only from the consumer thread.
         */
        [[nodiscard]] const T& currentRead() const noexcept
        {
            return slots_[toIndex(consumerState_.currentReadSequence)];
        }

        /**
         * @brief Mutable access to the current consumer-readable slot.
         *
         * Prefer the const overload unless mutable access is genuinely needed
         * for internal housekeeping.
         *
         * @warning Must be called only from the consumer thread.
         */
        [[nodiscard]] T& currentRead() noexcept
        {
            return slots_[toIndex(consumerState_.currentReadSequence)];
        }

        // ---------------------------------------------------------------------
        // Shared / diagnostic helpers
        // ---------------------------------------------------------------------

        /**
         * @brief Check whether at least one published frame is waiting to be
         *        acquired by the consumer.
         *
         * This is a momentary snapshot only.
         */
        [[nodiscard]] bool hasPendingFrame() const noexcept
        {
            const std::uint64_t published = sharedState_.publishedSequence.load(std::memory_order_acquire);
            return published > consumerState_.currentReadSequence;
        }

        /**
         * @brief Return the current number of published-but-not-yet-acquired
         *        frames ahead of the consumer.
         *
         * Because producer and consumer run concurrently, this value is only a
         * snapshot.
         */
        [[nodiscard]] std::size_t pendingFrames() const noexcept
        {
            const std::uint64_t published = sharedState_.publishedSequence.load(std::memory_order_acquire);
            const std::uint64_t consumed = sharedState_.consumedSequence.load(std::memory_order_acquire);
            return static_cast<std::size_t>(published - consumed);
        }

        /**
         * @brief Return how many frames ahead the producer's current write
         *        sequence is relative to the consumer's current read sequence.
         *
         * If the producer does not currently own a write slot, this refers to
         * the next sequence it would try to write.
         *
         * This is a diagnostic/helper value and should not be used as a strict
         * synchronization primitive.
         */
        [[nodiscard]] std::size_t framesAhead() const noexcept
        {
            const std::uint64_t consumed = sharedState_.consumedSequence.load(std::memory_order_acquire);
            return static_cast<std::size_t>(producerState_.nextWriteSequence - consumed);
        }

    private:
        [[nodiscard]] static constexpr std::size_t toIndex(std::uint64_t sequence) noexcept
        {
            return static_cast<std::size_t>(sequence % SlotCount);
        }

        [[nodiscard]] static constexpr std::uint64_t normalizeMaxPendingFrames(std::size_t requested) noexcept
        {
            if (requested == 0)
            {
                return 1;
            }

            const std::size_t maxAllowed = SlotCount - 1;
            return static_cast<std::uint64_t>(requested > maxAllowed ? maxAllowed : requested);
        }

        [[nodiscard]] static constexpr bool
        isSequenceWritable(std::uint64_t candidateWriteSequence, std::uint64_t consumedSequence) noexcept
        {
            return (candidateWriteSequence - consumedSequence) < SlotCount;
        }

    private:
        /**
         * @brief Persistent frame slots.
         *
         * Objects are never moved.  Ownership is transferred only by absolute
         * frame sequence and cyclic slot index.
         */
        alignas(cacheLineSize) std::array<T, SlotCount> slots_{};

        /**
         * @brief Shared synchronization state.
         */
        SharedState sharedState_{};

        /**
         * @brief Producer-local state.
         */
        ProducerState producerState_{};

        /**
         * @brief Consumer-local state.
         */
        ConsumerState consumerState_{};

        /**
         * @brief Maximum number of published pending frames allowed ahead of
         *        the consumer.
         */
        const std::uint64_t maxPendingFrames_;
    };

} // namespace lux::cxx
