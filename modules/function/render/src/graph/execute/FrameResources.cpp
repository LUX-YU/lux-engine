#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>
#include <lux/engine/render/graph/execute/CommandExtent.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <algorithm>
#include <bit>

namespace lux::render::detail
{
    namespace
    {
        bool containsBytes(const PassResourceInput& resource, std::uint64_t offset, std::uint64_t count) noexcept
        {
            const bool fits = offset <= resource.buffer_size && count <= resource.buffer_size - offset;
            const bool addressable = offset <= UINT64_MAX - resource.buffer_offset &&
                count <= UINT64_MAX - (resource.buffer_offset + offset);
            return resource.buffer && fits && addressable;
        }

        struct TransferSources final
        {
            std::span<const PassResourceInput> resources;
            CopyAlignment alignment;

            [[nodiscard]] const PassResourceInput& selected(const auto& operand) const noexcept
            {
                const auto& primary = resources[operand.slot];
                const bool fallback = !primary.history_valid && operand.fallback != kGraphInvalidIndex;
                if (fallback) return resources[operand.fallback];
                return primary;
            }

            [[nodiscard]] RenderResult<nri::TextureRegionDesc> region(const ImageOperand& operand) const noexcept
            {
                const auto& image = selected(operand);
                if (!image.image) return cxx::unexpected(RenderError{kDeviceArgument});
                return copyRegion(operand, {image.width, image.height});
            }

            [[nodiscard]] RenderResult<void>
            patch(const ImageCopyPacket& copy, TextureCopyRegion& output) const noexcept
            {
                auto from = region(copy.source);
                if (!from) return cxx::unexpected(from.error());
                auto to = region(copy.destination);
                if (!to) return cxx::unexpected(to.error());
                if (from->width != to->width || from->height != to->height)
                    return cxx::unexpected(RenderError{kInvalidTargetExtent});
                output.source = selected(copy.source).image;
                output.destination = selected(copy.destination).image;
                output.source_region = *from;
                output.destination_region = *to;
                return {};
            }

            [[nodiscard]] RenderResult<void>
            patch(const ImageBufferPacket& copy, FrozenImageBuffer& output) const noexcept
            {
                auto rect = region(copy.image);
                if (!rect) return cxx::unexpected(rect.error());
                const auto& buffer = selected(copy.buffer);
                auto layout = copyLayout(copy, *rect, buffer.buffer_offset, alignment);
                if (!layout) return cxx::unexpected(layout.error());
                if (!containsBytes(buffer, copy.buffer.range.offset, layout->slicePitch))
                    return cxx::unexpected(RenderError{kDeviceArgument});
                output = {selected(copy.image).image, buffer.buffer, *rect, *layout};
                return {};
            }
        };

    } // namespace

    ResourceQueueUses FrameResources::selectedUses(std::uint32_t resource) const noexcept
    {
        const auto found = std::ranges::find(replaced_projections, resource, &FrozenProjection::resource);
        return found == replaced_projections.end() ? bindings[resource].queue_uses : found->uses;
    }

    void FrameResources::preserveUses(std::uint32_t resource) noexcept
    {
        if (std::ranges::find(replaced_projections, resource, &FrozenProjection::resource) == replaced_projections.end())
            replaced_projections.push_back({resource, bindings[resource].queue_uses});
    }

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

    void FrameResources::invalidateImportedBindings() noexcept
    {
        record_ready = false;
        // Unique cache revisions prevent a reused instance address from matching an older binding cache.
        static std::atomic<std::uint64_t> next_revision{1};
        imported_revision = next_revision.fetch_add(1, std::memory_order_relaxed);
    }

    void FrameResources::invalidatePass(const compiled::RenderGraph& graph, std::uint32_t compiled_pass) noexcept
    {
        pending_passes.mark(compiled_pass);
        record_ready = false;
        if (command_batches.empty()) return; // Inputs can precede initialize().
        const auto batch = graph.passes()[compiled_pass].submission;
        if (compiled_pass < graph.submissions()[batch].output_begin) command_batches[batch].invalidate();
    }

    void FrameResources::invalidateResource(const RecordingDependencies& dependencies, std::uint32_t resource) noexcept
    {
        pending_resources.mark(resource);
        for (const auto field : dependencies.resource_operand_fields[resource]) pending_operand_fields.mark(field);
        for (const auto p : dependencies.resource_passes[resource]) pending_passes.mark(p);
        record_ready = false;
        if (command_batches.empty()) return;
        for (const auto batch : dependencies.resource_batches[resource])
            command_batches[batch].invalidate();
    }

    void FrameResources::invalidateRecordings() noexcept
    {
        pending_passes.markAll();
        pending_operand_fields.markAll();
        record_ready = false;
        for (auto& batch : command_batches) batch.invalidate();
    }

    void FrameResources::prepareDrawReads() noexcept
    {
        for (auto& draws : raster_draws)
        {
            if (draws.dependency_reads != draws.reads)
            {
                draws.dependency_reads = draws.reads;
                invalidateImportedBindings();
            }
        }
    }

    void FrameResources::patchDrawSection(std::uint32_t p, const RasterListCommands& dynamic) noexcept
    {
        const auto& pass = passes[p];
        auto& draws = raster_draws[dynamic.data_index];
        const auto& native = record_passes[p].native;
        if (draws.has_explicit_sections || !draws.sections.empty())
        {
            draws.implicit_section.reset();
            return;
        }
        draws.colors.assign(pass.colors.begin(), pass.colors.end());
        auto& section = draws.implicit_section.emplace();
        section.color_count = static_cast<std::uint32_t>(draws.colors.size());
        section.depth = pass.depth;
        section.viewport = native.viewport;
        section.parameter_size = dynamic.producer_stream ? 0 : static_cast<std::uint32_t>(draws.parameters.size());
        section.draw_count = static_cast<std::uint32_t>(draws.packets.size());
        for (auto& packet : draws.packets) packet.scissor = native.scissor;
    }

    namespace
    {
        // On a mixed join the skipped path can still hold the desired bindings. Each fixed pass
        // establishes its complete layout and its shared sets. A helper or stream clears the predecessor.
        FixedBindingDelta joinBindings(
            FixedBindingDelta delta, const NativeProgramSlot& target, const CompiledPass& previous,
            const NativeProgramBindings& programs
        ) noexcept
        {
            const auto& prior = programs[previous.binding_slot];
            delta.pool = false;
            if (prior.pipeline == target.pipeline) delta.pipeline = false;
            const auto& binding = target.descriptors;
            const bool same_layout = previous.fixed_bindings.point == delta.point &&
                prior.descriptors.layout == binding.layout;
            if (same_layout)
            {
                delta.layout = false;
                for (auto mask = delta.shared_sets; mask; mask &= mask - 1)
                {
                    const auto kind = std::countr_zero(mask);
                    if (prior.descriptors.shared_sets[kind] == binding.shared_sets[kind])
                        delta.shared_sets &= static_cast<std::uint8_t>(~(1u << kind));
                }
            }
            return delta;
        }
    } // namespace

    bool FrameResources::selectBindingJoin(
        const compiled::RenderGraph& graph, std::uint32_t index, bool enabled
    ) noexcept
    {
        const auto& join = graph.recordingProgram().binding_joins[index];
        auto& choice = binding_choices[index];
        auto predecessor = enabled ? join.enabled_predecessor : join.disabled_predecessor;
        auto commands = enabled ? join.enabled : join.disabled;
        if (!enabled && join.disabled_join != kGraphInvalidIndex)
        {
            predecessor = binding_choices[join.disabled_join].predecessor;
            if (predecessor != kGraphInvalidIndex)
            {
                const auto& pass = graph.passes()[join.pass];
                commands = joinBindings(commands, graph.programs()[pass.binding_slot],
                    graph.passes()[predecessor], graph.programs());
            }
        }
        const bool changed = predecessor != choice.predecessor;
        choice = {predecessor, commands};
        return changed;
    }

    void FrameResources::linkRecordInputs(const compiled::RenderGraph& graph) noexcept
    {
        const auto& recording = graph.recordingProgram();
        binding_choices.reserve(recording.binding_joins.size());
        for (const auto& join : recording.binding_joins)
            binding_choices.push_back({join.enabled_predecessor, join.enabled});
        for (std::uint32_t i = 0; i < passes.size(); ++i)
        {
            const auto& pass = passes[i];
            auto& record = record_passes[i];
            record.images = pass.images;
            record.buffers = pass.buffers;
            record.parameters = pass.parameters;
            auto& native = record.native;
            native.colors = pass.colors;
            if (const auto* compute = recording.computeCommands(*pass.compiled_pass))
            {
                if (const auto* fixed = std::get_if<nri::DispatchDesc>(&compute->dispatch)) native.dispatch = *fixed;
                continue;
            }
            const auto* raster = recording.rasterCommands(*pass.compiled_pass);
            const auto* list = raster ? std::get_if<RasterListCommands>(&raster->draw) : nullptr;
            if (list) native.draws = &raster_draws[list->data_index];
        }
        linked_recording_.emplace(FrozenRecordingFrame{{
            graph, transfers, record_resources, record_passes, history_initializations.values(), enabled_regions,
            binding_choices, final_images, final_buffers, native.descriptor_pool, shared_descriptors.sets
        }, command_batches, native.batches});
    }

    void FrameResources::patchRecordResource(std::uint32_t i, std::uint32_t history) noexcept
    {
        const auto& binding = bindings[i];
        auto& resource = record_resources[i];
        resource.buffer = binding.buffer;
        resource.buffer_offset = binding.buffer_offset;
        resource.buffer_size = binding.buffer_size;
        resource.image = binding.image;
        resource.history_valid = history == kGraphInvalidIndex || history_valid[history] != 0;
        resource.width = resource.height = 0;
        if (binding.image)
        {
            const auto& description = *binding.image_description;
            resource.width = description.width;
            resource.height = description.height;
        }
    }

    void FrameResources::patchRecordPass(const RecordingProgram& program, std::uint32_t i) noexcept
    {
        auto& pass = passes[i];
        auto& record = record_passes[i];
        record.alias_barrier = pass.alias_barrier;
        record.private_descriptors = pass.private_descriptors;
        auto& native = record.native;
        native.depth = pass.depth;
        if (const auto* scope = program.rasterScope(*pass.compiled_pass))
        {
            const auto& attachment = scope->colors.empty() ? *scope->depth : scope->colors.front();
            const auto& image = record_resources[attachment.access.version.resource.index];
            const auto mip = std::get<GraphImageRange>(attachment.access.range).first_mip;
            const auto width = std::max(1u, image.width >> mip);
            const auto height = std::max(1u, image.height >> mip);
            const auto& area = scope->viewport ? *scope->viewport : viewport;
            native.viewport = {
                area.x * width, area.y * height, area.width * width, area.height * height,
                area.min_depth, area.max_depth, true
            };
            native.scissor = {
                static_cast<std::int16_t>(area.x * width), static_cast<std::int16_t>(area.y * height),
                static_cast<nri::Dim_t>(area.width * width), static_cast<nri::Dim_t>(area.height * height)
            };
        }
        if (native.draws)
        {
            const auto& raster = *program.rasterCommands(*pass.compiled_pass);
            patchDrawSection(i, std::get<RasterListCommands>(raster.draw));
        }
    }

    void TransferOperands::initialize(const TransferProgram& program) noexcept
    {
        buffer_copies.resize(program.buffer_copies.size());
        for (std::size_t i = 0; i < buffer_copies.size(); ++i)
            buffer_copies[i].bytes = program.buffer_copies[i].source.range.size;
        image_copies.resize(program.image_copies.size());
        for (std::size_t i = 0; i < image_copies.size(); ++i)
        {
            image_copies[i].mip_count = program.image_copies[i].source.range.mip_count;
            image_copies[i].layer_count = program.image_copies[i].source.range.layer_count;
        }
        image_buffers.resize(program.image_buffers.size());
        fills.resize(program.fills.size());
        clears.resize(program.clears.size());
        for (std::size_t i = 0; i < clears.size(); ++i)
        {
            const auto& input = program.clears[i];
            auto& clear = clears[i];
            clear.loadOp = nri::LoadOp::CLEAR;
            clear.storeOp = nri::StoreOp::STORE;
            if (input.destination.range.aspects != 1) clear.clearValue.depthStencil.depth = input.depth;
            else clear.clearValue.color.f = {input.color[0], input.color[1], input.color[2], input.color[3]};
        }
    }

    RenderResult<void> FrameResources::patchOperandFields(
        const RecordingProgram& recording, const RecordingDependencies& dependencies, const nri::DeviceDesc& limits
    ) noexcept
    {
        using Field = RecordingDependencies::EOperandField;
        const auto& fields = dependencies.operand_fields;
        const auto& memory = limits.memoryAlignment;
        const CopyAlignment alignment{memory.uploadBufferTextureRow, memory.uploadBufferTextureSlice};
        const TransferSources sources{record_resources, alignment};
        const auto& program = recording.transfers;
        FrozenOperandPatches patched{};
        for (const auto index : pending_operand_fields.slots())
        {
            const auto [field, p, row] = fields[index];
            const auto& pass = passes[p];
            const auto condition = pass.compiled_pass->condition_region;
            if (condition != kGraphInvalidIndex && !enabled_regions[condition]) continue;
            switch (field)
            {
            case Field::INDIRECT_DISPATCH:
            {
                const auto& location = std::get<BufferLocation>(recording.compute[row].dispatch);
                const auto& buffer = record_resources[location.buffer];
                if (!containsBytes(buffer, location.offset, sizeof(nri::DispatchDesc)))
                    return cxx::unexpected(RenderError{kDeviceArgument});
                const auto offset = buffer.buffer_offset + location.offset;
                if (offset % 4) return cxx::unexpected(RenderError{kDeviceArgument});
                record_passes[p].native.indirect_buffer = buffer.buffer;
                record_passes[p].native.indirect_offset = offset;
                ++patched.compute;
                continue;
            }
            case Field::IMAGE_DISPATCH:
            {
                const auto& extent = std::get<ImageDispatchExtent>(recording.compute[row].dispatch);
                const auto& image = record_resources[extent.image];
                auto groups = dispatchGroups(extent, {image.width, image.height}, limits);
                if (!groups) return cxx::unexpected(groups.error());
                record_passes[p].native.dispatch = *groups;
                ++patched.compute;
                continue;
            }
            case Field::BUFFER_SOURCE:
            case Field::BUFFER_DESTINATION:
            {
                const auto& copy = program.buffer_copies[row];
                const bool source = field == Field::BUFFER_SOURCE;
                const auto& operand = source ? copy.source : copy.destination;
                const auto& buffer = sources.selected(operand);
                auto& native = transfers.buffer_copies[row];
                if (!containsBytes(buffer, operand.range.offset, native.bytes))
                    return cxx::unexpected(RenderError{kDeviceArgument});
                (source ? native.source : native.destination) = buffer.buffer;
                auto& offset = source ? native.source_offset : native.destination_offset;
                offset = buffer.buffer_offset + operand.range.offset;
                break;
            }
            case Field::IMAGE_COPY:
            {
                const auto& copy = program.image_copies[row];
                if (auto patched_copy = sources.patch(copy, transfers.image_copies[row]); !patched_copy)
                    return patched_copy;
                break;
            }
            case Field::IMAGE_BUFFER:
            {
                const auto& copy = program.image_buffers[row];
                if (auto patched_copy = sources.patch(copy, transfers.image_buffers[row]); !patched_copy)
                    return patched_copy;
                break;
            }
            case Field::FILL:
            {
                const auto& fill = program.fills[row];
                const auto& operand = fill.destination;
                const auto& buffer = sources.selected(operand);
                auto& native = transfers.fills[row];
                if (!containsBytes(buffer, operand.range.offset, operand.range.size))
                    return cxx::unexpected(RenderError{kDeviceArgument});
                native.offset = buffer.buffer_offset + operand.range.offset;
                if (native.offset % 4) return cxx::unexpected(RenderError{kDeviceArgument});
                native.buffer = buffer.buffer;
                if (fill.descriptor != kGraphInvalidIndex)
                {
                    auto* view = pass.private_views[fill.descriptor];
                    if (!view) view = pass.history_views[fill.descriptor];
                    if (!view) return cxx::unexpected(RenderError{kDeviceArgument});
                    native.storage_view = view;
                }
                break;
            }
            case Field::CLEAR:
            {
                auto* view = pass.clear_views[program.clears[row].view];
                if (!view) return cxx::unexpected(RenderError{kDeviceArgument});
                transfers.clears[row].descriptor = view;
                break;
            }
            }
            ++patched.transfer;
        }
        last_operands = patched;
        pending_operand_fields.clear();
        return {};
    }

    RenderResult<void> FrameResources::freeze(std::span<const HistoryState> histories, const nri::DeviceDesc& limits) noexcept
    {
        record_ready = false;
        if (!linked_recording_) return cxx::unexpected(RenderError{kDeviceArgument});
        auto& input = linked_recording_->input;
        const auto& graph = input.plan;
        if (!raster_draws.empty()) prepareDrawReads();
        for (const auto i : pending_resources.slots()) patchRecordResource(i, graph.resources()[i].history);
        for (const auto i : pending_passes.slots()) patchRecordPass(graph.recordingProgram(), i);
        if (pending_operand_fields.slots().empty()) last_operands = {};
        else
        {
            auto patched = patchOperandFields(graph.recordingProgram(), graph.recordingDependencies(), limits);
            if (!patched) return patched;
        }
        last_patches = {pending_resources.slots().size(), pending_passes.slots().size()};
        pending_resources.clear();
        pending_passes.clear();
        if (!pending_history_clears.slots().empty())
        {
            freezeHistoryInitializations(histories, graph.resources(), limits.graphicsAPI);
            input.history_initializations = history_initializations.values();
        }
        input.descriptor_pool = native.descriptor_pool;
        input.shared_descriptors = shared_descriptors.sets;
        record_ready = true;
        return {};
    }
} // namespace lux::render::detail
