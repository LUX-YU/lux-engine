#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <algorithm>

namespace lux::render::detail
{
    void mergeIndexedIndirectDraws(RasterDrawList& draws, std::uint32_t limit) noexcept
    {
        if (limit <= 1 || draws.packets.size() < 2) return;
        const auto same_state = [&](const RasterDrawPacket& a, const RasterDrawPacket& b) noexcept
        {
            const bool same_pipeline = a.pipeline == b.pipeline && a.bindings.layout == b.bindings.layout &&
                a.bindings.private_set == b.bindings.private_set && a.bindings.shared_sets == b.bindings.shared_sets;
            const bool same_index = a.index == b.index && a.index_offset == b.index_offset &&
                a.index_type == b.index_type;
            const bool same_scissor = a.scissor.x == b.scissor.x && a.scissor.y == b.scissor.y &&
                a.scissor.width == b.scissor.width && a.scissor.height == b.scissor.height;
            const bool same_counts = a.descriptor_count == b.descriptor_count && a.vertex_count == b.vertex_count &&
                a.parameter_size == b.parameter_size;
            if (!same_pipeline || !same_index || !same_scissor || !same_counts) return false;
            const auto descriptors = std::span(draws.descriptors);
            if (!std::ranges::equal(
                    descriptors.subspan(a.first_descriptor, a.descriptor_count),
                    descriptors.subspan(b.first_descriptor, b.descriptor_count)
                )) return false;
            for (std::uint32_t i = 0; i < a.vertex_count; ++i)
            {
                const auto& left = draws.vertices[a.first_vertex + i];
                const auto& right = draws.vertices[b.first_vertex + i];
                const bool same_vertex = left.binding == right.binding && left.buffer.buffer == right.buffer.buffer &&
                    left.buffer.offset == right.buffer.offset && left.buffer.stride == right.buffer.stride;
                if (!same_vertex) return false;
            }
            const auto parameters = std::span(draws.parameters);
            return std::ranges::equal(
                parameters.subspan(a.parameter_offset, a.parameter_size),
                parameters.subspan(b.parameter_offset, b.parameter_size)
            );
        };

        const auto append = [&](RasterDrawPacket& previous, const RasterDrawPacket& current) noexcept
        {
            if (!previous.index || !current.index) return false;
            auto* batch = std::get_if<RasterDrawPacket::Indirect>(&previous.draw);
            const auto* command = std::get_if<RasterDrawPacket::Indirect>(&current.draw);
            const bool can_merge = batch && command && batch->mergeable && command->mergeable;
            if (!can_merge || command->count != 1 || !batch->count || batch->count >= limit) return false;
            const bool same_arguments = batch->arguments == command->arguments && command->offset > batch->offset;
            if (!same_arguments || batch->count_buffer || command->count_buffer) return false;
            const auto distance = command->offset - batch->offset;
            const auto stride = batch->count == 1 ? distance : batch->stride;
            const bool valid_stride = stride >= sizeof(nri::DrawIndexedDesc) && stride <= UINT32_MAX &&
                stride % 4 == 0 && distance == std::uint64_t(batch->count) * stride;
            if (!valid_stride || !same_state(previous, current)) return false;
            // Each authored record was range checked before merging. Keep their order and section.
            batch->stride = static_cast<std::uint32_t>(stride);
            ++batch->count;
            return true;
        };

        std::uint32_t written = 0;
        const auto merge_range = [&](std::uint32_t first, std::uint32_t count) noexcept
        {
            const auto output_begin = written;
            for (std::uint32_t i = first; i < first + count; ++i)
            {
                const auto& packet = draws.packets[i];
                if (written != output_begin && append(draws.packets[written - 1], packet)) continue;
                if (written != i) draws.packets[written] = packet;
                ++written;
            }
        };
        if (draws.sections.empty())
        {
            merge_range(0, static_cast<std::uint32_t>(draws.packets.size()));
        }
        else
        {
            for (auto& section : draws.sections)
            {
                const auto first = written;
                merge_range(section.first_draw, section.draw_count);
                section.first_draw = first;
                section.draw_count = written - first;
            }
        }
        draws.packets.resize(written);
    }

    void RenderGraphInstance::FrameResources::invalidateImportedBindings() noexcept
    {
        record_ready = false;
        // Unique cache revisions prevent a reused instance address from matching an older binding cache.
        static std::atomic<std::uint64_t> next_revision{1};
        imported_revision = next_revision.fetch_add(1, std::memory_order_relaxed);
    }
} // namespace lux::render::detail
