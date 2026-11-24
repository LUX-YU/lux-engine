#include <lux/engine/render/graph/compile/GraphCompileWorkspace.hpp>
#include <algorithm>
#include <numeric>

namespace lux::render::detail
{
    RecordingDependencies RenderGraphCompiler::Workspace::recordingDependencies(const compiled::RenderGraph& plan) const noexcept
    {
        RecordingDependencies result;
        const auto batches = plan.submissions();
        const auto& recording = plan.recordingProgram();
        using Edges = std::vector<std::pair<std::uint32_t, std::uint32_t>>;
        Edges pass_uses, batch_uses, field_uses;
        result.pass_field_offsets.reserve(recording.passes.size() + 1);
        const auto add = [&](std::uint32_t resource, std::uint32_t p)
        {
            if (resource == kGraphInvalidIndex) return;
            pass_uses.emplace_back(resource, p);
            const auto batch = recording.passes[p].submission;
            if (batches[batch].reusesPass(p))
                batch_uses.emplace_back(resource, batch);
        };
        for (std::uint32_t p = 0; p < recording.passes.size(); ++p)
        {
            const auto& pass = recording.passes[p];
            for (const auto& barrier : recording.imageBarriers(pass)) add(barrier.resource, p);
            for (const auto& barrier : recording.bufferBarriers(pass)) add(barrier.resource, p);
            result.pass_field_offsets.push_back(static_cast<std::uint32_t>(result.operand_fields.size()));
            using Field = RecordingDependencies::EOperandField;
            const auto addField = [&](Field field, std::uint32_t row)
            {
                const auto index = static_cast<std::uint32_t>(result.operand_fields.size());
                result.operand_fields.push_back({field, p, row});
                return index;
            };
            const auto depends = [&](std::uint32_t resource, std::uint32_t field)
            {
                if (resource == kGraphInvalidIndex) return;
                add(resource, p);
                field_uses.emplace_back(resource, field);
            };
            const auto* raster = recording.rasterCommands(pass);
            if (raster)
            {
                for (auto i = raster->first_vertex; i < raster->first_vertex + raster->vertex_count; ++i)
                    depends(recording.raster_vertices[i].buffer, addField(Field::RASTER_VERTEX, i));
                const auto address = [&](std::uint32_t row)
                {
                    depends(recording.raster_addresses[row].location.buffer, addField(Field::RASTER_ADDRESS, row));
                };
                if (const auto* indexed = std::get_if<IndexedDrawPacket>(&raster->draw)) address(indexed->index.address);
                else if (const auto* indirect = std::get_if<IndirectDrawPacket>(&raster->draw))
                {
                    address(indirect->index.address);
                    address(indirect->arguments);
                    if (indirect->count_buffer != kGraphInvalidIndex) address(indirect->count_buffer);
                }
            }
            else if (const auto* slot = std::get_if<ComputeCommandSlot>(&pass.commands))
            {
                const auto& dispatch = recording.compute[slot->index].dispatch;
                if (const auto* indirect = std::get_if<BufferLocation>(&dispatch))
                    depends(indirect->buffer, addField(Field::INDIRECT_DISPATCH, slot->index));
                else if (const auto* extent = std::get_if<ImageDispatchExtent>(&dispatch))
                    depends(extent->image, addField(Field::IMAGE_DISPATCH, slot->index));
            }
            else if (const auto* range = recording.transferCommands(pass))
            {
                const auto& program = recording.transfers;
                const auto patch = [&](Field field, std::uint32_t operand, const auto&... sources)
                {
                    const auto index = addField(field, operand);
                    (depends(sources.slot, index), ...);
                    (depends(sources.fallback, index), ...);
                };
                for (const auto [opcode, row] : program.commands(*range))
                {
                    switch (opcode)
                    {
                    case ETransferOpcode::COPY_BUFFER:
                        patch(Field::BUFFER_SOURCE, row, program.buffer_copies[row].source);
                        patch(Field::BUFFER_DESTINATION, row, program.buffer_copies[row].destination);
                        break;
                    case ETransferOpcode::COPY_IMAGE:
                        patch(Field::IMAGE_COPY, row, program.image_copies[row].source, program.image_copies[row].destination);
                        break;
                    case ETransferOpcode::UPLOAD:
                    case ETransferOpcode::READBACK:
                        patch(Field::IMAGE_BUFFER, row, program.image_buffers[row].image, program.image_buffers[row].buffer);
                        break;
                    case ETransferOpcode::ZERO_BUFFER:
                    case ETransferOpcode::FILL_STORAGE: patch(Field::FILL, row, program.fills[row].destination); break;
                    case ETransferOpcode::CLEAR_COLOR:
                    case ETransferOpcode::CLEAR_DEPTH: patch(Field::CLEAR, row, program.clears[row].destination); break;
                    }
                }
            }
            const auto* scope = recording.rasterScope(pass);
            if (scope && !scope->viewport) result.viewport_passes.push_back(p);
            const auto* list = raster ? std::get_if<RasterListCommands>(&raster->draw) : nullptr;
            if (list)
            {
                result.raster_list_count = std::max(result.raster_list_count, list->data_index + 1);
                if (list->producer_stream)
                {
                    // Refresh a dynamic list's operands before deciding whether its CB is still reusable.
                    const auto& declared = source.graph.passes()[source.analysis.execution_order[p].index];
                    for (const auto& use : declared.uses)
                    {
                        pass_uses.emplace_back(use.version.resource.index, p);
                        if (use.fallback) pass_uses.emplace_back(use.fallback->resource.index, p);
                    }
                }
            }
            const auto batch = pass.submission;
            if (!batches[batch].reusesPass(p)) continue;
            const auto& sets = bindings.passes[pass.binding_slot].shared_sets;
            for (std::size_t kind = 0; kind < sets.size(); ++kind)
            {
                auto& consumers = result.shared_batches[kind];
                if (sets[kind] != kGraphInvalidIndex && (consumers.empty() || consumers.back() != batch))
                    consumers.push_back(batch);
            }
            if (list) result.raster_lists.emplace_back(batch, list->data_index);
        }
        for (const auto& slot : plan.physicalResources().slots)
            if (std::holds_alternative<GraphImageDesc>(plan.resources()[slot.resource.index].description))
                ++result.owned_image_capacity;
        for (std::uint32_t r = 0; r < plan.resources().size(); ++r)
        {
            const auto& resource = plan.resources()[r];
            if (resource.memory_used)
            {
                const bool runtime_bound = resource.origin == EGraphOrigin::IMPORTED ||
                    resource.origin == EGraphOrigin::FRAME_BUFFER;
                if (runtime_bound && std::holds_alternative<GraphBufferDesc>(resource.description))
                    ++result.required_buffers;
            }
            for (const auto& use : resource.descriptor_uses) add(r, use.first);
            for (const auto& view : resource.image_views) add(r, view.pass);
            for (const auto p : resource.extent_uses) add(r, p);
        }
        for (const auto& release : recording.buffer_read_releases) add(release.resource, release.previous_slot);
        for (std::uint32_t p = 0; p + 1 < recording.transient_exit_offsets.size(); ++p)
            for (auto i = recording.transient_exit_offsets[p]; i < recording.transient_exit_offsets[p + 1]; ++i)
                add(recording.transient_exits[i].resource, p);
        // Final transitions belong to the last CB, which can be a separately recorded WSI tail.
        if (!batches.empty() && !batches.back().recordings.empty() && batches.back().recordings.back().reusable)
        {
            const auto last = static_cast<std::uint32_t>(batches.size() - 1);
            for (const auto& barrier : recording.after.images) batch_uses.emplace_back(barrier.resource, last);
            for (const auto& barrier : recording.after.buffers) batch_uses.emplace_back(barrier.resource, last);
        }
        const auto publish = [](Edges& uses, std::size_t sources, RecordingDependencies::SlotIndex& index)
        {
            std::ranges::sort(uses);
            const auto unique = std::ranges::unique(uses);
            uses.erase(unique.begin(), unique.end());
            index.slots.reserve(uses.size());
            index.offsets.resize(sources + 1);
            for (const auto& [resource, slot] : uses)
            {
                ++index.offsets[resource + 1];
                index.slots.push_back(slot);
            }
            std::partial_sum(index.offsets.begin(), index.offsets.end(), index.offsets.begin());
        };
        result.pass_field_offsets.push_back(static_cast<std::uint32_t>(result.operand_fields.size()));
        publish(field_uses, plan.resources().size(), result.resource_operand_fields);
        publish(pass_uses, plan.resources().size(), result.resource_passes);
        publish(batch_uses, plan.resources().size(), result.resource_batches);
        Edges input_regions, history_regions;
        for (std::uint32_t r = 0; r < recording.conditions.size(); ++r)
        {
            const auto& region = recording.conditions[r];
            if (region.source == EConditionSource::INPUT) input_regions.emplace_back(region.input_slot, r);
            if (region.source == EConditionSource::HISTORY_VALID) history_regions.emplace_back(region.input_slot, r);
        }
        publish(input_regions, plan.conditionCount(), result.input_regions);
        publish(history_regions, plan.histories().size(), result.history_regions);
        return result;
    }
} // namespace lux::render::detail
