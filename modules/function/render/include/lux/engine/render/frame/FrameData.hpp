#pragma once

#include <limits>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/frame/RenderFrame.hpp>
#include <type_traits>

namespace lux::render
{
    // Short-lived decoding view. Resolved spans continue to borrow the submitted frame's stable blocks.
    class FrameDataView final
    {
    public:
        explicit FrameDataView(std::span<const std::span<const std::byte>> blocks) noexcept : blocks_(blocks) {}

        template <class T>
            requires(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>)
        [[nodiscard]] RenderResult<std::span<const T>> read(FrameDataRange<T> range) const noexcept
        {
            if (range.count == 0)
            {
                return std::span<const T>{};
            }
            const bool is_invalid_block = range.block >= blocks_.size();
            const bool is_invalid_count = range.count > std::numeric_limits<std::size_t>::max() / sizeof(T);
            if (is_invalid_block || is_invalid_count)
            {
                return cxx::unexpected<RenderError>(RenderError{kFrameContract});
            }
            const auto bytes = static_cast<std::size_t>(range.count) * sizeof(T);
            const auto block = blocks_[range.block];
            const bool is_invalid_range = range.offset > block.size() || bytes > block.size() - range.offset;
            if (is_invalid_range)
            {
                return cxx::unexpected<RenderError>(RenderError{kFrameContract});
            }
            const auto* data = block.data() + static_cast<std::size_t>(range.offset);
            if (reinterpret_cast<std::uintptr_t>(data) % alignof(T) != 0)
            {
                return cxx::unexpected<RenderError>(RenderError{kFrameContract});
            }
            return std::span<const T>{reinterpret_cast<const T*>(data), static_cast<std::size_t>(range.count)};
        }

        // Only after read() accepted this exact range on this view. Professional input
        // decoding validates once; subsequent passes over the immutable frame borrow
        // do not repeat the same bounds/alignment checks or allocate decoded arrays.
        template <class T> [[nodiscard]] std::span<const T> borrow(FrameDataRange<T> range) const noexcept
        {
            if (range.count == 0)
            {
                return {};
            }
            return {
                reinterpret_cast<const T*>(blocks_[range.block].data() + range.offset),
                static_cast<std::size_t>(range.count)
            };
        }

    private:
        std::span<const std::span<const std::byte>> blocks_;
    };

    // Fixed-value inputs use their real type. Inputs with variable arrays specialize Stored with
    // FrameDataRange fields and decode them to non-owning business spans at the professional boundary.
    template <class T> struct FrameDataLayout
    {
        static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);
        using Stored = T;

        [[nodiscard]] static RenderResult<T> read(const FrameDataView&, const Stored& value) noexcept
        {
            return value;
        }
    };
} // namespace lux::render
