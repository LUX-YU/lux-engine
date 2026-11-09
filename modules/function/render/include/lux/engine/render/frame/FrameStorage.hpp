#pragma once

#include <lux/engine/render/frame/FrameData.hpp>
#include <lux/engine/render/visibility.h>
#include <memory>
#include <cstring>
#include <vector>

namespace lux::render
{
    class FrameWriter;
    struct FrameCompletion;

    using FrameCompleted = void (*)(void*, const FrameCompletion&) noexcept;

    template <class T> struct FrameArray final
    {
        std::span<T> values;
        FrameDataRange<T> range;
    };

    // One application-owned CPU frame slot. Deliver local completion observers before reset(),
    // after the Server has returned its borrow (or the application has settled the Server).
    // Allocation capacity is retained; this object owns no GPU or execution state.
    class LUX_RENDER_PUBLIC FrameStorage final
    {
    public:
        explicit FrameStorage(std::size_t block_size = 64 * 1024) noexcept;
        ~FrameStorage() noexcept;
        FrameStorage(const FrameStorage&) = delete;
        FrameStorage& operator=(const FrameStorage&) = delete;
        FrameStorage(FrameStorage&&) noexcept;
        FrameStorage& operator=(FrameStorage&&) noexcept;

        void reset() noexcept;
        // Application-thread adoption only, including an unsubmitted frame being discarded.
        // Callbacks must not reset, move or write this storage during delivery. Their borrowed
        // targets remain alive until this call returns. No callback is part of RenderFrame.
        void deliverCompletion(const FrameCompletion&) noexcept;
        [[nodiscard]] RenderFrame seal(std::uint64_t sequence, bool resynchronize = false) const noexcept;
        [[nodiscard]] std::size_t retainedBytes() const noexcept;

    private:
        friend class FrameWriter;

        struct Block final
        {
            std::byte* data;
            std::size_t capacity;
            std::size_t alignment;
        };

        struct Allocation final
        {
            void* data;
            FrameDataRange<std::byte> range;
        };

        struct CompletionObserver final
        {
            void* data;
            FrameCompleted completed;
        };

        [[nodiscard]] RenderResult<Allocation> allocate(std::size_t bytes, std::size_t alignment) noexcept;
        [[nodiscard]] RenderResult<void> append(std::span<const FrameEntry> entries) noexcept;
        void clear() noexcept;

        std::vector<Block> blocks_;
        std::vector<std::span<const std::byte>> used_blocks_;
        std::vector<FrameEntry> entries_;
        std::vector<FramePhase> phases_;
        std::vector<CompletionObserver> completion_observers_;
        std::size_t block_size_;
        std::size_t current_block_ = 0;
    };

    // A temporary writer into the selected FrameStorage. It owns no allocator or parallel packet.
    class FrameWriter final
    {
    public:
        explicit FrameWriter(FrameStorage& storage) noexcept : storage_(storage) {}

        // Register once per producer, not per updated resource. The producer can retain a
        // revision summary in this frame's storage; it must not clear newer dirty revisions
        // when observing this completion. A failed frame is not an atomic rollback.
        void onCompletion(void* data, FrameCompleted completed) noexcept
        {
            storage_.completion_observers_.push_back({data, completed});
        }

        template <class T>
            requires(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T> &&
                     std::is_nothrow_default_constructible_v<T>)
        [[nodiscard]] RenderResult<FrameArray<T>> allocate(std::size_t count) noexcept
        {
            if (count == 0)
            {
                return FrameArray<T>{};
            }
            if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
            {
                return cxx::unexpected(RenderError{kRuntimeCapacity});
            }
            auto memory = storage_.allocate(count * sizeof(T), alignof(T));
            if (!memory)
            {
                return cxx::unexpected(memory.error());
            }
            auto* values = static_cast<T*>(memory->data);
            std::uninitialized_default_construct_n(values, count);
            return FrameArray<T>{{values, count}, {memory->range.block, memory->range.offset, count}};
        }

        template <class T>
            requires(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>)
        [[nodiscard]] RenderResult<FrameDataRange<T>> copy(std::span<const T> source) noexcept
        {
            if (source.empty())
            {
                return FrameDataRange<T>{};
            }
            if (source.size() > std::numeric_limits<std::size_t>::max() / sizeof(T))
            {
                return cxx::unexpected(RenderError{kRuntimeCapacity});
            }
            auto memory = storage_.allocate(source.size_bytes(), alignof(T));
            if (!memory)
            {
                return cxx::unexpected(memory.error());
            }
            // memcpy starts the trivial objects' lifetime. Do not first default-construct an
            // array that will be overwritten, or require a constructor for snapshot-only types.
            std::memcpy(memory->data, source.data(), source.size_bytes());
            return FrameDataRange<T>{memory->range.block, memory->range.offset, source.size()};
        }

        template <class T>
        [[nodiscard]] RenderResult<FrameEntry>
        store(FrameDataBinding<T> binding, const typename FrameDataLayout<T>::Stored& value) noexcept
        {
            using Stored = typename FrameDataLayout<T>::Stored;
            static_assert(std::is_trivially_copyable_v<Stored> && std::is_trivially_destructible_v<Stored>);
            auto memory = storage_.allocate(sizeof(Stored), alignof(Stored));
            if (!memory)
            {
                return cxx::unexpected(memory.error());
            }
            std::construct_at(static_cast<Stored*>(memory->data), value);
            return FrameEntry{binding.destination, memory->range};
        }

        // Default ordering: one input per phase. Independent batches are explicit, never inferred
        // from Handler type or automatically reordered across collectors.
        template <class T>
        [[nodiscard]] RenderResult<void>
        write(FrameDataBinding<T> binding, const typename FrameDataLayout<T>::Stored& value) noexcept
        {
            auto entry = store(binding, value);
            if (!entry)
            {
                return cxx::unexpected(entry.error());
            }
            return storage_.append({&*entry, 1});
        }

        [[nodiscard]] RenderResult<void> writeIndependent(std::span<const FrameEntry> entries) noexcept
        {
            return storage_.append(entries);
        }

    private:
        FrameStorage& storage_;
    };
} // namespace lux::render
