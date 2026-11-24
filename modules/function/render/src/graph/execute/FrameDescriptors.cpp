#include <lux/engine/render/runtime/commands/Error.hpp>
#include <algorithm>
#include <cstring>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/device/DescriptorTable.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/execute/PassExtent.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>

namespace lux::render::detail
{
    RenderResult<void> FrameResources::prepareBindings(
        const compiled::RenderGraph& plan, DeviceSession& session, nri::Descriptor* output_view
    ) noexcept
    {
        const auto& core = session.nriCore();
        const auto& entries = plan.recordingProgram().before;
        entry_images.reserve(entries.images.size());
        entry_buffers.reserve(entries.buffers.size());
        for (const auto& barrier : entries.images)
        {
            auto native = barrier.native;
            native.texture = bindings[barrier.resource].image;
            if (!native.texture)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            entry_images.push_back(native);
        }
        for (const auto& barrier : entries.buffers)
        {
            auto native = barrier.native;
            native.buffer = bindings[barrier.resource].buffer;
            if (native.buffer && barrier.bind_initial_access)
            {
                const auto usage = core.GetBufferDesc(*native.buffer).usage;
                native.before.access = bufferBoundaryAccess(session.description(), usage);
            }
            entry_buffers.push_back(native);
        }
        const auto bind_exit = [&](const ImageTransition& transition) noexcept
        {
            auto barrier = transition.native;
            barrier.texture = bindings[transition.resource].image;
            exit_images.push_back(barrier);
        };
        for (const auto& transition : plan.recordingProgram().after.images) bind_exit(transition);
        for (const auto& transition : plan.recordingProgram().transient_exits) bind_exit(transition);
        const auto bind_buffer_exit = [&](const BufferTransition& transition)
        {
            auto barrier = transition.native;
            barrier.buffer = bindings[transition.resource].buffer;
            exit_buffers.push_back(barrier);
        };
        for (const auto& transition : plan.recordingProgram().after.buffers) bind_buffer_exit(transition);
        for (const auto& transition : plan.recordingProgram().buffer_read_releases) bind_buffer_exit(transition);
        auto attachmentView = [&](std::uint32_t index, GraphImageRange range, bool depth, nri::Descriptor*& result)
            -> RenderResult<void>
        {
            if (index == plan.output())
            {
                result = output_view;
                return {};
            }
            if (plan.resources()[index].history != kGraphInvalidIndex)
            {
                return {}; // The selected physical History bank supplies its already-created view.
            }
            auto* image = bindings[index].image;
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
            this->native.views.push_back(result);
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
                [&](std::uint32_t resource) -> const FrameResourceBinding& { return bindings[resource]; }
            );
            if (!extent)
            {
                return extent;
            }
            if (const auto* raster = plan.recordingProgram().rasterScope(compiled_pass))
            {
                const auto& group = plan.recordingProgram().groups[compiled_pass.rendering_group];
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
            if (const auto* transfer = plan.transferCommands(compiled_pass))
            {
                pass.clear_views.resize(transfer->clear_view_count);
                const auto& transfers = plan.recordingProgram().transfers;
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
                std::span(this->parameters).subspan(compiled_pass.parameter_offset, compiled_pass.parameter_size);
            passes.push_back(std::move(pass));
        }
        // Pass arrays are fixed now. Acquiring another image only changes these borrowed view
        // fields, without allocating descriptors or searching author names during recording.
        for (auto& pass : passes)
        {
            if (const auto* raster = plan.recordingProgram().rasterScope(*pass.compiled_pass))
            {
                for (std::size_t i = 0; i < raster->colors.size(); ++i)
                {
                    const auto& attachment = raster->colors[i];
                    if (attachment.access.version.resource.index == plan.output())
                    {
                        output_views.push_back(&pass.colors[i].descriptor);
                    }
                    if (attachment.resolve && attachment.resolve->version.resource.index == plan.output())
                    {
                        output_views.push_back(&pass.colors[i].resolveDst);
                    }
                }
            }
            else if (const auto* transfer = plan.transferCommands(*pass.compiled_pass))
            {
                const auto& transfers = plan.recordingProgram().transfers;
                for (const auto& clear : transfers.clearCommands(*transfer))
                {
                    if (clear.destination.slot == plan.output())
                    {
                        output_views.push_back(&pass.clear_views[clear.view]);
                    }
                }
            }
        }
        return {};
    }

    RenderResult<void> FrameResources::createDescriptors(
        const compiled::RenderGraph& graph, DeviceSession& session, const nri::DescriptorPoolDesc& pool
    ) noexcept
    {
        const auto& layout = graph.bindingPlan();
        if (pool.descriptorSetMaxNum)
        {
            const auto status =
                session.nriCore().CreateDescriptorPool(session.device(), pool, this->native.descriptor_pool);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
        }
        descriptor_capacity = descriptor_reserved = pool;
        if (this->native.descriptor_pool)
        {
            auto shared = session.descriptors().createSnapshot(*this->native.descriptor_pool);
            if (!shared) return cxx::unexpected(shared.error());
            if (session.description().graphicsAPI == nri::GraphicsAPI::D3D12)
                shared_descriptors = *shared;
        }
        this->native.private_views.resize(passes.size());
        for (std::size_t index = 0; index < passes.size(); ++index)
        {
            auto& pass = passes[index];
            const auto& program = layout.pass(pass.compiled_pass->binding_slot);
            auto& owned_views = this->native.private_views[index];
            owned_views.resize(program.uses.size());
            pass.private_views = owned_views;
            pass.history_views.resize(program.uses.size());
            if (program.has_private_set)
            {
                const auto& native = graph.programs()[pass.compiled_pass->binding_slot].descriptors;
                const auto status = session.nriCore().AllocateDescriptorSets(
                    *this->native.descriptor_pool,
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
                    auto* sampler = graph.programs().sampler(use.sampler);
                    const nri::UpdateDescriptorRangeDesc
                        update{pass.private_descriptors, use.binding, use.element, &sampler, 1};
                    session.nriCore().UpdateDescriptorRanges(&update, 1);
                }
            }
        }
        for (std::uint32_t i = 0; i < bindings.size(); ++i)
        {
            auto& binding = bindings[i];
            if (!binding.dirty)
            {
                binding.dirty = true;
                changed_resources.push_back(i);
            }
        }
        return rebind(graph, session);
    }

    RenderResult<void> FrameResources::rebind(const compiled::RenderGraph& graph, DeviceSession& session) noexcept
    {
        const auto& layout = graph.bindingPlan();
        const auto& core = session.nriCore();
        for (auto resource : changed_resources)
        {
            auto& source = bindings[resource];
            const auto& uses = graph.resources()[resource];
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
            for (const auto index : uses.barrier_uses)
            {
                if (image)
                {
                    entry_images[index].texture = source.image;
                }
                else
                {
                    auto& barrier = entry_buffers[index];
                    barrier.buffer = source.buffer;
                    if (graph.recordingProgram().before.buffers[index].bind_initial_access)
                    {
                        const auto usage = source.buffer_description->usage;
                        barrier.before.access = bufferBoundaryAccess(session.description(), usage);
                    }
                }
            }
            for (auto index : uses.exit_barrier_uses)
            {
                if (image)
                {
                    exit_images[index].texture = source.image;
                }
                else
                {
                    exit_buffers[index].buffer = source.buffer;
                }
            }
            for (const auto [p, index] : uses.descriptor_uses)
            {
                if (uses.history != kGraphInvalidIndex)
                {
                    continue;
                }
                auto& pass = passes[p];
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
                    const auto description = graphImageView(source.image, program.bindings[use.field].image_type,
                        imageFormat(std::get<GraphImageDesc>(uses.description).format).native, range);
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
                    status = createGraphBufferView(session, description, candidate);
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
                const auto history = graph.resources()[use.resource].history;
                if (history != kGraphInvalidIndex) pending_history_views.mark(history);
                invalidatePass(graph, p);
            }
            source.dirty = false;
        }
        changed_resources.clear();
        return {};
    }

} // namespace lux::render::detail
