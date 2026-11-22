#include <algorithm>
#include <cstring>
#include <lux/engine/render/graph/execute/CommandExtent.hpp>
#include <lux/engine/render/graph/execute/BindingState.hpp>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/execute/FrameExecutor.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>

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
                if (!primary.history_valid && operand.fallback != kGraphInvalidIndex) return resources[operand.fallback];
                return primary;
            }

            [[nodiscard]] RenderResult<nri::TextureRegionDesc> region(const ImageOperand& operand) const noexcept
            {
                const auto& image = selected(operand);
                if (!image.image) return cxx::unexpected(RenderError{kDeviceArgument});
                return copyRegion(operand, {image.width, image.height});
            }

            [[nodiscard]] RenderResult<void> patch(const ImageCopyPacket& copy, TextureCopyRegion& output) const noexcept
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

            [[nodiscard]] RenderResult<void> patch(const ImageBufferPacket& copy, FrozenImageBuffer& output) const noexcept
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

        RenderResult<void> freezeCompute(
            const ComputeCommands& packet, std::span<const PassResourceInput> resources,
            const nri::DeviceDesc& device, PassNativeInput& native
        ) noexcept
        {
            if (const auto* indirect = std::get_if<BufferLocation>(&packet.dispatch))
            {
                const auto& buffer = resources[indirect->buffer];
                if (!containsBytes(buffer, indirect->offset, sizeof(nri::DispatchDesc)))
                    return cxx::unexpected(RenderError{kDeviceArgument});
                native.indirect_buffer = buffer.buffer;
                native.indirect_offset = buffer.buffer_offset + indirect->offset;
                if (native.indirect_offset % 4) return cxx::unexpected(RenderError{kDeviceArgument});
            }
            else if (const auto* fixed = std::get_if<nri::DispatchDesc>(&packet.dispatch)) native.dispatch = *fixed;
            else
            {
                const auto& extent = std::get<ImageDispatchExtent>(packet.dispatch);
                const auto& image = resources[extent.image];
                auto groups = dispatchGroups(extent, {image.width, image.height}, device);
                if (!groups) return cxx::unexpected(groups.error());
                native.dispatch = *groups;
            }
            return {};
        }

    } // namespace

    RenderResult<std::span<nri::CommandBuffer* const>>
    RenderGraphInstance::recordBatch(std::uint32_t frame_slot, std::uint32_t batch) noexcept
    {
        if (const auto error = session_->terminalError(); error.type) return cxx::unexpected(error);
        auto frame = recordingFrame(frame_slot);
        if (!frame) return cxx::unexpected(frame.error());
        return FrameExecutor::recordBatch(*frame, *session_, batch);
    }

    RenderResult<FrozenRecordingFrame> RenderGraphInstance::recordingFrame(std::uint32_t frame_slot) noexcept
    {
        if (frame_slot >= frames_.size() || !frames_[frame_slot].record_ready)
            return cxx::unexpected(RenderError{kDeviceArgument});
        auto& frame = frames_[frame_slot];
        return FrozenRecordingFrame{{
            graph_, frame.transfers, frame.record_resources, frame.record_passes, frame.history_initializations.values(),
            frame.enabled_regions, frame.final_images, frame.final_buffers,
            frame.native.descriptor_pool, frame.shared_descriptors.sets
        }, frame.command_batches, frame.native.batches};
    }

    void RenderGraphInstance::invalidateRecording(std::uint32_t frame_slot, std::uint32_t compiled_pass) noexcept
    {
        auto& frame = frames_[frame_slot];
        frame.pending_passes.mark(compiled_pass);
        frame.record_ready = false;
        if (frame.command_batches.empty()) return; // Inputs can precede initialize().
        const auto batch = graph_.passes()[compiled_pass].submission;
        if (compiled_pass < graph_.submissions()[batch].output_begin) frame.command_batches[batch].invalidate();
    }

    void RenderGraphInstance::invalidateResourceRecordings(std::uint32_t slot, std::uint32_t resource) noexcept
    {
        auto& frame = frames_[slot];
        frame.pending_resources.mark(resource);
        const auto& dependencies = graph_.binding_plan_->recordings;
        for (const auto field : dependencies.resource_transfer_fields[resource]) frame.pending_transfer_fields.mark(field);
        for (const auto p : dependencies.resource_passes[resource]) frame.pending_passes.mark(p);
        frame.record_ready = false;
        if (frame.command_batches.empty()) return;
        for (const auto batch : dependencies.resource_batches[resource])
            frame.command_batches[batch].invalidate();
    }

    void RenderGraphInstance::invalidateRecordings(std::uint32_t frame_slot) noexcept
    {
        frames_[frame_slot].pending_passes.markAll();
        frames_[frame_slot].pending_transfer_fields.markAll();
        frames_[frame_slot].record_ready = false;
        for (auto& batch : frames_[frame_slot].command_batches) batch.invalidate();
    }

    void RenderGraphInstance::invalidateRasterDraws(const CompiledPass& pass, std::uint32_t frame_slot) noexcept
    {
        invalidateRecording(frame_slot, pass.binding_slot);
    }

    RenderResult<void> RenderGraphInstance::createRecordings(std::uint32_t frame_slot) noexcept
    {
        auto& frame = frames_[frame_slot];
        const auto batches = graph_.submissions();
        frame.command_batches.resize(batches.size());
        frame.native.batches.resize(batches.size());
        const auto& core = session_->nriCore();
        for (std::size_t batch = 0; batch < batches.size(); ++batch)
        {
            const auto& description = batches[batch];
            const bool has_tail = description.output_begin != kGraphInvalidIndex;
            const std::uint32_t first_part = has_tail && description.output_begin == description.first_pass ? 1u : 0u;
            const std::uint32_t end_part = has_tail ? 2u : 1u;
            for (auto part = first_part; part < end_part; ++part)
            {
                auto result = core.CreateCommandAllocator(
                    session_->queue(description.queue).native(), frame.native.batches[batch].allocators[part]
                );
                if (result == nri::Result::SUCCESS)
                {
                    result = core.CreateCommandBuffer(
                        *frame.native.batches[batch].allocators[part], frame.native.batches[batch].commands[part]
                    );
                }
                if (result != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
            }
        }
        return {};
    }

    RasterDrawList&
    RenderGraphInstance::rasterDraws(const CompiledPass& pass, std::uint32_t slot, bool recording_changed) noexcept
    {
        if (recording_changed) invalidateRasterDraws(pass, slot);
        const auto& raster = *graph_.rasterCommands(pass);
        return frames_[slot].raster_draws[std::get<RasterListCommands>(raster.draw).data_index];
    }

    void RenderGraphInstance::prepareStreamedDraws(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        for (const auto p : graph_.binding_plan_->recordings.dynamic_passes)
        {
            const auto& pass = frame.passes[p];
            const auto& compiled = *pass.compiled_pass;
            const auto& dynamic = std::get<RasterListCommands>(graph_.rasterCommands(compiled)->draw);
            auto& draws = frame.raster_draws[dynamic.data_index];
            if (draws.dependency_reads != draws.reads)
            {
                draws.dependency_reads = draws.reads;
                frame.invalidateImportedBindings();
            }
        }
    }

    void RenderGraphInstance::patchDrawSection(FrameResources& frame, std::uint32_t p) noexcept
    {
        const auto& pass = frame.passes[p];
        const auto& compiled = *pass.compiled_pass;
        const auto& dynamic = std::get<RasterListCommands>(graph_.rasterCommands(compiled)->draw);
        auto& draws = frame.raster_draws[dynamic.data_index];
        const auto& native = frame.record_passes[p].native;
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

    void RenderGraphInstance::linkRecordInputs(FrameResources& frame) noexcept
    {
        const auto& recording = graph_.recordingProgram();
        for (std::uint32_t i = 0; i < frame.passes.size(); ++i)
        {
            const auto& pass = frame.passes[i];
            auto& record = frame.record_passes[i];
            record.images = pass.images;
            record.buffers = pass.buffers;
            record.parameters = pass.parameters;
            auto& native = record.native;
            native.colors = pass.colors;
            const auto* raster = recording.rasterCommands(*pass.compiled_pass);
            const auto* list = raster ? std::get_if<RasterListCommands>(&raster->draw) : nullptr;
            if (list) native.draws = &frame.raster_draws[list->data_index];
        }
    }

    void RenderGraphInstance::patchRecordResource(FrameResources& frame, std::uint32_t i) noexcept
    {
        const auto& binding = frame.bindings[i];
        auto& resource = frame.record_resources[i];
        resource.buffer = binding.buffer;
        resource.buffer_offset = binding.buffer_offset;
        resource.buffer_size = binding.buffer_size;
        resource.image = binding.image;
        const auto history = graph_.resources()[i].history;
        resource.history_valid = history == kGraphInvalidIndex || frame.history_valid[history] != 0;
        resource.width = resource.height = 0;
        if (binding.image)
        {
            const auto& description = *binding.image_description;
            resource.width = description.width;
            resource.height = description.height;
        }
    }

    RenderResult<void> RenderGraphInstance::patchRecordPass(FrameResources& frame, std::uint32_t i) noexcept
    {
        auto& pass = frame.passes[i];
        auto& record = frame.record_passes[i];
        record.alias_barrier = pass.alias_barrier;
        record.private_descriptors = pass.private_descriptors;
        auto& native = record.native;
        native.depth = pass.depth;
        const auto condition = pass.compiled_pass->condition_region;
        const bool enabled = condition == kGraphInvalidIndex || frame.enabled_regions[condition];
        if (enabled)
        {
            if (const auto* compute = graph_.computeCommands(*pass.compiled_pass))
            {
                auto ready = freezeCompute(*compute, frame.record_resources, session_->description(), native);
                if (!ready) return ready;
            }
        }
        if (const auto* scope = graph_.recordingProgram().rasterScope(*pass.compiled_pass))
        {
            const auto& attachment = scope->colors.empty() ? *scope->depth : scope->colors.front();
            const auto& image = frame.record_resources[attachment.access.version.resource.index];
            const auto mip = std::get<GraphImageRange>(attachment.access.range).first_mip;
            const auto width = std::max(1u, image.width >> mip);
            const auto height = std::max(1u, image.height >> mip);
            const auto& area = scope->viewport ? *scope->viewport : frame.viewport;
            native.viewport = {
                area.x * width, area.y * height, area.width * width, area.height * height,
                area.min_depth, area.max_depth, true
            };
            native.scissor = {
                static_cast<std::int16_t>(area.x * width), static_cast<std::int16_t>(area.y * height),
                static_cast<nri::Dim_t>(area.width * width), static_cast<nri::Dim_t>(area.height * height)
            };
        }
        if (native.draws) patchDrawSection(frame, i);
        return {};
    }

    void RenderGraphInstance::freezeHistoryInitializations(FrameResources& frame) const noexcept
    {
        for (const auto h : frame.pending_history_clears.slots())
        {
            const auto& history = histories_[h];
            const auto& description = history.plan->description;
            const auto canonical_layout = description.canonical_use == EGraphUse::SAMPLED ? nri::Layout::SHADER_RESOURCE
                : description.canonical_use == EGraphUse::TRANSFER_SOURCE ? nri::Layout::COPY_SOURCE
                                                                         : nri::Layout::SHADER_RESOURCE_STORAGE;
            const auto canonical_access = description.canonical_use == EGraphUse::SAMPLED
                ? nri::AccessBits::SHADER_RESOURCE
                : description.canonical_use == EGraphUse::TRANSFER_SOURCE ? nri::AccessBits::COPY_SOURCE
                                                                         : nri::AccessBits::SHADER_RESOURCE_STORAGE;
            const auto& selections = frame.history_clears[h];
            for (std::size_t side = 0; side < selections.size(); ++side)
            {
                // History banks own disjoint resources. Each active clear is independently recordable.
                const auto key = static_cast<std::size_t>(h) * selections.size() + side;
                const auto selection = selections[side];
                if (selection.slot == kGraphInvalidIndex)
                {
                    frame.history_initializations.erase(key);
                    continue;
                }
                const auto& selected = history.slots[selection.slot];
                auto& record = frame.history_initializations.emplace(key);
                if (selected.image)
                {
                    const auto& graph_image =
                        std::get<GraphImageDesc>(graph_.resources()[description.current.index].description);
                    const auto aspects = rdesc::textureAspectMask(graph_image.format);
                    record.depth = aspects != 1;
                    record.views = selected.initial_views;
                    auto& transition = record.image_before;
                    transition.texture = selected.image;
                    if (selected.initialized)
                        transition.before = {canonical_access, canonical_layout, nri::StageBits::ALL};
                    transition.after = record.depth
                        ? nri::AccessLayoutStage{
                            nri::AccessBits::DEPTH_STENCIL_ATTACHMENT, nri::Layout::DEPTH_STENCIL_ATTACHMENT,
                            nri::StageBits::DEPTH_STENCIL_ATTACHMENT
                        }
                        : nri::AccessLayoutStage{
                            nri::AccessBits::COLOR_ATTACHMENT, nri::Layout::COLOR_ATTACHMENT,
                            nri::StageBits::COLOR_ATTACHMENT
                        };
                    transition.mipNum = selected.image_description->mipNum;
                    transition.layerNum = selected.image_description->layerNum;
                    transition.planes = record.depth ? nri::PlaneBits::DEPTH : nri::PlaneBits::COLOR;
                    if (aspects & 4) transition.planes |= nri::PlaneBits::STENCIL;
                    if (record.depth) record.clear_value.depthStencil.depth = description.initial.depth;
                    else
                    {
                        const auto& color = description.initial.color;
                        record.clear_value.color.f = {color[0], color[1], color[2], color[3]};
                    }
                    record.image_after = transition;
                    record.image_after.before = transition.after;
                    record.image_after.after = {canonical_access, canonical_layout, nri::StageBits::ALL};
                }
                else
                {
                    auto& transition = record.buffer_before;
                    transition.buffer = selected.buffer;
                    if (selected.initialized) transition.before = {canonical_access, nri::StageBits::ALL};
                    transition.after = {
                        nri::AccessBits::COPY_DESTINATION, zeroBufferStages(session_->description().graphicsAPI)
                    };
                    record.buffer_size = selected.buffer_size;
                    record.buffer_after = transition;
                    record.buffer_after.before = transition.after;
                    record.buffer_after.after = {canonical_access, nri::StageBits::ALL};
                }
            }
        }
        frame.pending_history_clears.clear();
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

    RenderResult<void> RenderGraphInstance::FrameResources::patchTransferFields(
        const TransferProgram& program, const RecordingDependencies& dependencies, CopyAlignment alignment
    ) noexcept
    {
        using Field = RecordingDependencies::ETransferField;
        const auto& fields = dependencies.transfer_fields;
        const TransferSources sources{record_resources, alignment};
        std::size_t patched = 0;
        for (const auto index : pending_transfer_fields.slots())
        {
            const auto [field, p, row] = fields[index];
            const auto& pass = passes[p];
            const auto condition = pass.compiled_pass->condition_region;
            if (condition != kGraphInvalidIndex && !enabled_regions[condition]) continue;
            ++patched;
            switch (field)
            {
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
                (source ? native.source_offset : native.destination_offset) = buffer.buffer_offset + operand.range.offset;
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
        }
        patched_transfer_fields = patched;
        pending_transfer_fields.clear();
        return {};
    }

    std::size_t RenderGraphInstance::lastTransferFieldPatches(std::uint32_t slot) const noexcept
    {
        return frames_[slot].patched_transfer_fields;
    }

    RenderResult<void> RenderGraphInstance::freezeRecordInput(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        for (const auto i : frame.pending_resources.slots()) patchRecordResource(frame, i);
        for (const auto i : frame.pending_passes.slots())
        {
            if (auto patched = patchRecordPass(frame, i); !patched) return patched;
        }
        const auto& memory = session_->description().memoryAlignment;
        auto transfers = frame.patchTransferFields(
            graph_.recordingProgram().transfers, graph_.binding_plan_->recordings,
            {memory.uploadBufferTextureRow, memory.uploadBufferTextureSlice}
        );
        if (!transfers) return transfers;
        frame.last_patches = {frame.pending_resources.slots().size(), frame.pending_passes.slots().size()};
        frame.pending_resources.clear();
        frame.pending_passes.clear();
        if (!frame.pending_history_clears.slots().empty()) freezeHistoryInitializations(frame);
        return {};
    }

    RenderGraphInstance::FrozenPatchCounts RenderGraphInstance::lastFrozenPatches(std::uint32_t slot) const noexcept
    {
        return frames_[slot].last_patches;
    }

    std::uint32_t RenderGraphInstance::recordedPassCount(std::uint32_t slot) const noexcept
    {
        return frames_[slot].enabled_pass_count;
    }
} // namespace lux::render::detail
