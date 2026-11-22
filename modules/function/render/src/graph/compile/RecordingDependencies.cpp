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

        }
    } // namespace

    RecordingDependencies GraphCompiler::Workspace::recordingDependencies(const compiled::RenderGraph& plan) const noexcept
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
            if (batches[batch].reusable_recording && p < batches[batch].output_begin)
                batch_uses.emplace_back(resource, batch);
        };
        for (std::uint32_t p = 0; p < recording.passes.size(); ++p)
        {
            const auto& pass = recording.passes[p];
            commandResources(recording, pass, [&](std::uint32_t resource) { add(resource, p); });
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
            if (const auto* slot = std::get_if<ComputeCommandSlot>(&pass.commands))
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
            const auto* raster = recording.rasterCommands(pass);
            const auto* list = raster ? std::get_if<RasterListCommands>(&raster->draw) : nullptr;
            if (list)
            {
                result.raster_list_count = std::max(result.raster_list_count, list->data_index + 1);
                if (list->producer_stream) result.producer_passes.push_back(p);
            }
            const auto batch = pass.submission;
            if (!batches[batch].reusable_recording || p >= batches[batch].output_begin) continue;
            const auto& sets = bindings.passes[pass.binding_slot].shared_sets;
            for (std::size_t kind = 0; kind < sets.size(); ++kind)
            {
                auto& consumers = result.shared_batches[kind];
                if (sets[kind] != kGraphInvalidIndex && (consumers.empty() || consumers.back() != batch))
                    consumers.push_back(batch);
            }
            if (list) result.raster_lists.emplace_back(batch, list->data_index);
        }
        for (std::uint32_t r = 0; r < plan.resources().size(); ++r)
        {
            const auto& resource = plan.resources()[r];
            if (resource.memory_used)
            {
                const bool runtime_bound = resource.origin == EGraphOrigin::IMPORTED ||
                    resource.origin == EGraphOrigin::FRAME_BUFFER;
                if (runtime_bound && std::holds_alternative<GraphBufferDesc>(resource.description))
                    ++result.required_buffers;
                if (resource.origin != EGraphOrigin::IMPORTED && std::holds_alternative<GraphImageDesc>(resource.description))
                    ++result.owned_image_capacity;
            }
            for (const auto& use : resource.barrier_uses) add(r, use.first);
            for (const auto& use : resource.descriptor_uses) add(r, use.first);
            for (const auto& view : resource.image_views) add(r, view.pass);
            for (const auto p : resource.extent_uses) add(r, p);
            // Final transitions belong to the last CB, which can be a separately recorded WSI tail.
            if (!resource.final_barrier_uses.empty() && batches.back().reusable_recording &&
                batches.back().output_begin == kGraphInvalidIndex)
                batch_uses.emplace_back(r, static_cast<std::uint32_t>(batches.size() - 1));
        }
        const auto publish = [&](Edges& uses, RecordingDependencies::ResourceIndex& index)
        {
            std::ranges::sort(uses);
            const auto unique = std::ranges::unique(uses);
            uses.erase(unique.begin(), unique.end());
            index.slots.reserve(uses.size());
            index.offsets.resize(plan.resources().size() + 1);
            for (const auto& [resource, slot] : uses)
            {
                ++index.offsets[resource + 1];
                index.slots.push_back(slot);
            }
            std::partial_sum(index.offsets.begin(), index.offsets.end(), index.offsets.begin());
        };
        result.pass_field_offsets.push_back(static_cast<std::uint32_t>(result.operand_fields.size()));
        publish(field_uses, result.resource_operand_fields);
        publish(pass_uses, result.resource_passes);
        publish(batch_uses, result.resource_batches);
        return result;
    }
} // namespace lux::render::detail
