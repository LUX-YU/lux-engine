#include <lux/engine/render/commands/Error.hpp>
#include <algorithm>
#include <cstring>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/execute/PassExtent.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>

namespace lux::render::detail
{
    RenderResult<void> RenderGraphInstance::prepareFrameBindings(std::uint32_t slot) noexcept
    {
        auto& session = *session_;
        const auto& core = session.nriCore();
        const auto& plan = graph_;
        auto& frame = frames_[slot];
        for (const auto& transition : plan.recording_.after.images)
        {
            auto barrier = transition.native;
            barrier.texture = frame.bindings[transition.resource].image;
            frame.final_images.push_back(barrier);
        }
        for (const auto& transition : plan.recording_.after.buffers)
        {
            auto barrier = transition.native;
            barrier.buffer = frame.bindings[transition.resource].buffer;
            frame.final_buffers.push_back(barrier);
        }
        auto attachmentView = [&](std::uint32_t index, GraphImageRange range, bool depth, nri::Descriptor*& result)
            -> RenderResult<void>
        {
            if (index == plan.output())
            {
                result = output_.frames[slot].view;
                return {};
            }
            if (plan.resources()[index].history != kGraphInvalidIndex)
            {
                return {}; // The selected physical History bank supplies its already-created view.
            }
            auto* image = frame.bindings[index].image;
            if (!image)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            const nri::TextureViewDesc description{
                image,
                depth ? nri::TextureView::DEPTH_STENCIL_ATTACHMENT : nri::TextureView::COLOR_ATTACHMENT,
                imageFormat(std::get<GraphImageDesc>(plan.resources()[index].description).format).native,
                static_cast<nri::Dim_t>(range.first_mip),
                1,
                static_cast<nri::Dim_t>(range.first_layer),
                static_cast<nri::Dim_t>(range.layer_count)
            };
            const auto status = core.CreateTextureView(description, result);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
            frame.native.views.push_back(result);
            return {};
        };
        auto attachment = [&](const RasterAttachment& input, bool depth, nri::AttachmentDesc& result
                          ) -> RenderResult<void>
        {
            auto view = attachmentView(
                input.access.version.resource.index, std::get<GraphImageRange>(input.access.range),
                depth, result.descriptor
            );
            if (!view)
            {
                return view;
            }
            if (input.resolve)
            {
                auto resolved = attachmentView(
                    input.resolve->version.resource.index, std::get<GraphImageRange>(input.resolve->range),
                    depth, result.resolveDst
                );
                if (!resolved)
                {
                    return resolved;
                }
            }
            result.loadOp = input.load == EAttachmentLoad::CLEAR  ? nri::LoadOp::CLEAR
                            : input.load == EAttachmentLoad::LOAD ? nri::LoadOp::LOAD
                                                                  : nri::LoadOp::CLEAR;
            result.storeOp = input.store == EAttachmentStore::STORE ? nri::StoreOp::STORE : nri::StoreOp::DISCARD;
            if (depth)
            {
                result.clearValue.depthStencil.depth = input.clear_depth;
            }
            else
            {
                result.clearValue.color.f = {input.clear[0], input.clear[1], input.clear[2], input.clear[3]};
            }
            return {};
        };
        for (const auto& compiled_pass : plan.passes())
        {
            FramePass pass{&compiled_pass};
            auto extent = validatePassExtent(
                session, plan, compiled_pass,
                [&](std::uint32_t resource) -> const FrameResourceBinding& { return frame.bindings[resource]; }
            );
            if (!extent)
            {
                return extent;
            }
            for (const auto& barrier : compiled_pass.before.images)
            {
                auto native = barrier.native;
                native.texture = frame.bindings[barrier.resource].image;
                if (!native.texture)
                {
                    return cxx::unexpected(RenderError{kDeviceArgument});
                }
                pass.images.push_back(native);
            }
            for (const auto& barrier : compiled_pass.before.buffers)
            {
                auto native = barrier.native;
                native.buffer = frame.bindings[barrier.resource].buffer;
                if (native.buffer && native.before.stages == nri::StageBits::ALL &&
                    native.before.access == nri::AccessBits::NONE)
                {
                    native.before.access = bufferAccess(core.GetBufferDesc(*native.buffer).usage);
                }
                pass.buffers.push_back(native);
            }
            if (const auto* raster = graph_.recordingProgram().rasterScope(compiled_pass))
            {
                const auto& group = plan.recording_.groups[compiled_pass.rendering_group];
                pass.colors.resize(group.colors.size());
                for (std::size_t i = 0; i < group.colors.size(); ++i)
                {
                    auto ready = attachment(group.colors[i], false, pass.colors[i]);
                    if (!ready)
                    {
                        return ready;
                    }
                }
                if (raster->depth)
                {
                    auto ready = attachment(*group.depth, true, pass.depth);
                    if (!ready)
                    {
                        return ready;
                    }
                }
            }
            if (const auto* transfer = graph_.transferCommands(compiled_pass))
            {
                pass.clear_views.resize(transfer->clear_view_count);
                const auto& transfers = graph_.recordingProgram().transfers;
                for (const auto& clear : transfers.clearCommands(*transfer))
                {
                    const auto& image = std::get<GraphImageDesc>(
                        plan.resources()[clear.destination.slot].description
                    );
                    auto ready = attachmentView(
                        clear.destination.slot, clear.destination.range,
                        rdesc::textureAspectMask(image.format) != 1,
                        pass.clear_views[clear.view]
                    );
                    if (!ready)
                    {
                        return ready;
                    }
                }
            }
            pass.parameters =
                std::span(frame.parameters).subspan(compiled_pass.parameter_offset, compiled_pass.parameter_size);
            frame.passes.push_back(std::move(pass));
        }
        // Pass arrays are fixed now. Acquiring another image only changes these borrowed view
        // fields, without allocating descriptors or searching author names during recording.
        for (auto& pass : frame.passes)
        {
            if (const auto* raster = graph_.recordingProgram().rasterScope(*pass.compiled_pass))
            {
                for (std::size_t i = 0; i < raster->colors.size(); ++i)
                {
                    const auto& attachment = raster->colors[i];
                    if (attachment.access.version.resource.index == plan.output())
                    {
                        frame.output_views.push_back(&pass.colors[i].descriptor);
                    }
                    if (attachment.resolve && attachment.resolve->version.resource.index == plan.output())
                    {
                        frame.output_views.push_back(&pass.colors[i].resolveDst);
                    }
                }
            }
            else if (const auto* transfer = graph_.transferCommands(*pass.compiled_pass))
            {
                const auto& transfers = graph_.recordingProgram().transfers;
                for (const auto& clear : transfers.clearCommands(*transfer))
                {
                    if (clear.destination.slot == plan.output())
                    {
                        frame.output_views.push_back(&pass.clear_views[clear.view]);
                    }
                }
            }
        }
        return {};
    }

    RenderResult<void> RenderGraphInstance::prepareDescriptors() noexcept
    {
        const auto& layout = *graph_.binding_plan_;
        auto pool = layout.private_pool;
        if (auto counted = session_->descriptors().addSnapshotDescriptors(pool); !counted) return counted;
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            auto& frame = frames_[slot];
            if (pool.descriptorSetMaxNum)
            {
                const auto status =
                    session_->nriCore().CreateDescriptorPool(session_->device(), pool, frame.native.descriptor_pool);
                if (status != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(status));
                }
            }
            frame.descriptor_capacity = frame.descriptor_reserved = pool;
            if (frame.native.descriptor_pool)
            {
                auto shared = session_->descriptors().createSnapshot(*frame.native.descriptor_pool);
                if (!shared) return cxx::unexpected(shared.error());
                if (session_->description().graphicsAPI == nri::GraphicsAPI::D3D12)
                    frame.shared_descriptors = *shared;
            }
            frame.native.private_views.resize(frame.passes.size());
            for (std::size_t index = 0; index < frame.passes.size(); ++index)
            {
                auto& pass = frame.passes[index];
                const auto& program = layout.pass(pass.compiled_pass->binding_slot);
                auto& owned_views = frame.native.private_views[index];
                owned_views.resize(program.uses.size());
                pass.private_views = owned_views;
                pass.history_views.resize(program.uses.size());
                if (program.has_private_set)
                {
                    const auto& native = graph_.programs()[pass.compiled_pass->binding_slot].descriptors;
                    const auto status = session_->nriCore().AllocateDescriptorSets(
                        *frame.native.descriptor_pool,
                        *native.layout,
                        native.private_set,
                        &pass.private_descriptors,
                        1,
                        0
                    );
                    if (status != nri::Result::SUCCESS)
                    {
                        return cxx::unexpected(nativeError(status));
                    }
                    for (const auto& use : program.uses)
                    {
                        if (use.sampler == kGraphInvalidIndex)
                        {
                            continue;
                        }
                        auto* sampler = graph_.programs().sampler(use.sampler);
                        const nri::UpdateDescriptorRangeDesc
                            update{pass.private_descriptors, use.binding, use.element, &sampler, 1};
                        session_->nriCore().UpdateDescriptorRanges(&update, 1);
                    }
                }
            }
            for (std::uint32_t i = 0; i < frame.bindings.size(); ++i)
            {
                auto& binding = frame.bindings[i];
                if (!binding.dirty)
                {
                    binding.dirty = true;
                    frame.changed_resources.push_back(i);
                }
            }
            if (auto rebound = rebind(slot); !rebound)
            {
                return rebound;
            }
        }
        return {};
    }

    RenderResult<void> RenderGraphInstance::rebind(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        const auto& layout = *graph_.binding_plan_;
        const auto& core = session_->nriCore();
        for (auto resource : frame.changed_resources)
        {
            auto& source = frame.bindings[resource];
            const auto& uses = graph_.resources()[resource];
            const bool image = std::holds_alternative<GraphImageDesc>(uses.description);
            if (!source.dirty)
            {
                continue;
            }
            if (!image && !source.buffer)
            {
                source.dirty = false;
                continue;
            }
            for (const auto [p, index] : uses.barrier_uses)
            {
                if (image)
                {
                    frame.passes[p].images[index].texture = source.image;
                }
                else
                {
                    auto& barrier = frame.passes[p].buffers[index];
                    barrier.buffer = source.buffer;
                    const auto& pass = *frame.passes[p].compiled_pass;
                    if (pass.before.buffers[index].bind_initial_access)
                    {
                        barrier.before.access = bufferAccess(source.buffer_description->usage);
                    }
                }
            }
            for (auto index : uses.final_barrier_uses)
            {
                if (image)
                {
                    frame.final_images[index].texture = source.image;
                }
                else
                {
                    frame.final_buffers[index].buffer = source.buffer;
                }
            }
            for (const auto [p, index] : uses.descriptor_uses)
            {
                if (uses.history != kGraphInvalidIndex)
                {
                    continue;
                }
                auto& pass = frame.passes[p];
                const auto& program = layout.pass(pass.compiled_pass->binding_slot);
                const auto& use = program.uses[index];
                nri::Descriptor* candidate = nullptr;
                auto status = nri::Result::SUCCESS;
                if (image)
                {
                    if (!source.image)
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                    const auto& range = std::get<GraphImageRange>(use.range);
                    nri::TextureViewDesc description{
                        source.image,
                        use.kind == EDescriptorKind::STORAGE_IMAGE
                            ? (program.bindings[use.field].image_arrayed ? nri::TextureView::STORAGE_TEXTURE_ARRAY
                                                                         : nri::TextureView::STORAGE_TEXTURE)
                            : (program.bindings[use.field].image_arrayed ? nri::TextureView::TEXTURE_ARRAY
                                                                         : nri::TextureView::TEXTURE),
                        imageFormat(std::get<GraphImageDesc>(uses.description).format).native,
                        static_cast<nri::Dim_t>(range.first_mip),
                        static_cast<nri::Dim_t>(range.mip_count),
                        static_cast<nri::Dim_t>(range.first_layer),
                        static_cast<nri::Dim_t>(range.layer_count)
                    };
                    description.planes = range.aspects == 1   ? nri::PlaneBits::COLOR
                                         : range.aspects == 2 ? nri::PlaneBits::DEPTH
                                                              : nri::PlaneBits::STENCIL;
                    status = core.CreateTextureView(description, candidate);
                }
                else
                {
                    const auto& range = std::get<GraphBufferRange>(use.range);
                    // Imported variable-size input buffers expose their current physical range.
                    const auto size =
                        use.whole_resource && use.fallback == kGraphInvalidIndex ? source.buffer_size : range.size;
                    const bool invalid_range = range.offset > source.buffer_size ||
                        size > source.buffer_size - range.offset || range.offset > UINT64_MAX - source.buffer_offset;
                    if (invalid_range)
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                    const auto description = program.bindings[use.field].bufferView(
                        source.buffer, source.buffer_offset + range.offset, size
                    );
                    status = createGraphBufferView(*session_, description, candidate);
                }
                if (status != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(status));
                }
                core.DestroyDescriptor(std::exchange(pass.private_views[index], candidate));
                candidate = pass.private_views[index];
                const nri::UpdateDescriptorRangeDesc
                    update{pass.private_descriptors, use.binding, use.element, &candidate, 1};
                core.UpdateDescriptorRanges(&update, 1);
                pass.history_views[index] = nullptr;
                const auto history = graph_.resources()[use.resource].history;
                if (history != kGraphInvalidIndex) frame.pending_history_views.mark(history);
                frames_[slot].invalidatePass(graph_, p);
            }
            source.dirty = false;
        }
        frame.changed_resources.clear();
        return {};
    }

    RenderResult<void> RenderGraphInstance::writeInlineConstant(
        std::uint32_t slot, std::uint32_t pass, std::span<const std::byte> bytes, std::uint64_t revision
    ) noexcept
    {
        if (slot >= frames_.size()) return cxx::unexpected(RenderError{kDeviceArgument});
        return frames_[slot].writeInlineConstant(graph_, pass, bytes, revision);
    }
} // namespace lux::render::detail
