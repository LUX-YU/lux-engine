#pragma once

#include <lux/engine/render/effect/FrameAlgorithm.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>

namespace lux::render::detail
{
    struct BoundInlineBlock final
    {
        CompiledPassSlot pass;
        std::span<const std::byte> bytes;
    };

    struct PreparationRange final
    {
        std::uint32_t first = 0, count = 0;
        [[nodiscard]] bool contains(std::uint32_t slot) const noexcept { return slot >= first && slot - first < count; }
    };

    struct FrameAlgorithm final
    {
        extension_detail::LinkedFrameAlgorithm call;
        std::uint32_t workspace_slot = kGraphInvalidIndex;
        PreparationRange streams, buffers;
        std::uint32_t first_view = 0;
        std::size_t inline_offset = 0, inline_size = 0;
    };

    struct FrameAlgorithms final
    {
        [[nodiscard]] bool empty() const noexcept
        {
            return calls.empty();
        }

        struct ConstantCopy final
        {
            std::size_t offset;
            CompiledPassSlot pass;
            std::uint32_t bytes, revision_offset = kGraphInvalidIndex;
        };

        static_assert(std::is_standard_layout_v<ConstantCopy> && std::is_trivially_copyable_v<ConstantCopy>);

        std::vector<ConstantCopy> constant_copies;
        std::size_t inline_bytes = 0;
        // Final import slot for each Resource read in call argument order.
        std::vector<std::uint32_t> resource_view_slots;
        [[nodiscard]] RenderResult<std::uint32_t> drawOutput(std::uint32_t call, std::uint32_t pass) const noexcept;
        [[nodiscard]] RenderResult<std::uint32_t> drawOutput(std::uint32_t call, RasterResourceSlot source) const noexcept;
        std::vector<std::uint32_t> output_by_pass;
        std::vector<const CompiledRasterStream*> draw_outputs;

        struct DrawBinding final
        {
            std::uint32_t binding, output;
            DrawResourceReads reads;
        };

        std::vector<DrawBinding> draw_resource_bindings;
        std::uint32_t workspace_count = 0;
        std::vector<FrameAlgorithm> calls;
    };
}
