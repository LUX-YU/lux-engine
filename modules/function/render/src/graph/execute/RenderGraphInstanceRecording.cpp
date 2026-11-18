#include <algorithm>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/graph/execute/BindingState.hpp>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/execute/FrameExecutor.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>

namespace lux::render::detail
{
    RenderResult<std::span<nri::CommandBuffer* const>>
    RenderGraphInstance::recordBatch(std::uint32_t frame_slot, std::uint32_t batch) noexcept
    {
        if (const auto error = session_->terminalError(); error.type) return cxx::unexpected(error);
        return recordFrozenBatch(frame_slot, batch);
    }

    RenderResult<std::span<nri::CommandBuffer* const>>
    RenderGraphInstance::recordFrozenBatch(std::uint32_t frame_slot, std::uint32_t batch) noexcept
    {
        const bool is_invalid_slot = frame_slot >= frames_.size();
        const bool is_invalid_batch = batch >= plan_->submissions().size();
        const bool is_invalid_recording = is_invalid_slot || is_invalid_batch;
        if (is_invalid_recording) return cxx::unexpected(RenderError{kDeviceArgument});
        auto& frame = frames_[frame_slot];
        if (!frame.record_ready) return cxx::unexpected(RenderError{kDeviceArgument});
        auto& recording = frame.command_batches[batch];
        if (recording.recording.exchange(true, std::memory_order_acquire))
            return cxx::unexpected(RenderError{kDeviceBusy});
        cxx::scope_exit release([&]() noexcept { recording.recording.store(false, std::memory_order_release); });
        const FrameRecordInput input{
            *plan_, frame.record_resources, frame.record_passes, frame.history_initializations,
            frame.enabled_regions, frame.history_valid, frame.final_images, frame.final_buffers,
            frame.descriptor_pool, frame.shared_descriptors, frame.viewport
        };
        auto& device = *session_;
        const auto& description = plan_->submissions()[batch];
        const bool has_tail = description.output_begin != kGraphInvalidIndex;
        const std::uint32_t first_part = has_tail && description.output_begin == description.first_pass ? 1u : 0u;
        const std::uint32_t end_part = has_tail ? 2u : 1u;
        for (auto part = first_part; part < end_part; ++part)
        {
            const bool reusable = part == 0 && description.reusable_recording;
            if (reusable && recording.revision == recording.recorded_revision) continue;
            if (part == 0) recording.recorded_revision = 0;
            auto* command = recording.commands[part];
            auto result = device.resetCommandAllocator(*recording.allocators[part]);
            if (result == nri::Result::SUCCESS) result = device.beginCommandBuffer(*command, reusable);
            if (result != nri::Result::SUCCESS) return cxx::unexpected(nativeError(result));
            BindingState bindings;
            auto encoded = FrameExecutor::recordBatchPart(
                input, device.nriCore(), *command, batch, part == 1, bindings
            );
            result = device.nriCore().EndCommandBuffer(*command);
            if (!encoded) return cxx::unexpected(encoded.error());
            if (result != nri::Result::SUCCESS) return cxx::unexpected(nativeError(result));
            if (reusable) recording.recorded_revision = recording.revision;
        }
        return std::span<nri::CommandBuffer* const>{
            recording.commands.data() + first_part, end_part - first_part
        };
    }

    void RenderGraphInstance::invalidateRecording(std::uint32_t frame_slot, std::uint32_t compiled_pass) noexcept
    {
        auto& frame = frames_[frame_slot];
        frame.record_ready = false;
        if (frame.command_batches.empty()) return; // Inputs can precede initialize().
        const auto batch = plan_->passes()[compiled_pass].submission;
        if (compiled_pass < plan_->submissions()[batch].output_begin) ++frame.command_batches[batch].revision;
    }

    void RenderGraphInstance::invalidateRecordings(std::uint32_t frame_slot) noexcept
    {
        frames_[frame_slot].record_ready = false;
        for (auto& batch : frames_[frame_slot].command_batches) ++batch.revision;
    }

    void RenderGraphInstance::invalidateRasterDraws(GraphPassId pass, std::uint32_t frame_slot) noexcept
    {
        invalidateRecording(frame_slot, plan_->pass_indices_[pass.index]);
    }

    RenderResult<void> RenderGraphInstance::createRecordings(std::uint32_t frame_slot) noexcept
    {
        auto& frame = frames_[frame_slot];
        const auto batches = plan_->submissions();
        frame.command_batches.resize(batches.size());
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
                    session_->queue(description.queue).native(), frame.command_batches[batch].allocators[part]
                );
                if (result == nri::Result::SUCCESS)
                {
                    result = core.CreateCommandBuffer(
                        *frame.command_batches[batch].allocators[part], frame.command_batches[batch].commands[part]
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
    RenderGraphInstance::rasterDraws(GraphPassId pass, std::uint32_t slot, bool recording_changed) noexcept
    {
        if (recording_changed) invalidateRasterDraws(pass, slot);
        const auto& compiled = *compiledPass(pass);
        const auto& raster = compiled.work.get<RasterCommands>();
        return frames_[slot].raster_draws[std::get<RasterListCommands>(raster.draw).data_index];
    }

    void RenderGraphInstance::prepareStreamedDraws(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        for (const auto& pass : frame.passes)
        {
            const auto& compiled = *pass.compiled_pass;
            const auto* raster = compiled.work.getIf<RasterCommands>();
            const auto* scope = compiled.rasterScope();
            const auto* streamed = raster ? std::get_if<IndexedListCommands>(&raster->draw) : nullptr;
            const auto* dynamic = raster ? std::get_if<RasterListCommands>(&raster->draw) : nullptr;
            if (!streamed && !dynamic)
            {
                continue;
            }
            auto& draws =
                streamed ? frame.draw_lists[streamed->data_index].draws : frame.raster_draws[dynamic->data_index];
            if (draws.dependency_reads != draws.reads)
            {
                draws.dependency_reads = draws.reads;
                frame.invalidateImportedBindings();
            }
            if (dynamic && !draws.sections.empty())
            {
                continue; // The feature supplied explicit per-layer rendering sections for this reusable slice.
            }
            const auto& placement = plan_->descriptor_plan_->passes[compiled.source_pass];
            draws.colors.assign(pass.colors.begin(), pass.colors.end());
            if (streamed)
            {
                draws.descriptors.clear();
                if (pass.private_descriptors) draws.descriptors.push_back(pass.private_descriptors);
                draws.vertices.resize(raster->vertices.size());
            }
            for (std::size_t v = 0; streamed && v < raster->vertices.size(); ++v)
            {
                const auto& source = raster->vertices[v];
                const auto& binding = frame.bindings[source.resource.index];
                draws.vertices[v] = {
                    source.binding,
                    {binding.buffer, binding.buffer_offset + source.offset, source.stride}
                };
            }
            const auto& attachment = scope->colors.empty() ? *scope->depth : scope->colors.front();
            const auto& image =
                session_->nriCore().GetTextureDesc(*frame.bindings[attachment.access.version.resource.index].image);
            const auto mip = std::get<GraphImageRange>(attachment.access.range).first_mip;
            const auto width = std::max(1u, std::uint32_t(image.width) >> mip);
            const auto height = std::max(1u, std::uint32_t(image.height) >> mip);
            const auto& area = scope->state_input ? pass.raster_state.viewport
                               : scope->viewport  ? *scope->viewport
                                                   : frame.viewport;
            draws.sections.resize(1);
            auto& section = draws.sections.front();
            section = {};
            section.color_count = static_cast<std::uint32_t>(draws.colors.size());
            section.depth = pass.depth;
            section.viewport = {
                area.x * width,
                area.y * height,
                area.width * width,
                area.height * height,
                area.min_depth,
                area.max_depth,
                true
            };
            section.parameter_size = dynamic ? static_cast<std::uint32_t>(draws.parameters.size())
                                     : streamed->partial_parameters
                                         ? static_cast<std::uint32_t>(compiled.parameter_size)
                                         : 0;
            section.draw_count = static_cast<std::uint32_t>(draws.packets.size());
            if (scope->state_input)
            {
                section.depth_bias =
                    nri::DepthBiasDesc{pass.raster_state.depth_bias_constant, 0, pass.raster_state.depth_bias_slope};
            }
            if (dynamic)
            {
                const nri::Rect scissor{
                    static_cast<std::int16_t>(area.x * width),
                    static_cast<std::int16_t>(area.y * height),
                    static_cast<nri::Dim_t>(area.width * width),
                    static_cast<nri::Dim_t>(area.height * height)
                };
                for (auto& packet : draws.packets)
                {
                    packet.scissor = scissor;
                }
                continue;
            }
            const auto& index = frame.bindings[streamed->index_resource.resource.index];
            for (auto& packet : draws.packets)
            {
                packet.pipeline = compiled.pipeline;
                packet.bindings = placement.recordBindings();
                packet.descriptor_count = static_cast<std::uint32_t>(draws.descriptors.size());
                packet.vertex_count = static_cast<std::uint32_t>(draws.vertices.size());
                packet.index = index.buffer;
                packet.index_offset = index.buffer_offset + streamed->index_offset;
                packet.index_type =
                    streamed->type == EIndexType::UINT16 ? nri::IndexType::UINT16 : nri::IndexType::UINT32;
            }
        }
    }

    void RenderGraphInstance::freezeRecordInput(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        const auto& core = session_->nriCore();
        for (std::size_t i = 0; i < frame.bindings.size(); ++i)
        {
            const auto& binding = frame.bindings[i];
            auto& resource = frame.record_resources[i];
            resource = {binding.buffer, binding.buffer_offset, binding.image, 0, 0, binding.buffer_size};
            const auto history = plan_->resources()[i].history;
            resource.history_valid = history == kGraphInvalidIndex || frame.history_valid[history] != 0;
            if (binding.image)
            {
                const auto& description = core.GetTextureDesc(*binding.image);
                resource.width = description.width;
                resource.height = description.height;
            }
        }
        for (std::size_t i = 0; i < frame.passes.size(); ++i)
        {
            auto& pass = frame.passes[i];
            auto& record = frame.record_passes[i];
            record = {
                pass.compiled_pass, pass.images, pass.buffers, pass.alias_barrier,
                pass.private_descriptors, pass.parameters, pass.scratch
            };
            auto& native = record.native;
            native.private_views = pass.private_views;
            native.history_views = pass.history_views;
            native.clear_views = pass.clear_views;
            native.colors = pass.colors;
            native.depth = pass.depth;
            native.private_descriptors = pass.private_descriptors;
            native.copy_layouts = pass.copy_layouts;
            native.dispatch_groups = pass.dispatch_groups;
            native.shared_descriptors = frame.shared_descriptors;
            if (const auto* raster = pass.compiled_pass->work.getIf<RasterCommands>())
            {
                if (const auto* streamed = std::get_if<IndexedListCommands>(&raster->draw))
                    native.draws = &frame.draw_lists[streamed->data_index].draws;
                else if (const auto* dynamic = std::get_if<RasterListCommands>(&raster->draw))
                    native.draws = &frame.raster_draws[dynamic->data_index];
            }
            if (const auto* scope = pass.compiled_pass->rasterScope())
            {
                const auto& group = plan_->recording_.groups[pass.compiled_pass->rendering_group];
                native.begin_rendering = group.first_pass == i;
                native.end_rendering = group.first_pass + group.pass_count == i + 1;
                const auto* previous = i ? frame.passes[i - 1].compiled_pass->rasterScope() : nullptr;
                const bool has_state_input = scope->state_input.has_value();
                const bool previous_changed = previous && (previous->state_input ||
                    previous->viewport != scope->viewport || previous->exclusive);
                native.reset_viewport = native.begin_rendering || has_state_input || previous_changed;
                const auto& attachment = scope->colors.empty() ? *scope->depth : scope->colors.front();
                const auto& image = frame.record_resources[attachment.access.version.resource.index];
                const auto mip = std::get<GraphImageRange>(attachment.access.range).first_mip;
                const auto width = std::max(1u, image.width >> mip);
                const auto height = std::max(1u, image.height >> mip);
                const auto& area = has_state_input ? pass.raster_state.viewport
                    : scope->viewport ? *scope->viewport : frame.viewport;
                native.viewport = {
                    area.x * width, area.y * height, area.width * width, area.height * height,
                    area.min_depth, area.max_depth, true
                };
                native.scissor = {
                    static_cast<std::int16_t>(area.x * width), static_cast<std::int16_t>(area.y * height),
                    static_cast<nri::Dim_t>(area.width * width), static_cast<nri::Dim_t>(area.height * height)
                };
                native.dynamic_depth_bias = has_state_input;
                native.depth_bias = has_state_input
                    ? nri::DepthBiasDesc{pass.raster_state.depth_bias_constant, 0, pass.raster_state.depth_bias_slope}
                    : nri::DepthBiasDesc{scope->depth_bias[0], 0, scope->depth_bias[1]};
            }
        }

        frame.history_initializations.clear();
        for (const auto& history : histories_)
        {
            const auto& description = history.plan->description;
            const auto canonical_layout = description.canonical_use == EGraphUse::SAMPLED ? nri::Layout::SHADER_RESOURCE
                : description.canonical_use == EGraphUse::TRANSFER_SOURCE ? nri::Layout::COPY_SOURCE
                                                                         : nri::Layout::SHADER_RESOURCE_STORAGE;
            const auto canonical_access = description.canonical_use == EGraphUse::SAMPLED
                ? nri::AccessBits::SHADER_RESOURCE
                : description.canonical_use == EGraphUse::TRANSFER_SOURCE ? nri::AccessBits::COPY_SOURCE
                                                                         : nri::AccessBits::SHADER_RESOURCE_STORAGE;
            const std::array selections{
                std::pair{history.previous, history.clear_previous}, std::pair{history.current, history.clear_current}
            };
            for (const auto [index, clear] : selections)
            {
                if (!clear) continue;
                const auto& selected = history.slots[index];
                auto& record = frame.history_initializations.emplace_back();
                if (selected.image)
                {
                    const auto& image = core.GetTextureDesc(*selected.image);
                    const auto& graph_image =
                        std::get<GraphImageDesc>(plan_->resources()[description.current.index].description);
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
                    transition.mipNum = image.mipNum;
                    transition.layerNum = image.layerNum;
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
    }

    std::uint32_t RenderGraphInstance::recordedPassCount(std::uint32_t slot) const noexcept
    {
        return frames_[slot].enabled_pass_count;
    }

    RenderResult<void> RenderGraphInstance::setIndexedDrawData(
        GraphPassId id, std::uint32_t slot, const IndexedDrawData& input
    ) noexcept
    {
        const auto* pass = compiledPass(id);
        const auto* raster = pass ? pass->work.getIf<RasterCommands>() : nullptr;
        const auto* list = raster ? std::get_if<IndexedListCommands>(&raster->draw) : nullptr;
        const bool has_layout = list && raster->vertices.size() == 1;
        const bool is_matching_layout = has_layout && input.vertex_stride == raster->vertices[0].stride &&
            input.index_stride == (list->type == EIndexType::UINT16 ? 2u : 4u) &&
            input.parameters.size() <= pass->parameter_size;
        if (!is_matching_layout || slot >= frames_.size() || !resources_)
            return cxx::unexpected(RenderError{kFrameContract,
                {input.vertex_stride, has_layout ? raster->vertices[0].stride : 0, pass ? pass->parameter_size : 0}});
        auto prepared = prepareDrawList(
            id, slot, std::max((input.vertices.size() + 3) & ~std::size_t{3}, std::size_t{input.vertex_stride}),
            std::max((input.indices.size() + 3) & ~std::size_t{3}, std::size_t{4})
        );
        if (!prepared) return cxx::unexpected(prepared.error());
        auto& physical = **prepared;
        physical.vertex_data.assign(input.vertices.begin(), input.vertices.end());
        physical.index_data.assign(input.indices.begin(), input.indices.end());
        physical.vertex_data.resize((physical.vertex_data.size() + 3) & ~std::size_t{3});
        physical.index_data.resize((physical.index_data.size() + 3) & ~std::size_t{3});
        physical.upload_writes.clear();
        if (!physical.vertex_data.empty())
        {
            physical.upload_writes.push_back({physical.vertices, 0, physical.vertex_data});
        }
        if (!physical.index_data.empty()) physical.upload_writes.push_back({physical.indices, 0, physical.index_data});
        auto& draws = physical.draws;
        draws.reads.clear();
        std::fill(draws.parameters.begin(), draws.parameters.end(), std::byte{});
        std::ranges::copy(input.parameters, draws.parameters.begin());
        for (const auto& command : input.draws)
        {
            if (command.image >= input.images.size()) return cxx::unexpected(RenderError{kFrameContract});
            auto image = resources_->sampledImage(input.images[command.image], slot);
            if (!image) return cxx::unexpected(image.error());
            const RasterDrawList::Read read{image->image, GraphImageRange{1, 0, 1, 0, 1}, EGraphUse::SAMPLED, 2};
            if (std::ranges::find(draws.reads, read) == draws.reads.end()) draws.reads.push_back(read);
            const auto offset = draws.parameters.size();
            const auto bytes = list->partial_parameters ? sizeof(std::uint32_t) : pass->parameter_size;
            draws.parameters.resize(offset + bytes);
            auto* destination = draws.parameters.data() + offset;
            if (!list->partial_parameters)
            {
                std::memcpy(destination, draws.parameters.data(), pass->parameter_size);
                destination += list->selection_offset;
            }
            std::memcpy(destination, &image->descriptor, sizeof(image->descriptor));
            RasterDrawPacket packet;
            packet.parameter_offset = static_cast<std::uint32_t>(offset);
            packet.parameter_size = static_cast<std::uint32_t>(bytes);
            packet.parameter_destination = list->partial_parameters ? list->selection_offset : 0;
            packet.scissor = command.scissor;
            packet.draw = command.draw;
            draws.packets.push_back(packet);
        }
        invalidateRecordings(slot);
        return {};
    }
} // namespace lux::render::detail
