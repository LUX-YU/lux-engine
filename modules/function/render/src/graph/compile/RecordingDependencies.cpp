#include <lux/engine/render/graph/compile/GraphCompileWorkspace.hpp>
#include <algorithm>
#include <numeric>

namespace lux::render::detail
{
    namespace
    {
        // Only slots dereferenced by a fixed packet belong here. Dynamic lists compare their selected native draws.
        void commandResources(const RecordingProgram& recording, const CompiledPass& pass, auto add)
        {
            if (const auto* raster = recording.rasterCommands(pass))
            {
                for (const auto& vertex : raster->vertices) add(vertex.buffer);
                if (const auto* indexed = std::get_if<IndexedDrawPacket>(&raster->draw))
                    add(indexed->index.location.buffer);
                else if (const auto* indirect = std::get_if<IndirectDrawPacket>(&raster->draw))
                {
                    add(indirect->index.location.buffer);
                    add(indirect->arguments.buffer);
                    add(indirect->count_buffer.buffer);
                }
            }
            else if (const auto* compute = recording.computeCommands(pass))
            {
                if (const auto* indirect = std::get_if<BufferLocation>(&compute->dispatch)) add(indirect->buffer);
                else if (const auto* extent = std::get_if<ImageDispatchExtent>(&compute->dispatch)) add(extent->image);
            }
            else
            {
                const auto operand = [&](const auto& value)
                {
                    add(value.slot);
                    add(value.fallback);
                };
                for (const auto& packet : recording.transferCommands(pass)->operations)
                {
                    std::visit([&](const auto& op)
                    {
                        if constexpr (std::same_as<std::remove_cvref_t<decltype(op)>, ImageBufferPacket>)
                        {
                            operand(op.image);
                            operand(op.buffer);
                        }
                        else
                        {
                            if constexpr (requires { op.source; }) operand(op.source);
                            operand(op.destination);
                        }
                    }, packet);
                }
            }
        }
    } // namespace

    RecordingDependencies GraphCompiler::Workspace::recordingDependencies(const RenderGraphExecutable& plan) const noexcept
    {
        RecordingDependencies result;
        const auto batches = plan.submissions();
        const auto& recording = plan.recordingProgram();
        std::vector<std::pair<std::uint32_t, std::uint32_t>> uses;
        const auto add = [&](std::uint32_t resource, std::uint32_t p)
        {
            const auto batch = recording.passes[p].submission;
            if (batches[batch].reusable_recording && p < batches[batch].output_begin)
                uses.emplace_back(resource, batch);
        };
        for (std::uint32_t p = 0; p < recording.passes.size(); ++p)
        {
            const auto& pass = recording.passes[p];
            const auto batch = pass.submission;
            if (!batches[batch].reusable_recording || p >= batches[batch].output_begin) continue;
            commandResources(recording, pass, [&](std::uint32_t resource)
            {
                if (resource != kGraphInvalidIndex) uses.emplace_back(resource, batch);
            });
            const auto& sets = bindings.passes[pass.binding_slot].shared_sets;
            for (std::size_t kind = 0; kind < sets.size(); ++kind)
            {
                auto& consumers = result.shared_batches[kind];
                if (sets[kind] != kGraphInvalidIndex && (consumers.empty() || consumers.back() != batch))
                    consumers.push_back(batch);
            }
            const auto* raster = recording.rasterCommands(pass);
            const auto* list = raster ? std::get_if<RasterListCommands>(&raster->draw) : nullptr;
            if (list) result.raster_lists.emplace_back(batch, list->data_index);
        }
        for (std::uint32_t r = 0; r < plan.resources().size(); ++r)
        {
            const auto& resource = plan.resources()[r];
            for (const auto& use : resource.barrier_uses) add(r, use.first);
            for (const auto& use : resource.descriptor_uses) add(r, use.first);
            for (const auto& view : resource.image_views) add(r, view.pass);
            for (const auto p : resource.extent_uses) add(r, p);
            // Final transitions belong to the last CB, which can be a separately recorded WSI tail.
            if (!resource.final_barrier_uses.empty() && batches.back().reusable_recording &&
                batches.back().output_begin == kGraphInvalidIndex)
                uses.emplace_back(r, static_cast<std::uint32_t>(batches.size() - 1));
        }
        std::ranges::sort(uses);
        const auto unique = std::ranges::unique(uses);
        uses.erase(unique.begin(), unique.end());
        result.batches.reserve(uses.size());
        result.offsets.resize(plan.resources().size() + 1);
        for (const auto& [resource, batch] : uses)
        {
            ++result.offsets[resource + 1];
            result.batches.push_back(batch);
        }
        std::partial_sum(result.offsets.begin(), result.offsets.end(), result.offsets.begin());
        return result;
    }
} // namespace lux::render::detail
