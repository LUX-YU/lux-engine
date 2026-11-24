#include <lux/engine/render/frame/FrameError.hpp>
#include <lux/engine/render/graph/execute/PhysicalFrameAllocation.hpp>
#include <lux/engine/render/memory/Cleanup.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <cstring>
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
        bool containsBytes(const FrameResourceBinding& resource, std::uint64_t offset, std::uint64_t count) noexcept
        {
            const bool fits = offset <= resource.buffer_size && count <= resource.buffer_size - offset;
            const bool addressable = offset <= UINT64_MAX - resource.buffer_offset &&
                count <= UINT64_MAX - (resource.buffer_offset + offset);
            return resource.buffer && fits && addressable;
        }

        struct TransferSources final
        {
            std::span<const FrameResourceBinding> bindings;
            std::span<const CompiledResource> resources;
            std::span<const std::uint8_t> history_valid;
            CopyAlignment alignment;

            [[nodiscard]] const FrameResourceBinding& selected(const auto& operand) const noexcept
            {
                const auto history = resources[operand.slot].history;
                const bool fallback = operand.fallback != kGraphInvalidIndex &&
                    history != kGraphInvalidIndex && !history_valid[history];
                return bindings[fallback ? operand.fallback : operand.slot];
            }

            [[nodiscard]] RenderResult<nri::TextureRegionDesc> region(const ImageOperand& operand) const noexcept
            {
                const auto& image = selected(operand);
                if (!image.image) return cxx::unexpected(RenderError{kDeviceArgument});
                const auto& desc = *image.image_description;
                return copyRegion(operand, {desc.width, desc.height, desc.depth});
            }

            [[nodiscard]] RenderResult<void>
            patch(const ImageCopyPacket& copy, TextureCopyRegion& output) const noexcept
            {
                auto from = region(copy.source);
                if (!from) return cxx::unexpected(from.error());
                auto to = region(copy.destination);
                if (!to) return cxx::unexpected(to.error());
                if (from->width != to->width || from->height != to->height || from->depth != to->depth)
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
                const auto bytes = std::uint64_t{layout->slicePitch} * rect->depth;
                if (!containsBytes(buffer, copy.buffer.range.offset, bytes))
                    return cxx::unexpected(RenderError{kDeviceArgument});
                output = {selected(copy.image).image, buffer.buffer, *rect, *layout};
                const auto rows = (std::uint32_t{rect->height} + copy.image.block_dim - 1) / copy.image.block_dim;
                // A placed footprint steps by its packed row count on D3D12. Encode padding between Z planes explicitly.
                if (rect->depth > 1 && layout->slicePitch != std::uint64_t{layout->rowPitch} * rows)
                {
                    output.slice_copies = rect->depth;
                    output.region.depth = 1;
                }
                return {};
            }
        };

    } // namespace

    void FrameResources::initializeRows(const compiled::RenderGraph& plan) noexcept
    {
        parameters = plan.bindingPlan().initial_parameters;
        bindings.resize(plan.resources().size());
        raster_vertices.resize(plan.recordingProgram().raster_vertices.size());
        raster_addresses.resize(plan.recordingProgram().raster_addresses.size());
        dispatches.resize(plan.recordingProgram().compute.size());
        pending_passes.initialize(plan.passes().size());
        pending_operand_fields.initialize(plan.bindingPlan().recordings.operand_fields.size());
        transfers.initialize(plan.recordingProgram().transfers);
        const auto clear_capacity = plan.histories().size() * 2;
        history_initializations.prepareCapacity(clear_capacity, clear_capacity);
        pending_history_clears.initialize(plan.histories().size());
        pending_history_views.initialize(plan.histories().size());
        enabled_regions.resize(plan.recordingProgram().conditions.size(), 1);
        enabled_pass_count = static_cast<std::uint32_t>(plan.passes().size());
        history_valid.resize(plan.histories().size(), 0);
        conditions.resize(plan.conditionCount());
        pending_condition_inputs.initialize(plan.conditionCount());
        pending_regions.initialize(enabled_regions.size());
        history_clears.resize(plan.histories().size());
        changed_resources.reserve(plan.resources().size());
        unavailable_buffers = plan.recordingDependencies().required_buffers;
        native.images.reserve(plan.recordingDependencies().owned_image_capacity);
    }

    void FrameResources::collectRasterViews(
        nri::Buffer* buffer, nri::Texture* image, std::vector<nri::Descriptor*>& retired
    ) noexcept
    {
        if (!buffer && !image) return;
        if (image)
        {
            const auto removed = std::erase_if(native.raster_attachment_views, [&](const auto& attachment) noexcept
            {
                if (attachment.image != image) return false;
                retired.push_back(attachment.view);
                return true;
            });
            // Layer views can be shared by several rendering scopes in this FIF.
            if (removed) invalidateRecordings();
        }
        for (auto& record : native.raster_bindings)
        {
            for (std::size_t i = 0; i < record.inputs.size(); ++i)
            {
                const auto& input = record.inputs[i];
                const bool matches = (buffer && input.buffer == buffer) || (image && input.image == image);
                if (!matches) continue;
                if (auto* view = std::exchange(record.views[i], nullptr)) retired.push_back(view);
                record.inputs[i] = {};
                invalidatePass(recordingGraph(), record.pass);
            }
        }
    }

    void FrameResources::retireRasterViews(DeviceSession& session, nri::Buffer* buffer, nri::Texture* image) noexcept
    {
        std::vector<nri::Descriptor*> views;
        collectRasterViews(buffer, image, views);
        if (!views.empty())
        {
            session.deferRelease([core = &session.nriCore(), views = std::move(views)]() noexcept
            {
                for (auto* view : views) core->DestroyDescriptor(view);
            });
        }
    }

    RenderResult<void> FrameResources::bindDynamicBuffer(
        const compiled::RenderGraph& graph, DeviceSession& device, std::size_t slot, std::uint64_t size
    ) noexcept
    {
        const auto& contract = graph.dynamicBuffers()[slot];
        if (size < contract.minimum_bytes || size > contract.maximum_bytes || size % 4)
            return cxx::unexpected(RenderError{kFrameContract, {kGraphInvalidIndex, contract.resource}});
        auto& replica = native.prepared_buffers[slot];
        if (size > replica.capacity)
        {
            if (replica.generation == UINT64_MAX) return cxx::unexpected(RenderError{kDeviceCapacity});
            const auto capacity = size > std::bit_floor(UINT64_MAX) ?
                contract.maximum_bytes : std::min(std::bit_ceil(size), contract.maximum_bytes);
            const auto description = graphBufferDescription(device, graph.resources()[contract.resource], capacity);
            if (!description) return cxx::unexpected(description.error());
            nri::Buffer* candidate = nullptr;
            const auto result =
                device.nriCore().CreatePlacedBuffer(device.device(), NriDeviceHeap, *description, candidate);
            if (result != nri::Result::SUCCESS) return cxx::unexpected(nativeError(result));
            bindResource(graph, device, contract.resource, {candidate, 0, size});
            releaseAfterGpu(device, replica.buffer);
            replica.buffer = candidate;
            ++replica.generation;
            replica.capacity = capacity;
            replica.accepted = {};
        }
        else
        {
            const auto& current = bindings[contract.resource];
            if (current.buffer != replica.buffer || current.buffer_size != size)
                bindResource(graph, device, contract.resource, {replica.buffer, 0, size});
        }
        return {};
    }

    void InstalledGraphBindings::bind(GraphResourceId resource, std::uint32_t slot, const ResourceInput& value) noexcept
    {
        const auto first = slot == UINT32_MAX ? 0 : slot;
        const auto end = slot == UINT32_MAX ? static_cast<std::uint32_t>(frames.size()) : slot + 1;
        for (auto current = first; current < end; ++current)
            frames[current].bindResource(graph, device, resource.index, value);
    }

    RenderResult<void>
    InstalledGraphBindings::qualifyImage(GraphResourceId resource, const nri::Texture& image) const noexcept
    {
        return detail::qualifyImportedImage(device, graph.resources()[resource.index], image, target);
    }

    void FrameResources::bindResource(
        const compiled::RenderGraph& graph, DeviceSession& session, std::uint32_t resource, const ResourceInput& value
    ) noexcept
    {
        auto& binding = bindings[resource];
        const bool changed_physical = !binding.sameBacking(value);
        const bool changed_addresses = !binding.sameAddresses(value);
        if (frozen_for_submission)
        {
            const auto pending = std::ranges::find(pending_bindings_, resource, &PendingBinding::resource);
            if (!changed_physical && !changed_addresses && binding.queue_uses == value.queue_uses)
            {
                // A publication can also return to the frozen value before the receipt arrives.
                if (pending != pending_bindings_.end()) pending_bindings_.erase(pending);
                return;
            }
            if (pending == pending_bindings_.end()) pending_bindings_.push_back({resource, value});
            else pending->value = value;
            return;
        }
        binding.addressed_buffers = value.addressed_buffers;
        binding.addressed_revision = value.addressed_revision;
        binding.queue_uses = value.queue_uses;
        if (changed_addresses) record_ready = false;
        if (!changed_physical)
        {
            binding.select(value.buffer, value.image, session.nriCore());
            return;
        }
        if (graph.resources()[resource].memory_used &&
            std::holds_alternative<GraphBufferDesc>(graph.resources()[resource].description))
        {
            if (!binding.buffer && value.buffer)
            {
                --unavailable_buffers;
            }
            else if (binding.buffer && !value.buffer)
            {
                ++unavailable_buffers;
            }
        }
        // A pool rebuild must replay only views whose owners still hold the allocation.
        retireRasterViews(session, binding.buffer != value.buffer ? binding.buffer : nullptr,
            binding.image != value.image ? binding.image : nullptr);
        binding.select(value.buffer, value.image, session.nriCore());
        binding.buffer_offset = value.offset;
        binding.buffer_size = value.size;
        invalidateResource(graph.recordingDependencies(), resource);
        if (!binding.dirty)
        {
            binding.dirty = true;
            changed_resources.push_back(resource);
        }
    }

    void FrameResources::applyInlineConstant(
        const compiled::RenderGraph& graph, CompiledPassSlot pass,
        std::span<const std::byte> bytes, std::uint64_t revision
    ) noexcept
    {
        auto& selected = passes[pass.index];
        const auto destination = selected.parameters;
        if (revision != 0 && revision == selected.parameter_source_revision) return;
        selected.parameter_source_revision = revision;
        if (std::memcmp(destination.data(), bytes.data(), bytes.size()) != 0)
        {
            std::memcpy(destination.data(), bytes.data(), bytes.size());
            invalidatePass(graph, pass.index);
        }
    }

    void FrameResources::prepareAliasBarriers(const PhysicalResourcePlan& plan) noexcept
    {
        if (memory_bindings.empty()) return;
        const auto barriers = instantiateAliasBarriers(
            plan, memory_bindings, passes.size()
        );
        for (std::size_t p = 0; p < barriers.size(); ++p) passes[p].alias_barrier = barriers[p];
    }

    void FrameResources::finishSubmission(const compiled::RenderGraph& graph, DeviceSession& session) noexcept
    {
        frozen_for_submission = false;
        for (const auto& update : pending_bindings_) bindResource(graph, session, update.resource, update.value);
        pending_bindings_.clear();
    }

    void mergeIndexedIndirectDraws(RasterDrawList& draws, std::uint32_t limit) noexcept
    {
        if (limit <= 1 || draws.packets.size() < 2) return;
        const auto same_state = [&](const RasterDrawPacket& a, const RasterDrawPacket& b) noexcept
        {
            const bool same_pipeline = a.pipeline == b.pipeline && a.bindings == b.bindings;
            const bool same_index = a.index == b.index && a.index_offset == b.index_offset &&
                a.index_type == b.index_type;
            const bool same_scissor = a.scissor.x == b.scissor.x && a.scissor.y == b.scissor.y &&
                a.scissor.width == b.scissor.width && a.scissor.height == b.scissor.height;
            const bool same_counts = a.descriptor_set == b.descriptor_set && a.vertex_count == b.vertex_count &&
                a.parameter_size == b.parameter_size;
            if (!same_pipeline || !same_index || !same_scissor || !same_counts) return false;
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

    void FrameResources::invalidatePass(const compiled::RenderGraph& graph, std::uint32_t compiled_pass) noexcept
    {
        pending_passes.mark(compiled_pass);
        record_ready = false;
        if (command_batches.empty()) return; // Inputs can precede initialize().
        const auto batch = graph.passes()[compiled_pass].submission;
        if (graph.submissions()[batch].reusesPass(compiled_pass)) command_batches[batch].invalidate();
    }

    void FrameResources::invalidateResource(const RecordingDependencies& dependencies, std::uint32_t resource) noexcept
    {
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

    void FrameResources::patchDrawSection(std::uint32_t p, const RasterListCommands& dynamic) noexcept
    {
        const auto& pass = passes[p];
        auto& draws = raster_draws[dynamic.data_index];
        if (draws.has_explicit_sections || !draws.sections.empty())
        {
            draws.implicit_section.reset();
            return;
        }
        draws.colors.assign(pass.colors.begin(), pass.colors.end());
        auto& section = draws.implicit_section.emplace();
        section.color_count = static_cast<std::uint32_t>(draws.colors.size());
        section.depth = pass.depth;
        section.viewport = pass.viewport;
        section.parameter_size = dynamic.producer_stream ? 0 : static_cast<std::uint32_t>(draws.parameters.size());
        section.draw_count = static_cast<std::uint32_t>(draws.packets.size());
        if (!dynamic.producer_stream)
            for (auto& packet : draws.packets) packet.scissor = pass.scissor;
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
        for (std::size_t i = 0; i < recording.compute.size(); ++i)
            if (const auto* fixed = std::get_if<nri::DispatchDesc>(&recording.compute[i].dispatch))
                dispatches[i].dispatch = *fixed;
        linked_recording_.emplace(FrozenRecordingFrame{
            graph, transfers, raster_vertices, raster_addresses, passes, dispatches, raster_draws,
            history_initializations.values(), enabled_regions,
            binding_choices, entry_images, entry_buffers, exit_images, exit_buffers, native.descriptor_pool, shared_descriptors.sets,
            command_batches, native.batches});
    }

    void FrameResources::patchRecordPass(const RecordingProgram& program, std::uint32_t i) noexcept
    {
        auto& pass = passes[i];
        if (const auto* scope = program.rasterScope(*pass.compiled_pass))
        {
            const auto& attachment = scope->colors.empty() ? *scope->depth : scope->colors.front();
            const auto* image = bindings[attachment.access.version.resource.index].image_description;
            const auto mip = std::get<GraphImageRange>(attachment.access.range).first_mip;
            const auto width = image ? std::max(1u, std::uint32_t{image->width} >> mip) : 1u;
            const auto height = image ? std::max(1u, std::uint32_t{image->height} >> mip) : 1u;
            const auto& area = scope->viewport ? *scope->viewport : viewport;
            pass.viewport = {
                area.x * width, area.y * height, area.width * width, area.height * height,
                area.min_depth, area.max_depth, true
            };
            pass.scissor = {
                static_cast<std::int16_t>(area.x * width), static_cast<std::int16_t>(area.y * height),
                static_cast<nri::Dim_t>(area.width * width), static_cast<nri::Dim_t>(area.height * height)
            };
        }
        if (const auto* raster = program.rasterCommands(*pass.compiled_pass))
            if (const auto* stream = std::get_if<RasterListCommands>(&raster->draw)) patchDrawSection(i, *stream);
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
        const TransferSources sources{bindings, recordingGraph().resources(), history_valid, alignment};
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
            case Field::RASTER_VERTEX:
            {
                const auto& vertex = recording.raster_vertices[row];
                const auto& buffer = bindings[vertex.buffer];
                if (!containsBytes(buffer, vertex.offset, 1)) return cxx::unexpected(RenderError{kDeviceArgument});
                raster_vertices[row] = {
                    vertex.binding, {buffer.buffer, buffer.buffer_offset + vertex.offset, vertex.stride}
                };
                continue;
            }
            case Field::RASTER_ADDRESS:
            {
                const auto& address = recording.raster_addresses[row];
                const auto& buffer = bindings[address.location.buffer];
                if (!containsBytes(buffer, address.location.offset, address.bytes))
                    return cxx::unexpected(RenderError{kDeviceArgument});
                const auto offset = buffer.buffer_offset + address.location.offset;
                if (offset % address.alignment) return cxx::unexpected(RenderError{kDeviceArgument});
                raster_addresses[row] = {buffer.buffer, offset};
                continue;
            }
            case Field::INDIRECT_DISPATCH:
            {
                const auto& location = std::get<BufferLocation>(recording.compute[row].dispatch);
                const auto& buffer = bindings[location.buffer];
                if (!containsBytes(buffer, location.offset, sizeof(nri::DispatchDesc)))
                    return cxx::unexpected(RenderError{kDeviceArgument});
                const auto offset = buffer.buffer_offset + location.offset;
                if (offset % 4) return cxx::unexpected(RenderError{kDeviceArgument});
                dispatches[row].indirect_buffer = buffer.buffer;
                dispatches[row].indirect_offset = offset;
                ++patched.compute;
                continue;
            }
            case Field::IMAGE_DISPATCH:
            {
                const auto& extent = std::get<ImageDispatchExtent>(recording.compute[row].dispatch);
                const auto* image = bindings[extent.image].image_description;
                if (!image) return cxx::unexpected(RenderError{kDeviceArgument});
                auto groups = dispatchGroups(extent, {image->width, image->height, image->depth}, limits);
                if (!groups) return cxx::unexpected(groups.error());
                dispatches[row].dispatch = *groups;
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
        auto& input = *linked_recording_;
        const auto& graph = input.plan;
        for (const auto i : pending_passes.slots()) patchRecordPass(graph.recordingProgram(), i);
        if (pending_operand_fields.slots().empty()) last_operands = {};
        else
        {
            auto patched = patchOperandFields(graph.recordingProgram(), graph.recordingDependencies(), limits);
            if (!patched) return patched;
        }
        last_patched_passes = pending_passes.slots().size();
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
