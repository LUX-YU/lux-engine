#include <algorithm>
#include <cstring>
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
            graph_, frame.record_resources, frame.record_passes, frame.history_initializations.values(),
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
        for (const auto p : dependencies.resource_passes[resource]) frame.pending_passes.mark(p);
        frame.record_ready = false;
        if (frame.command_batches.empty()) return;
        for (const auto batch : dependencies.resource_batches[resource])
            frame.command_batches[batch].invalidate();
    }

    void RenderGraphInstance::invalidateRecordings(std::uint32_t frame_slot) noexcept
    {
        frames_[frame_slot].pending_passes.markAll();
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
            auto ready = pass.freezeCommands(graph_, frame.record_resources, session_->description(), native);
            if (!ready) return ready;
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

    RenderResult<void> RenderGraphInstance::freezeRecordInput(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        for (const auto i : frame.pending_resources.slots()) patchRecordResource(frame, i);
        for (const auto i : frame.pending_passes.slots())
        {
            if (auto patched = patchRecordPass(frame, i); !patched) return patched;
        }
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
