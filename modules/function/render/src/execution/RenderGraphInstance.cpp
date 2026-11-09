#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/commands/ImageCommands.hpp>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/execution/FrameResources.hpp>
#include <lux/engine/render/execution/PassExtent.hpp>
#include <lux/engine/render/execution/RenderGraphInstance.hpp>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/execution/FrameExecution.hpp>
#include <lux/engine/render/graph/PhysicalResourcePlan.hpp>
#include <lux/engine/render/graph/ShaderBindingPlan.hpp>
#include <numeric>

namespace lux::render::detail
{
    ResourceInput RenderGraphInstance::resourceInput(GraphResourceId resource, std::uint32_t slot) const noexcept
    {
        const auto& input = frames_[slot].bindings[resource.index];
        return {
            input.buffer,
            input.buffer_offset,
            input.buffer_size,
            input.parameters,
            input.parameter_layout,
            input.parameter_revision,
            input.image,
            input.addressed_buffers,
            input.addressed_revision
        };
    }

    RenderGraphInstance::RenderGraphInstance(
        DeviceSession& session,
        std::uint32_t frame_count,
        const RenderPlan& plan,
        TargetOutput output
    ) noexcept
        : session_(&session), plan_(&plan), output_(output)
    {
        instance_values_.resize(plan.descriptor_plan_->instance_parameter_bytes);
        condition_values_.resize(plan.condition_count_);
        parameter_copies_.resize(plan.passes().size());
        const auto parameter_bytes = plan.passes().empty() ? 0 :
            plan.passes().back().parameter_offset + plan.passes().back().parameter_size;
        for (const auto& program : plan.descriptor_plan_->passes)
        {
            for (const auto& use : program.uses)
            {
                if (use.sampler != kGraphInvalidIndex && use.sampler >= samplers_.size())
                {
                    samplers_.resize(std::size_t(use.sampler) + 1);
                }
                if (use.sampler != kGraphInvalidIndex)
                {
                    samplers_[use.sampler].comparison = use.comparison_sampler;
                    samplers_[use.sampler].used = true;
                }
            }
        }
        frames_.resize(frame_count);
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            auto& frame = frames_[slot];
            frame.invalidateImportedBindings();
            frame.parameters.resize(parameter_bytes);
            for (const auto& pass : plan.passes())
            {
                const auto& initial = plan.descriptor_plan_->passes[pass.source_pass].initial_parameters;
                std::ranges::copy(initial, frame.parameters.begin() + pass.parameter_offset);
            }
            frame.parameter_dirty.resize(plan.passes().size());
            frame.parameter_updates.reserve(plan.passes().size());
            frame.bindings.resize(plan.resources().size());
            frame.enabled_regions.resize(plan.recording_.conditions.size(), 1);
            frame.history_valid.resize(plan.histories_.size(), 0);
            frame.changed_resources.reserve(plan.resources().size());
            frame.unavailable_buffers = std::ranges::count_if(
                plan.resources(),
                [](const auto& resource)
                {
                    return resource.memory_used && std::holds_alternative<GraphBufferDesc>(resource.description) &&
                           resource.origin == EGraphOrigin::IMPORTED;
                }
            );
            frame.images.reserve(std::ranges::count_if(
                plan.resources(),
                [](const auto& resource)
                {
                    return resource.memory_used && std::holds_alternative<GraphImageDesc>(resource.description) &&
                           resource.origin != EGraphOrigin::IMPORTED;
                }
            ));
        }
    }

    RenderGraphInstance::~RenderGraphInstance() noexcept
    {
        // Physical cleanup is owned by the Session; it never calls back into this instance.
        session_->deferRelease(
            [frames = std::move(frames_), histories = std::move(histories_), nri_core = &session_->nriCore()](
            ) mutable noexcept
            {
                for (auto& frame : frames)
                {
                    frame.release(*nri_core);
                }
                for (auto& history : histories)
                {
                    history.release(*nri_core);
                }
            }
        );
    }

    void RenderGraphInstance::bindImage(GraphResourceId resource, std::uint32_t slot, nri::Texture& image) noexcept
    {
        auto& binding = frames_[slot].bindings[resource.index];
        if (binding.image != &image)
        {
            frames_[slot].invalidateImportedBindings();
        }
        binding.image = &image;
    }

    RenderResult<void>
    RenderGraphInstance::bindSampler(std::uint32_t input, nri::Descriptor& sampler, bool comparison) noexcept
    {
        if (input >= samplers_.size() || samplers_[input].comparison != comparison)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        samplers_[input].descriptor = &sampler;
        return {};
    }

    RenderResult<void> RenderGraphInstance::initialize() noexcept
    {
        auto& session = *session_;
        const auto& plan = *plan_;
        const bool wrong_device = plan.device() != &session.device();
        const bool wrong_target =
            !output_.frames.empty() &&
            (!plan.compatible(output_.description) || output_.frames.size() != frames_.size());
        const bool needs_target = plan.output() != kGraphInvalidIndex ||
            std::ranges::any_of(plan.resources(), [](const auto& resource) noexcept { return resource.target_sized; });
        const bool missing_target = needs_target && output_.frames.empty();
        const bool missing_programs = !plan.program_cache_ || std::ranges::any_of(plan.passes(), [&](const auto& pass)
        {
            const auto& bindings = plan.descriptor_plan_->passes[pass.source_pass];
            if (!bindings.private_bindings.empty() && !bindings.layout) return true;
            if (const auto* raster = std::get_if<RasterCommands>(&pass.work))
                return !std::holds_alternative<RasterListCommands>(raster->draw) && !raster->pipeline;
            const auto* compute = std::get_if<ComputeCommands>(&pass.work);
            return compute && !compute->pipeline;
        });
        if (wrong_device || wrong_target || missing_target || missing_programs)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        if (auto histories = createHistories(); !histories)
        {
            return histories;
        }
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            if (auto lists = createDrawListResources(slot); !lists)
            {
                return lists;
            }
            if (auto resources = createFrameResources(slot); !resources)
            {
                return resources;
            }
            if (auto passes = prepareFrameBindings(slot); !passes)
            {
                return passes;
            }
            prepareAliasBarriers(frames_[slot]);
            if (auto recordings = createRecordings(slot); !recordings)
            {
                return recordings;
            }
        }
        if (auto descriptors = prepareDescriptors(); !descriptors)
        {
            return descriptors;
        }
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            bindHistory(slot);
            updateHistoryViews(slot);
        }
        return {};
    }

    RenderResult<void> RenderGraphInstance::createDrawListResources(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        const auto& core = session_->nriCore();
        for (std::size_t p = 0; p < plan_->passes().size(); ++p)
        {
            const auto& pass = plan_->passes()[p];
            const auto* raster = std::get_if<RasterCommands>(&pass.work);
            if (raster && std::holds_alternative<RasterListCommands>(raster->draw))
            {
                frame.raster_draws.emplace_back();
                continue;
            }
            const auto* list = raster ? std::get_if<IndexedListCommands>(&raster->draw) : nullptr;
            if (!list)
            {
                continue;
            }
            auto& physical = frame.draw_lists.emplace_back();
            const auto vertex = raster->vertices.front();
            physical.vertex_capacity = std::max<std::uint64_t>(vertex.stride, 4);
            physical.index_capacity = 4;
            auto status = core.CreatePlacedBuffer(
                session_->device(),
                NriDeviceHeap,
                {physical.vertex_capacity, 0, nri::BufferUsageBits::VERTEX_BUFFER},
                physical.vertices
            );
            if (status == nri::Result::SUCCESS)
            {
                status = core.CreatePlacedBuffer(
                    session_->device(),
                    NriDeviceHeap,
                    {physical.index_capacity, 0, nri::BufferUsageBits::INDEX_BUFFER},
                    physical.indices
                );
            }
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
            physical.draws.parameters.resize(pass.parameter_size);
            setResource(vertex.resource.index, slot, {physical.vertices, 0, physical.vertex_capacity});
            setResource(list->index_resource.resource.index, slot, {physical.indices, 0, physical.index_capacity});
        }
        return {};
    }

    const CompiledPass* RenderGraphInstance::compiledPass(GraphPassId id) const noexcept
    {
        const auto index = id.index < plan_->pass_indices_.size() ? plan_->pass_indices_[id.index] : kGraphInvalidIndex;
        return index == kGraphInvalidIndex ? nullptr : &plan_->passes()[index];
    }

    RenderResult<DrawListResources*> RenderGraphInstance::prepareDrawList(
        GraphPassId id,
        std::uint32_t slot,
        std::size_t vertex_bytes,
        std::size_t index_bytes
    ) noexcept
    {
        const auto* pass = compiledPass(id);
        const auto* raster = pass ? std::get_if<RasterCommands>(&pass->work) : nullptr;
        const auto* list = raster ? std::get_if<IndexedListCommands>(&raster->draw) : nullptr;
        if (!list)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const auto vertex = raster->vertices.front();
        auto& physical = frames_[slot].draw_lists[list->data_index];
        const auto& core = session_->nriCore();
        const auto grow =
            [&](nri::Buffer*& buffer, std::uint64_t& capacity, std::size_t bytes, nri::BufferUsageBits usage
            ) -> RenderResult<void>
        {
            if (bytes <= capacity)
            {
                return {};
            }
            if (bytes > (std::uint64_t{1} << 63))
            {
                return cxx::unexpected(RenderError{kDeviceCapacity});
            }
            const auto size = std::bit_ceil(std::max(std::uint64_t(bytes), std::uint64_t{4}));
            nri::Buffer* candidate = nullptr;
            const auto status = core.CreatePlacedBuffer(session_->device(), NriDeviceHeap, {size, 0, usage}, candidate);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
            auto* previous = std::exchange(buffer, candidate);
            capacity = size;
            session_->deferRelease([native = &core, previous]() noexcept { native->DestroyBuffer(previous); });
            return {};
        };
        physical.draws.packets.clear();
        physical.draws.parameters.resize(pass->parameter_size);
        auto vertices =
            grow(physical.vertices, physical.vertex_capacity, vertex_bytes, nri::BufferUsageBits::VERTEX_BUFFER);
        if (!vertices)
        {
            return cxx::unexpected(vertices.error());
        }
        setResource(vertex.resource.index, slot, {physical.vertices, 0, physical.vertex_capacity});
        auto indices = grow(physical.indices, physical.index_capacity, index_bytes, nri::BufferUsageBits::INDEX_BUFFER);
        if (!indices)
        {
            return cxx::unexpected(indices.error());
        }
        setResource(list->index_resource.resource.index, slot, {physical.indices, 0, physical.index_capacity});
        return &physical;
    }

    RenderResult<bool>
    RenderGraphInstance::prepareDrawListUploads(std::uint32_t slot, ResourceUploader& uploads) noexcept
    {
        for (auto& list : frames_[slot].draw_lists)
        {
            while (list.staged_write < list.upload_writes.size())
            {
                const auto& write = list.upload_writes[list.staged_write];
                auto staged = uploads.stageBuffer(
                    {write.target, write.offset + list.staged_bytes, write.source.subspan(list.staged_bytes)}
                );
                if (!staged)
                {
                    return cxx::unexpected(staged.error());
                }
                list.staged_bytes += *staged;
                if (list.staged_bytes != write.source.size())
                {
                    return false;
                }
                ++list.staged_write;
                list.staged_bytes = 0;
            }
        }
        return true;
    }

    void RenderGraphInstance::finishDrawListUploads(std::uint32_t slot, RenderResult<void> accepted) noexcept
    {
        for (auto& list : frames_[slot].draw_lists)
        {
            if (accepted && list.staged_write != list.upload_writes.size())
            {
                continue;
            }
            list.upload_writes.clear();
            list.upload_tails.clear();
            list.staged_write = list.staged_bytes = 0;
            if (!accepted)
            {
                list.draws.packets.clear();
                list.draws.reads.clear();
            }
        }
    }

    RasterDrawList&
    RenderGraphInstance::rasterDraws(GraphPassId pass, std::uint32_t slot, bool recording_changed) noexcept
    {
        if (recording_changed) invalidateRasterDraws(pass, slot);
        const auto& compiled = *compiledPass(pass);
        const auto& raster = std::get<RasterCommands>(compiled.work);
        return frames_[slot].raster_draws[std::get<RasterListCommands>(raster.draw).data_index];
    }

    RenderResult<std::span<nri::Descriptor* const>>
    RenderGraphInstance::prepareImageLayers(GraphResourceId resource, std::uint32_t slot, std::uint32_t layers) noexcept
    {
        auto& frame = frames_[slot];
        const auto selected =
            std::ranges::find(frame.layered_images, resource, &FrameResources::LayeredImage::resource);
        const bool invalid_layers = layers == 0 || layers > session_->description().dimensions.textureLayerMaxNum;
        if (selected == frame.layered_images.end() || invalid_layers)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        if (selected->views.size() == layers)
        {
            return std::span<nri::Descriptor* const>{selected->views};
        }
        invalidateRecordings(slot);
        const auto& core = session_->nriCore();
        auto& binding = frame.bindings[resource.index];
        const auto& compiled = plan_->resources()[resource.index];
        auto description = core.GetTextureDesc(*binding.image);
        const bool replace_image = description.layerNum != layers;
        description.layerNum = static_cast<nri::Dim_t>(layers);
        nri::Texture* image = binding.image;
        if (replace_image)
        {
            const auto created = core.CreatePlacedTexture(session_->device(), NriDeviceHeap, description, image);
            if (created != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(created));
            }
        }

        struct View final
        {
            ImageViewUse use;
            nri::Descriptor* descriptor;
        };

        std::vector<View> replacements;
        std::vector<nri::Descriptor*> layer_views;
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                for (const auto& view : replacements)
                {
                    core.DestroyDescriptor(view.descriptor);
                }
                for (auto* view : layer_views)
                {
                    core.DestroyDescriptor(view);
                }
                if (replace_image)
                {
                    core.DestroyTexture(image);
                }
            }
        );
        const auto add_view = [&](ImageViewUse use, nri::TextureView kind, std::uint32_t count) -> RenderResult<void>
        {
            nri::TextureViewDesc view{
                image,
                kind,
                description.format,
                static_cast<nri::Dim_t>(use.range.first_mip),
                static_cast<nri::Dim_t>(use.range.mip_count),
                static_cast<nri::Dim_t>(use.range.first_layer),
                static_cast<nri::Dim_t>(count)
            };
            view.planes = use.range.aspects == 1 ? nri::PlaneBits::COLOR : nri::PlaneBits::DEPTH;
            nri::Descriptor* descriptor = nullptr;
            const auto created = core.CreateTextureView(view, descriptor);
            if (created != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(created));
            }
            replacements.push_back({use, descriptor});
            return {};
        };
        if (replace_image)
        {
            for (const auto& use : compiled.image_views)
            {
                const auto kind = use.range.aspects == 1 ? nri::TextureView::COLOR_ATTACHMENT
                                                         : nri::TextureView::DEPTH_STENCIL_ATTACHMENT;
                if (auto created = add_view(use, kind, 1); !created)
                {
                    return cxx::unexpected(created.error());
                }
            }
            for (const auto [pass, index] : compiled.descriptor_uses)
            {
                const auto& program = plan_->descriptor_plan_->passes[plan_->passes()[pass].source_pass];
                const auto& use = program.uses[index];
                const auto kind = use.kind == EDescriptorKind::STORAGE_IMAGE ? nri::TextureView::STORAGE_TEXTURE_ARRAY
                                                                             : nri::TextureView::TEXTURE_ARRAY;
                if (auto created = add_view(
                        {pass, index, EImageViewTarget::DESCRIPTOR, std::get<GraphImageRange>(use.range)},
                        kind,
                        layers
                    );
                    !created)
                {
                    return cxx::unexpected(created.error());
                }
            }
        }
        const auto aspects = rdesc::textureAspectMask(std::get<GraphImageDesc>(compiled.description).format);
        for (std::uint32_t layer = 0; layer < layers; ++layer)
        {
            const nri::TextureViewDesc view{
                image,
                aspects == 1 ? nri::TextureView::COLOR_ATTACHMENT : nri::TextureView::DEPTH_STENCIL_ATTACHMENT,
                description.format,
                0,
                1,
                static_cast<nri::Dim_t>(layer),
                1
            };
            nri::Descriptor* descriptor = nullptr;
            const auto created = core.CreateTextureView(view, descriptor);
            if (created != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(created));
            }
            layer_views.push_back(descriptor);
        }
        // All objects exist. The acquired FIF slot has no GPU reader; replace views before
        // releasing the old image. Normal frame destruction retains the same view-before-image order.
        for (auto& replacement : replacements)
        {
            auto& pass = frame.passes[replacement.use.pass];
            nri::Descriptor** destination = nullptr;
            switch (replacement.use.target)
            {
            case EImageViewTarget::DESCRIPTOR:
                destination = &pass.private_views[replacement.use.index];
                break;
            case EImageViewTarget::COLOR:
                destination = &pass.colors[replacement.use.index].descriptor;
                break;
            case EImageViewTarget::DEPTH:
                destination = &pass.depth.descriptor;
                break;
            case EImageViewTarget::COLOR_RESOLVE:
                destination = &pass.colors[replacement.use.index].resolveDst;
                break;
            case EImageViewTarget::DEPTH_RESOLVE:
                destination = &pass.depth.resolveDst;
                break;
            case EImageViewTarget::CLEAR:
                destination = &pass.clear_views[replacement.use.index];
                break;
            }
            auto* previous = std::exchange(*destination, replacement.descriptor);
            if (replacement.use.target == EImageViewTarget::DESCRIPTOR)
            {
                const auto& use =
                    plan_->descriptor_plan_->passes[pass.compiled_pass->source_pass].uses[replacement.use.index];
                const nri::UpdateDescriptorRangeDesc
                    update{pass.private_descriptors, use.binding, use.element, destination, 1};
                core.UpdateDescriptorRanges(&update, 1);
            }
            else
            {
                *std::ranges::find(frame.views, previous) = replacement.descriptor;
            }
            core.DestroyDescriptor(previous);
        }
        for (auto* previous : selected->views)
        {
            std::erase(frame.views, previous);
            core.DestroyDescriptor(previous);
        }
        selected->views = std::move(layer_views);
        frame.views.insert(frame.views.end(), selected->views.begin(), selected->views.end());
        if (replace_image)
        {
            *std::ranges::find(frame.images, binding.image) = image;
            core.DestroyTexture(std::exchange(binding.image, image));
            for (const auto [pass, index] : compiled.barrier_uses)
            {
                auto& barrier = frame.passes[pass].images[index];
                barrier.texture = binding.image;
                barrier.layerNum = static_cast<nri::Dim_t>(layers);
            }
            for (const auto index : compiled.final_barrier_uses)
            {
                frame.final_images[index].texture = binding.image;
                frame.final_images[index].layerNum = static_cast<nri::Dim_t>(layers);
            }
            frame.invalidateImportedBindings();
        }
        rollback.release();
        return std::span<nri::Descriptor* const>{selected->views};
    }

    void RenderGraphInstance::prepareStreamedDraws(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        for (const auto& pass : frame.passes)
        {
            const auto& compiled = *pass.compiled_pass;
            const auto* raster = std::get_if<RasterCommands>(&compiled.work);
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
            const auto& attachment = raster->colors.empty() ? *raster->depth : raster->colors.front();
            const auto& image =
                session_->nriCore().GetTextureDesc(*frame.bindings[attachment.access.version.resource.index].image);
            const auto mip = std::get<GraphImageRange>(attachment.access.range).first_mip;
            const auto width = std::max(1u, std::uint32_t(image.width) >> mip);
            const auto height = std::max(1u, std::uint32_t(image.height) >> mip);
            const auto& area = raster->state_input ? pass.raster_state.viewport
                               : raster->viewport  ? *raster->viewport
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
            if (raster->state_input)
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
                packet.pipeline = raster->pipeline;
                packet.bindings = &placement;
                packet.descriptor_count = static_cast<std::uint32_t>(draws.descriptors.size());
                packet.vertex_count = static_cast<std::uint32_t>(draws.vertices.size());
                packet.index = index.buffer;
                packet.index_offset = index.buffer_offset + streamed->index_offset;
                packet.index_type =
                    streamed->type == EIndexType::UINT16 ? nri::IndexType::UINT16 : nri::IndexType::UINT32;
            }
        }
    }

    RenderResult<void> RenderGraphInstance::createFrameResources(std::uint32_t slot) noexcept
    {
        auto& session = *session_;
        const auto& core = session.nriCore();
        const auto& plan = *plan_;
        auto& frame = frames_[slot];
        if (auto created = allocatePhysicalResources(frame); !created)
        {
            return created;
        }
        for (std::uint32_t i = 0; i < frame.bindings.size(); ++i)
        {
            auto& binding = frame.bindings[i];
            const auto& resource = plan.resources()[i];
            if (!resource.memory_used || resource.shared_table)
            {
                continue;
            }
            if (resource.history != kGraphInvalidIndex)
            {
                const auto& history = histories_[resource.history];
                const auto selected =
                    resource.origin == EGraphOrigin::HISTORY_PREVIOUS ? history.previous : history.current;
                const auto& physical = history.slots[selected];
                binding.image = physical.image;
                binding.buffer = physical.buffer;
                binding.buffer_size = physical.buffer_size;
                continue;
            }
            if (resource.origin == EGraphOrigin::IMPORTED)
            {
                if (const auto* image = std::get_if<GraphImageDesc>(&resource.description))
                {
                    if (!binding.image)
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                    const auto& actual = core.GetTextureDesc(*binding.image);
                    auto width = image->width, height = image->height;
                    if (resource.target_sized)
                    {
                        auto resolved = textureDescription(resource);
                        if (!resolved) return cxx::unexpected(resolved.error());
                        width = resolved->width;
                        height = resolved->height;
                    }
                    const bool extent_mismatch = image->extent.source != EImageExtentSource::IMPORTED &&
                                                 (actual.width != width || actual.height != height);
                    const bool mismatch = extent_mismatch || actual.mipNum != image->mips ||
                                          actual.layerNum != image->layers || actual.sampleNum != image->samples ||
                                          actual.format != imageFormat(image->format).native;
                    if (mismatch)
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                }
                continue;
            }
            if (i == plan.output())
            {
                binding.image = output_.frames[slot].image;
                continue;
            }
            if (const auto* image = std::get_if<GraphImageDesc>(&resource.description); image && image->runtime_layers)
            {
                if (auto created = createResource(i, binding); !created)
                {
                    return created;
                }
                frame.images.push_back(binding.image);
                frame.layered_images.push_back({GraphResourceId{i}});
                continue;
            }
            const auto physical = plan.physical_resources_.resource_slots[i];
            binding = frame.bindings[plan.physical_resources_.slots[physical].resource.index];
        }
        return {};
    }

    RenderResult<void> RenderGraphInstance::allocatePhysicalResources(FrameResources& frame) noexcept
    {
        auto& session = *session_;
        const auto& core = session.nriCore();
        const auto& physical = plan_->physical_resources_;
        const bool alias_memory = physical.allocation == ETransientAllocation::ALIAS_MEMORY;
        std::vector<nri::MemoryDesc> requirements(alias_memory ? physical.slots.size() : 0);
        for (std::uint32_t s = 0; s < physical.slots.size(); ++s)
        {
            const auto index = physical.slots[s].resource.index;
            const auto& resource = plan_->resources()[index];
            auto& binding = frame.bindings[index];
            if (!alias_memory)
            {
                if (auto created = createResource(index, binding); !created)
                {
                    return created;
                }
            }
            else
            {
                nri::Result created;
                if (std::holds_alternative<GraphImageDesc>(resource.description))
                {
                    auto description = textureDescription(resource);
                    if (!description)
                    {
                        return cxx::unexpected(description.error());
                    }
                    created = core.CreateTexture(session.device(), *description, binding.image);
                }
                else
                {
                    binding.buffer_size = std::get<GraphBufferDesc>(resource.description).size;
                    const nri::BufferDesc description{binding.buffer_size, 4, resource.buffer_usage};
                    created = core.CreateBuffer(session.device(), description, binding.buffer);
                }
                if (created != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(created));
                }
            }
            if (binding.image)
            {
                frame.images.push_back(binding.image);
                if (alias_memory)
                {
                    core.GetTextureMemoryDesc(*binding.image, nri::MemoryLocation::DEVICE, requirements[s]);
                }
            }
            else
            {
                frame.buffers.push_back(binding.buffer);
                if (alias_memory)
                {
                    core.GetBufferMemoryDesc(*binding.buffer, nri::MemoryLocation::DEVICE, requirements[s]);
                }
            }
        }
        if (!alias_memory)
        {
            return {};
        }
        const auto placements = instantiatePlacements(physical, plan_->resources(), plan_->recording_, requirements);
        std::vector<nri::AllocateMemoryDesc> descriptions(physical.slots.size());
        for (std::uint32_t s = 0; s < placements.size(); ++s)
        {
            auto& description = descriptions[placements[s].allocation];
            const auto& required = requirements[s];
            description.size = std::max(description.size, required.size);
            description.type = required.type;
            description.vma.enable = !required.mustBeDedicated;
            description.vma.alignment = std::max(description.vma.alignment, required.alignment);
            const auto* image =
                std::get_if<GraphImageDesc>(&plan_->resources()[physical.slots[s].resource.index].description);
            description.allowMultisampleTextures |= image && image->samples > 1;
        }
        frame.memory_bindings = placements;
        for (std::uint32_t s = 0; s < placements.size(); ++s)
        {
            if (placements[s].allocation != s)
            {
                continue;
            }
            nri::Memory* memory = nullptr;
            const auto allocated = core.AllocateMemory(session.device(), descriptions[s], memory);
            if (allocated != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(allocated));
            }
            frame.memory_bindings[s].allocation = static_cast<std::uint32_t>(frame.memories.size());
            frame.memories.push_back({memory, descriptions[s].size});
        }
        std::vector<nri::BindBufferMemoryDesc> buffers;
        std::vector<nri::BindTextureMemoryDesc> images;
        for (std::uint32_t s = 0; s < placements.size(); ++s)
        {
            const auto group = frame.memory_bindings[placements[s].allocation].allocation;
            frame.memory_bindings[s].allocation = group;
            auto* memory = frame.memories[group].memory;
            const auto& binding = frame.bindings[physical.slots[s].resource.index];
            if (binding.image)
            {
                images.push_back({binding.image, memory, 0});
            }
            else
            {
                buffers.push_back({binding.buffer, memory, 0});
            }
        }
        if (!buffers.empty())
        {
            const auto status = core.BindBufferMemory(buffers.data(), static_cast<std::uint32_t>(buffers.size()));
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
        }
        if (!images.empty())
        {
            const auto status = core.BindTextureMemory(images.data(), static_cast<std::uint32_t>(images.size()));
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
        }
        return {};
    }

    void RenderGraphInstance::prepareAliasBarriers(FrameResources& frame) noexcept
    {
        std::vector<std::uint32_t> tails(frame.memories.size(), kGraphInvalidIndex);
        const auto& slots = plan_->physical_resources_.slots;
        for (std::uint32_t s = 0; s < frame.memory_bindings.size(); ++s)
        {
            auto& previous = tails[frame.memory_bindings[s].allocation];
            if (previous != kGraphInvalidIndex)
            {
                for (std::size_t q = 0; q < slots[s].first_pass.size(); ++q)
                {
                    const auto first = slots[s].first_pass[q];
                    if (first == kGraphInvalidIndex)
                    {
                        continue;
                    }
                    auto& barrier = frame.passes[first].alias_barrier;
                    if (!barrier)
                    {
                        barrier.emplace();
                    }
                    barrier->before.access |= slots[previous].accesses[q];
                    barrier->after.access |= slots[s].accesses[q];
                    // ALL is zero in NRI. Cross-Queue visibility comes from the already-proven Fence waits.
                    barrier->before.stages = barrier->after.stages = nri::StageBits::ALL;
                }
            }
            previous = s;
        }
    }

    RenderResult<nri::TextureDesc>
    RenderGraphInstance::textureDescription(const CompiledResource& resource) const noexcept
    {
        return textureDescription(resource, output_.description);
    }

    RenderResult<nri::TextureDesc>
    RenderGraphInstance::textureDescription(const CompiledResource& resource, const TargetDesc& target) const noexcept
    {
        auto& session = *session_;
        const auto max_extent = session.description().dimensions.texture2DMaxDim;
        const auto* image = &std::get<GraphImageDesc>(resource.description);
        const auto dimension = [&](std::uint32_t absolute, std::uint32_t target)
        {
            if (!resource.target_sized)
            {
                return std::uint64_t(absolute);
            }
            const auto& expression = image->extent;
            const auto scaled =
                (std::uint64_t(target) * expression.numerator + expression.denominator - 1) / expression.denominator;
            return (scaled + expression.alignment - 1) / expression.alignment * expression.alignment;
        };
        const auto width = dimension(image->width, target.width);
        const auto height = dimension(image->height, target.height);
        if (!width || !height || width > max_extent || height > max_extent ||
            image->mips > std::bit_width(std::max(width, height)))
        {
            return cxx::unexpected(RenderError{kInvalidTargetExtent});
        }
        const nri::TextureDesc description{
            nri::TextureType::TEXTURE_2D,
            resource.image_usage,
            imageFormat(image->format).native,
            static_cast<nri::Dim_t>(width),
            static_cast<nri::Dim_t>(height),
            1,
            static_cast<nri::Dim_t>(image->mips),
            static_cast<nri::Dim_t>(image->layers),
            static_cast<nri::Sample_t>(image->samples),
            nri::SharingMode::CONCURRENT
        };
        if (auto valid = session.validateTexture(description); !valid)
        {
            return cxx::unexpected(valid.error());
        }
        return description;
    }

    RenderResult<void> RenderGraphInstance::createResource(std::uint32_t index, Binding& binding) noexcept
    {
        return createResource(index, binding, output_.description);
    }

    RenderResult<void>
    RenderGraphInstance::createResource(std::uint32_t index, Binding& binding, const TargetDesc& target) noexcept
    {
        auto& session = *session_;
        const auto& core = session.nriCore();
        const auto& resource = plan_->resources()[index];
        if (std::holds_alternative<GraphImageDesc>(resource.description))
        {
            auto description = textureDescription(resource, target);
            if (!description)
            {
                return cxx::unexpected(description.error());
            }
            const auto status = core.CreatePlacedTexture(session.device(), NriDeviceHeap, *description, binding.image);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
        }
        else
        {
            const auto size = std::get<GraphBufferDesc>(resource.description).size;
            const nri::BufferDesc description{size, 4, resource.buffer_usage};
            const auto status = core.CreatePlacedBuffer(session.device(), NriDeviceHeap, description, binding.buffer);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
            binding.buffer_size = size;
        }
        return {};
    }

    RenderResult<void> RenderGraphInstance::createHistories() noexcept
    {
        histories_.resize(plan_->histories_.size());
        for (std::size_t index = 0; index < histories_.size(); ++index)
        {
            auto& history = histories_[index];
            history.plan = &plan_->histories_[index];
            history.slots.resize(frames_.size() + 1);
            for (auto& slot : history.slots)
            {
                Binding candidate;
                if (auto created = createResource(history.plan->description.current.index, candidate); !created)
                {
                    return created;
                }
                slot.image = candidate.image;
                slot.buffer = candidate.buffer;
                slot.buffer_size = candidate.buffer_size;
            }
            if (auto views = createHistoryViews(history); !views)
            {
                return views;
            }
        }
        return {};
    }

    RenderResult<void> RenderGraphInstance::createHistoryViews(HistoryState& history) noexcept
    {
        using Target = EImageViewTarget;
        const auto& core = session_->nriCore();
        const auto& description = history.plan->description;
        const auto belongs = [&](GraphResourceId id)
        { return id == description.previous || id == description.current; };
        const auto addView = [&](GraphResourceId resource,
                                 const VGraphRange& range,
                                 std::uint32_t pass,
                                 std::uint32_t index,
                                 Target target,
                                 EDescriptorKind kind) -> RenderResult<void>
        {
            if (!belongs(resource))
            {
                return {};
            }
            history.views.push_back({resource, pass, index, target});
            for (auto& slot : history.slots)
            {
                nri::Descriptor* view = nullptr;
                nri::Result result;
                if (slot.image)
                {
                    const auto& selected = std::get<GraphImageRange>(range);
                    const bool attachment = target != Target::DESCRIPTOR;
                    const bool depth = selected.aspects != 1;
                    const auto type =
                        attachment
                            ? (depth ? nri::TextureView::DEPTH_STENCIL_ATTACHMENT : nri::TextureView::COLOR_ATTACHMENT)
                            : (kind == EDescriptorKind::STORAGE_IMAGE ? nri::TextureView::STORAGE_TEXTURE
                                                                      : nri::TextureView::TEXTURE);
                    const auto& program = plan_->descriptor_plan_->passes[plan_->passes()[pass].source_pass];
                    const bool arrayed =
                        target == Target::DESCRIPTOR && program.bindings[program.uses[index].field].image_arrayed;
                    const auto view_type =
                        arrayed ? (kind == EDescriptorKind::STORAGE_IMAGE ? nri::TextureView::STORAGE_TEXTURE_ARRAY
                                                                          : nri::TextureView::TEXTURE_ARRAY)
                                : type;
                    nri::TextureViewDesc input{
                        slot.image,
                        view_type,
                        core.GetTextureDesc(*slot.image).format,
                        static_cast<nri::Dim_t>(selected.first_mip),
                        static_cast<nri::Dim_t>(selected.mip_count),
                        static_cast<nri::Dim_t>(selected.first_layer),
                        static_cast<nri::Dim_t>(selected.layer_count)
                    };
                    input.planes = selected.aspects == 1   ? nri::PlaneBits::COLOR
                                   : selected.aspects == 2 ? nri::PlaneBits::DEPTH
                                                           : nri::PlaneBits::STENCIL;
                    result = core.CreateTextureView(input, view);
                }
                else
                {
                    const auto& selected = std::get<GraphBufferRange>(range);
                    const nri::BufferViewDesc input{
                        slot.buffer,
                        kind == EDescriptorKind::UNIFORM_BUFFER         ? nri::BufferView::CONSTANT_BUFFER
                        : kind == EDescriptorKind::TYPED_STORAGE_BUFFER ? nri::BufferView::STORAGE_BUFFER
                                                                        : nri::BufferView::STORAGE_BYTE_ADDRESS_BUFFER,
                        selected.offset,
                        selected.size,
                        kind == EDescriptorKind::TYPED_STORAGE_BUFFER ? nri::Format::R32_UINT : nri::Format::UNKNOWN
                    };
                    result = core.CreateBufferView(input, view);
                }
                if (result != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
                slot.views.push_back(view);
            }
            return {};
        };
        for (std::uint32_t p = 0; p < plan_->passes().size(); ++p)
        {
            const auto& pass = plan_->passes()[p];
            const auto& uses = plan_->descriptor_plan_->passes[pass.source_pass].uses;
            for (std::uint32_t i = 0; i < uses.size(); ++i)
            {
                const auto& use = uses[i];
                if (auto created = addView({use.resource}, use.range, p, i, Target::DESCRIPTOR, use.kind); !created)
                {
                    return created;
                }
            }
            if (const auto* raster = std::get_if<RasterCommands>(&pass.work))
            {
                const auto addAttachment = [&](const RasterAttachment& attachment, std::uint32_t i, bool depth
                                           ) -> RenderResult<void>
                {
                    const auto& access = attachment.access;
                    if (auto view = addView(
                            access.version.resource,
                            access.range,
                            p,
                            i,
                            depth ? Target::DEPTH : Target::COLOR,
                            EDescriptorKind::SAMPLED_IMAGE
                        );
                        !view)
                    {
                        return view;
                    }
                    if (attachment.resolve)
                    {
                        const auto& resolve = *attachment.resolve;
                        return addView(
                            resolve.version.resource,
                            resolve.range,
                            p,
                            i,
                            depth ? Target::DEPTH_RESOLVE : Target::COLOR_RESOLVE,
                            EDescriptorKind::SAMPLED_IMAGE
                        );
                    }
                    return {};
                };
                for (std::uint32_t i = 0; i < raster->colors.size(); ++i)
                {
                    if (auto created = addAttachment(raster->colors[i], i, false); !created)
                    {
                        return created;
                    }
                }
                if (raster->depth)
                {
                    if (auto created = addAttachment(*raster->depth, 0, true); !created)
                    {
                        return created;
                    }
                }
            }
            if (const auto* transfer = std::get_if<TransferWork>(&pass.work))
            {
                for (std::uint32_t i = 0; i < transfer->operations.size(); ++i)
                {
                    if (const auto* clear = std::get_if<ClearImage>(&transfer->operations[i]))
                    {
                        const auto& access = clear->destination;
                        if (auto created = addView(
                                access.version.resource,
                                access.range,
                                p,
                                i,
                                Target::CLEAR,
                                EDescriptorKind::SAMPLED_IMAGE
                            );
                            !created)
                        {
                            return created;
                        }
                    }
                }
            }
        }
        // Initialization views are immutable and shared by the safe frame descriptor banks.
        for (auto& slot : history.slots)
        {
            if (!slot.image)
            {
                continue;
            }
            const auto& image = core.GetTextureDesc(*slot.image);
            const auto& graph_image =
                std::get<GraphImageDesc>(plan_->resources()[description.current.index].description);
            const bool depth = rdesc::textureAspectMask(graph_image.format) != 1;
            for (nri::Dim_t mip = 0; mip < image.mipNum; ++mip)
            {
                nri::Descriptor* view = nullptr;
                const nri::TextureViewDesc input{
                    slot.image,
                    depth ? nri::TextureView::DEPTH_STENCIL_ATTACHMENT : nri::TextureView::COLOR_ATTACHMENT,
                    image.format,
                    mip,
                    1,
                    0,
                    image.layerNum
                };
                const auto result = core.CreateTextureView(input, view);
                if (result != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
                slot.initial_views.push_back(view);
            }
        }
        return {};
    }

    bool RenderGraphInstance::selectHistory() noexcept
    {
        return std::ranges::all_of(histories_, [&](auto& history) { return history.select(*session_); });
    }

    void RenderGraphInstance::bindHistory(std::uint32_t frame_slot) noexcept
    {
        auto& frame = frames_[frame_slot];
        for (std::size_t h = 0; h < histories_.size(); ++h)
        {
            const auto& history = histories_[h];
            frame.history_valid[h] = history.latest != kGraphInvalidIndex;
            const auto& description = history.plan->description;
            for (const auto [resource, selected] :
                 {std::pair{description.previous, history.previous}, std::pair{description.current, history.current}})
            {
                const bool was_dirty = frame.bindings[resource.index].dirty;
                const auto& physical = history.slots[selected];
                frame.bindings[resource.index].image = physical.image;
                frame.bindings[resource.index].buffer = physical.buffer;
                frame.bindings[resource.index].buffer_size = physical.buffer_size;
                frame.bindings[resource.index].dirty = true;
                if (!was_dirty)
                {
                    frame.changed_resources.push_back(resource.index);
                }
            }
        }
    }

    void RenderGraphInstance::updateHistoryViews(std::uint32_t frame_slot) noexcept
    {
        using Target = EImageViewTarget;
        auto& frame = frames_[frame_slot];
        const auto& core = session_->nriCore();
        for (std::size_t h = 0; h < histories_.size(); ++h)
        {
            const auto& history = histories_[h];
            const auto& description = history.plan->description;
            for (std::size_t i = 0; i < history.views.size(); ++i)
            {
                const auto& use = history.views[i];
                const auto selected = use.resource == description.previous ? history.previous : history.current;
                auto* view = history.slots[selected].views[i];
                auto& pass = frame.passes[use.pass];
                switch (use.target)
                {
                case Target::DESCRIPTOR:
                {
                    const auto& binding =
                        plan_->descriptor_plan_->passes[pass.compiled_pass->source_pass].uses[use.index];
                    if (!frame.history_valid[h] && binding.fallback != kGraphInvalidIndex)
                    {
                        view = pass.private_views[use.index];
                    }
                    const nri::UpdateDescriptorRangeDesc
                        update{pass.private_descriptors, binding.binding, binding.element, &view, 1};
                    core.UpdateDescriptorRanges(&update, 1);
                    pass.history_views[use.index] = view;
                    break;
                }
                case Target::COLOR:
                    pass.colors[use.index].descriptor = view;
                    break;
                case Target::DEPTH:
                    pass.depth.descriptor = view;
                    break;
                case Target::COLOR_RESOLVE:
                    pass.colors[use.index].resolveDst = view;
                    break;
                case Target::DEPTH_RESOLVE:
                    pass.depth.resolveDst = view;
                    break;
                case Target::CLEAR:
                    pass.clear_views[use.index] = view;
                    break;
                }
            }
        }
    }

    RenderResult<void> RenderGraphInstance::invalidateHistory(std::span<const GraphResourceId> previous) noexcept
    {
        // Validate the whole request before changing any instance state.
        for (auto resource : previous)
        {
            const bool valid = resource.index < plan_->resources().size() &&
                               plan_->resources()[resource.index].origin == EGraphOrigin::HISTORY_PREVIOUS &&
                               plan_->resources()[resource.index].history != kGraphInvalidIndex;
            if (!valid)
            {
                return cxx::unexpected(RenderError{kGraphUnknownResource});
            }
        }
        if (previous.empty())
        {
            for (auto& history : histories_)
            {
                history.invalidate = true;
            }
        }
        else
        {
            for (auto resource : previous)
            {
                histories_[plan_->resources()[resource.index].history].invalidate = true;
            }
        }
        return {};
    }

    void RenderGraphInstance::publishHistory(std::uint32_t frame_slot) noexcept
    {
        if (histories_.empty())
        {
            return;
        }
        const auto& frame = frames_[frame_slot];
        for (auto& history : histories_)
        {
            const bool produced = std::ranges::all_of(history.plan->writers, [&](auto writer)
            {
                const auto region = plan_->passes()[writer].condition_region;
                return region == kGraphInvalidIndex || frame.enabled_regions[region] != 0;
            });
            if (produced)
            {
                history.publish();
            }
        }
    }

    RenderResult<void> RenderGraphInstance::prepareFrameBindings(std::uint32_t slot) noexcept
    {
        auto& session = *session_;
        const auto& core = session.nriCore();
        const auto& plan = *plan_;
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
        auto attachmentView = [&](const ResourceAccess& access, bool depth, nri::Descriptor*& result
                              ) -> RenderResult<void>
        {
            const auto index = access.version.resource.index;
            if (index == plan.output())
            {
                result = output_.frames[slot].view;
                return {};
            }
            if (plan.resources()[index].history != kGraphInvalidIndex)
            {
                return {}; // The selected physical History bank supplies its already-created view.
            }
            const auto& range = std::get<GraphImageRange>(access.range);
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
            frame.views.push_back(result);
            return {};
        };
        auto attachment = [&](const RasterAttachment& input, bool depth, nri::AttachmentDesc& result
                          ) -> RenderResult<void>
        {
            auto view = attachmentView(input.access, depth, result.descriptor);
            if (!view)
            {
                return view;
            }
            if (input.resolve)
            {
                auto resolved = attachmentView(*input.resolve, depth, result.resolveDst);
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
            Pass pass{&compiled_pass};
            auto extent = preparePassExtent(
                session,
                plan,
                compiled_pass,
                [&](std::uint32_t resource) -> const Binding& { return frame.bindings[resource]; },
                pass.dispatch_groups,
                pass.copy_layouts
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
            if (const auto* raster = std::get_if<RasterCommands>(&compiled_pass.work))
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
            if (const auto* transfer = std::get_if<TransferWork>(&compiled_pass.work))
            {
                pass.copy_layouts.resize(transfer->operations.size());
                pass.clear_views.resize(transfer->operations.size());
                for (std::size_t i = 0; i < transfer->operations.size(); ++i)
                {
                    const auto& operation = transfer->operations[i];
                    if (const auto* clear = std::get_if<ClearImage>(&operation))
                    {
                        const auto& image = std::get<GraphImageDesc>(
                            plan.resources()[clear->destination.version.resource.index].description
                        );
                        auto ready = attachmentView(
                            clear->destination,
                            rdesc::textureAspectMask(image.format) != 1,
                            pass.clear_views[i]
                        );
                        if (!ready)
                        {
                            return ready;
                        }
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
            if (const auto* raster = std::get_if<RasterCommands>(&pass.compiled_pass->work))
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
            else if (const auto* transfer = std::get_if<TransferWork>(&pass.compiled_pass->work))
            {
                for (std::size_t i = 0; i < transfer->operations.size(); ++i)
                {
                    const auto* clear = std::get_if<ClearImage>(&transfer->operations[i]);
                    if (clear && clear->destination.version.resource.index == plan.output())
                    {
                        frame.output_views.push_back(&pass.clear_views[i]);
                    }
                }
            }
        }
        return {};
    }

    RenderResult<void> RenderGraphInstance::prepareDescriptors() noexcept
    {
        const auto& layout = *plan_->descriptor_plan_;
        // Actual input parameter offsets are qualified once at Attach. Physical slices
        // share that type/layout; only their values and GPU addresses differ at recording.
        for (std::uint32_t p = 0; p < plan_->passes().size(); ++p)
        {
            const auto& compiled_pass = plan_->passes()[p];
            if (const auto* raster = std::get_if<RasterCommands>(&compiled_pass.work); raster && raster->state_input)
            {
                const auto& input = *raster->state_input;
                const auto bytes = frames_[0].bindings[input.resource.index].parameters.size();
                if (input.offset > bytes || sizeof(RasterState) > bytes - input.offset)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
            }
            if (std::holds_alternative<TransferWork>(compiled_pass.work))
            {
                continue;
            }
            const auto& pass_layout = layout.passes[compiled_pass.source_pass];
            for (const auto& mapping : pass_layout.parameters)
            {
                if (mapping.resource == kGraphInvalidIndex) continue;
                const ResourceInput source{
                    nullptr, 0, 0, frames_[0].bindings[mapping.resource].parameters,
                    frames_[0].bindings[mapping.resource].parameter_layout
                };
                if (!source.parameter_layout)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                const auto& fields = source.parameter_layout->fields;
                const auto field = std::ranges::find(fields, mapping.source_field, &rdesc::ShaderMember::path);
                if (field == fields.end())
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                const auto& destination = mapping.destination;
                const bool mismatch = field->scalar != destination.scalar || field->rows != destination.rows ||
                                      field->columns != destination.columns ||
                                      field->matrix_stride != destination.matrix_stride ||
                                      field->array_stride != destination.array_stride;
                const auto bytes = std::uint64_t(field->rows) * 4 +
                                   (field->columns ? std::uint64_t(field->columns - 1) * field->matrix_stride : 0);
                const bool invalid = !bytes || bytes > UINT32_MAX ||
                                     std::uint64_t(field->offset) + bytes > source.parameters.size() ||
                                     std::uint64_t(destination.offset) + bytes > compiled_pass.parameter_size;
                if (mismatch || invalid)
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
                parameter_copies_[p].push_back(
                    {mapping.resource,
                     field->offset,
                     destination.offset,
                     std::uint32_t(bytes)}
                );
            }
        }
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            for (std::uint32_t pass = 0; pass < plan_->passes().size(); ++pass)
            {
                const auto* raster = std::get_if<RasterCommands>(&plan_->passes()[pass].work);
                if (!layout.passes[plan_->passes()[pass].source_pass].parameters.empty() ||
                    (raster && raster->state_input))
                {
                    markParameters(pass, slot);
                }
            }
        }
        nri::DescriptorPoolDesc pool{};
        for (const auto& pass : plan_->passes())
        {
            if (auto counted = FrameResources::addDescriptors(pool, layout.passes[pass.source_pass]); !counted)
                return counted;
        }
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            auto& frame = frames_[slot];
            if (pool.descriptorSetMaxNum)
            {
                const auto status = session_->nriCore().CreateDescriptorPool(*device(), pool, frame.descriptor_pool);
                if (status != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(status));
                }
            }
            frame.descriptor_capacity = frame.descriptor_reserved = pool;
            for (auto& pass : frame.passes)
            {
                const auto& program = layout.passes[pass.compiled_pass->source_pass];
                pass.private_views.resize(program.uses.size());
                pass.history_views.resize(program.uses.size());
                if (!program.private_bindings.empty())
                {
                    const auto status = session_->nriCore().AllocateDescriptorSets(
                        *frame.descriptor_pool,
                        *program.layout,
                        program.private_set,
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
                        auto* sampler = samplers_[use.sampler].descriptor;
                        if (!sampler)
                        {
                            return cxx::unexpected(RenderError{kDeviceArgument});
                        }
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

    bool RenderGraphInstance::dumpResources(DumpSink sink) const noexcept
    {
        if (!sink.write)
        {
            return false;
        }
        std::string output = "{\"allocation\":\"compiled physical slots\",\"frames\":[";
        for (std::size_t i = 0; i < frames_.size(); ++i)
        {
            if (i)
            {
                output += ',';
            }
            const auto& frame = frames_[i];
            std::uint64_t bytes = 0;
            for (auto* buffer : frame.buffers)
            {
                nri::MemoryDesc memory{};
                session_->nriCore().GetBufferMemoryDesc(*buffer, nri::MemoryLocation::DEVICE, memory);
                bytes += memory.size;
            }
            for (auto* image : frame.images)
            {
                nri::MemoryDesc memory{};
                session_->nriCore().GetTextureMemoryDesc(*image, nri::MemoryLocation::DEVICE, memory);
                bytes += memory.size;
            }
            for (const auto& list : frame.draw_lists)
            {
                for (auto* buffer : {list.vertices, list.indices})
                {
                    nri::MemoryDesc memory{};
                    session_->nriCore().GetBufferMemoryDesc(*buffer, nri::MemoryLocation::DEVICE, memory);
                    bytes += memory.size;
                }
            }
            const auto allocated = std::accumulate(
                frame.memories.begin(),
                frame.memories.end(),
                std::uint64_t{0},
                [](auto sum, const auto& allocation) { return sum + allocation.bytes; }
            );
            std::size_t command_count = 0;
            for (const auto& batch : frame.command_buffers)
                command_count += std::ranges::count_if(
                    batch, [](auto* command) noexcept { return command != nullptr; }
                );
            output += "{\"slot\":" + std::to_string(i) +
                      ",\"imported_revision\":" + std::to_string(frame.imported_revision) +
                      ",\"owned_command_buffers\":" + std::to_string(command_count) +
                      ",\"owned_buffers\":" + std::to_string(frame.buffers.size() + frame.draw_lists.size() * 2) +
                      ",\"owned_textures\":" + std::to_string(frame.images.size()) +
                      ",\"allocation_requirement_bytes\":" + std::to_string(bytes) +
                      ",\"explicit_memory_count\":" + std::to_string(frame.memories.size()) +
                      ",\"explicit_memory_bytes\":" + std::to_string(allocated) + ",\"memory_bindings\":[";
            constexpr std::array
                reasons{"excluded", "dedicated", "rendering_scope", "distinct_requirements", "unordered", "aliased"};
            for (std::size_t s = 0; s < frame.memory_bindings.size(); ++s)
            {
                if (s)
                {
                    output += ',';
                }
                const auto& binding = frame.memory_bindings[s];
                output += "{\"allocation\":" + std::to_string(binding.allocation) + ",\"reason\":\"" +
                          reasons[static_cast<std::size_t>(binding.reason)] + "\"}";
            }
            output += "]}";
        }
        output += "],\"histories\":[";
        for (std::size_t i = 0; i < histories_.size(); ++i)
        {
            if (i)
            {
                output += ',';
            }
            const auto& history = histories_[i];
            output += "{\"previous_resource\":" + std::to_string(history.plan->description.previous.index) +
                      ",\"current_resource\":" + std::to_string(history.plan->description.current.index) +
                      ",\"sequence\":" + std::to_string(history.sequence) +
                      ",\"latest_slot\":" + std::to_string(history.latest) + ",\"slots\":[";
            for (std::size_t s = 0; s < history.slots.size(); ++s)
            {
                if (s)
                {
                    output += ',';
                }
                const auto& slot = history.slots[s];
                nri::MemoryDesc memory{};
                if (slot.image)
                {
                    session_->nriCore().GetTextureMemoryDesc(*slot.image, nri::MemoryLocation::DEVICE, memory);
                }
                else
                {
                    session_->nriCore().GetBufferMemoryDesc(*slot.buffer, nri::MemoryLocation::DEVICE, memory);
                }
                output += "{\"sample\":" + std::to_string(slot.sample_sequence) +
                          ",\"initialized\":" + (slot.initialized ? "true" : "false") +
                          ",\"allocation_requirement_bytes\":" + std::to_string(memory.size) + ",\"last_use\":[";
                for (std::size_t q = 0; q < session_->queueCount(); ++q)
                {
                    if (q)
                    {
                        output += ',';
                    }
                    output += std::to_string(slot.last_use.serials[q]);
                }
                output += "]}";
            }
            output += "]}";
        }
        output += "],\"excludes\":\"borrowed Target/inputs, allocator pool reserve and descriptors\"}";
        return sink.write(sink.destination, output);
    }

    nri::Buffer* RenderGraphInstance::nativeBuffer(std::uint32_t resource, std::uint32_t slot) const noexcept
    {
        return frames_[slot].bindings[resource].buffer;
    }

    void
    RenderGraphInstance::setResource(std::uint32_t resource, std::uint32_t slot, const ResourceInput& value) noexcept
    {
        const auto first = slot == UINT32_MAX ? 0 : slot;
        const auto end = slot == UINT32_MAX ? static_cast<std::uint32_t>(frames_.size()) : slot + 1;
        for (auto current = first; current < end; ++current)
        {
            auto& frame = frames_[current];
            auto& binding = frame.bindings[resource];
            const bool changed_physical = binding.buffer != value.buffer || binding.buffer_offset != value.offset ||
                                          binding.buffer_size != value.size || binding.image != value.image;
            const bool changed_addresses = binding.addressed_revision != value.addressed_revision ||
                                           binding.addressed_buffers.data() != value.addressed_buffers.data() ||
                                           binding.addressed_buffers.size() != value.addressed_buffers.size();
            binding.addressed_buffers = value.addressed_buffers;
            binding.addressed_revision = value.addressed_revision;
            if ((changed_physical || changed_addresses) &&
                plan_->resources()[resource].origin == EGraphOrigin::IMPORTED)
            {
                frame.invalidateImportedBindings();
            }
            if (plan_->resources()[resource].memory_used &&
                std::holds_alternative<GraphBufferDesc>(plan_->resources()[resource].description))
            {
                if (!binding.buffer && value.buffer)
                {
                    --frame.unavailable_buffers;
                }
                else if (binding.buffer && !value.buffer)
                {
                    ++frame.unavailable_buffers;
                }
            }
            if (binding.buffer && binding.buffer != value.buffer)
            {
                // Private raster recipes borrow this physical resource too. A later pool rebuild
                // must not replay a view whose Buffer owner has already replaced its allocation.
                std::vector<nri::Descriptor*> obsolete;
                for (auto& record : frame.raster_bindings)
                {
                    for (std::size_t i = 0; i < record.inputs.size(); ++i)
                    {
                        if (record.inputs[i].buffer != binding.buffer) continue;
                        if (auto* view = std::exchange(record.views[i], nullptr)) obsolete.push_back(view);
                        record.inputs[i] = {};
                    }
                }
                if (!obsolete.empty())
                    session_->deferRelease([core = &session_->nriCore(), views = std::move(obsolete)]() noexcept
                    {
                        for (auto* view : views) core->DestroyDescriptor(view);
                    });
            }
            binding.image = value.image;
            binding.buffer = value.buffer;
            binding.buffer_offset = value.offset;
            binding.buffer_size = value.size;
            const bool changed_parameters = value.parameter_revision == 0 ||
                                            binding.parameter_revision != value.parameter_revision ||
                                            binding.parameters.data() != value.parameters.data() ||
                                            binding.parameters.size() != value.parameters.size() ||
                                            binding.parameter_layout != value.parameter_layout;
            binding.parameters = value.parameters;
            binding.parameter_layout = value.parameter_layout;
            binding.parameter_revision = value.parameter_revision;
            if (changed_parameters)
            {
                for (auto pass : plan_->descriptor_plan_->parameter_uses[resource])
                {
                    markParameters(pass, current);
                }
            }
            if (changed_physical) invalidateRecordings(current);
            if (changed_physical && !binding.dirty)
            {
                binding.dirty = true;
                frame.changed_resources.push_back(resource);
            }
        }
    }

    RenderResult<void> RenderGraphInstance::rebind(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        const auto& layout = *plan_->descriptor_plan_;
        const auto& core = session_->nriCore();
        for (auto resource : frame.changed_resources)
        {
            auto& source = frame.bindings[resource];
            const auto& uses = plan_->resources()[resource];
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
                    const bool initial_graphics = pass.queue == EQueueRole::GRAPHICS &&
                                                  pass.before.buffers[index].previous.index == kGraphInvalidIndex;
                    if (initial_graphics && barrier.before.stages == nri::StageBits::ALL &&
                        barrier.before.access == nri::AccessBits::NONE)
                    {
                        barrier.before.access = bufferAccess(core.GetBufferDesc(*source.buffer).usage);
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
                const auto& program = layout.passes[pass.compiled_pass->source_pass];
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
                    const bool invalid_range =
                        range.offset > source.buffer_size || size > source.buffer_size - range.offset;
                    if (invalid_range)
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                    const nri::BufferViewDesc description{
                        source.buffer,
                        use.kind == EDescriptorKind::UNIFORM_BUFFER ? nri::BufferView::CONSTANT_BUFFER
                        : use.kind == EDescriptorKind::TYPED_STORAGE_BUFFER
                            ? nri::BufferView::STORAGE_BUFFER
                            : nri::BufferView::STORAGE_BYTE_ADDRESS_BUFFER,
                        source.buffer_offset + range.offset,
                        size,
                        use.kind == EDescriptorKind::TYPED_STORAGE_BUFFER ? nri::Format::R32_UINT : nri::Format::UNKNOWN
                    };
                    status = core.CreateBufferView(description, candidate);
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
                invalidateRecordings(slot);
            }
            source.dirty = false;
        }
        frame.changed_resources.clear();
        return {};
    }

    RenderResult<void> RenderGraphInstance::setParameterInput(
        std::span<const std::byte> input,
        const ParameterProjection& projection
    ) noexcept
    {
        auto destination = std::span(instance_values_).subspan(projection.offset, projection.bytes);
        auto applied = projection.apply(projection.type, input, destination);
        if (applied)
        {
            markInstanceParameters(projection.offset, projection.bytes);
        }
        return applied;
    }

    RenderResult<void> RenderGraphInstance::setInputs(
        std::span<const std::byte> parameters,
        std::span<const std::uint8_t> conditions,
        cxx::type_info parameter_type,
        std::span<const ParameterProjection> projections
    ) noexcept
    {
        const bool invalid_conditions = conditions.size() != condition_values_.size() ||
                                        std::ranges::any_of(conditions, [](auto value) { return value > 1; });
        if (invalid_conditions)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        if (parameter_type.isValid())
        {
            // Initial Attach/explicit cold update is atomic across all matching capability regions.
            // The frame path writes one qualified region directly through setParameterInput.
            auto candidate = instance_values_;
            bool matched = false;
            for (const auto& projection : projections)
            {
                if (projection.type != parameter_type)
                {
                    continue;
                }
                auto applied = projection.apply(
                    parameter_type,
                    parameters,
                    std::span(candidate).subspan(projection.offset, projection.bytes)
                );
                if (!applied)
                {
                    return applied;
                }
                matched = true;
            }
            if (!matched)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            instance_values_.swap(candidate);
        }
        else
        {
            if (parameters.size() != instance_values_.size())
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            std::ranges::copy(parameters, instance_values_.begin());
        }
        std::ranges::copy(conditions, condition_values_.begin());
        markInstanceParameters(0, static_cast<std::uint32_t>(instance_values_.size()));
        return {};
    }

    RenderResult<void> RenderGraphInstance::setConditions(std::span<const std::uint8_t> values) noexcept
    {
        if (values.size() != condition_values_.size() ||
            std::ranges::any_of(values, [](auto value) { return value > 1; }))
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        std::ranges::copy(values, condition_values_.begin());
        return {};
    }

    RenderResult<bool> RenderGraphInstance::prepareFrame(std::uint32_t slot) noexcept
    {
        if (submission_error_.type != 0)
        {
            return cxx::unexpected(submission_error_);
        }
        auto& frame = frames_[slot];
        if (plan_->output() != kGraphInvalidIndex)
        {
            const auto& output = output_.frames[slot];
            auto& binding = frame.bindings[plan_->output()];
            if (binding.image != output.image)
            {
                binding.image = output.image;
                // A presentation image has one writer and cannot be sampled by another instance.
                // Rotation changes this binding and its barriers, not the cross-instance dependency graph.
                if (output_.description.kind != ETargetKind::SURFACE)
                {
                    frame.invalidateImportedBindings();
                }
                if (!binding.dirty)
                {
                    binding.dirty = true;
                    frame.changed_resources.push_back(plan_->output());
                }
                for (auto* view : frame.output_views)
                {
                    *view = output.view;
                }
            }
        }
        if (frame.unavailable_buffers)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        if (!selectHistory())
        {
            return false;
        }
        bindHistory(slot);
        frame.enabled_pass_count = static_cast<std::uint32_t>(frame.passes.size());
        for (std::size_t r = 0; r < plan_->recording_.conditions.size(); ++r)
        {
            const auto& region = plan_->recording_.conditions[r];
            const auto& condition = region.predicate;
            const bool value = condition.source == EConditionSource::INPUT
                                   ? condition_values_[condition.input] != 0
                                   : frame.history_valid[plan_->resources()[condition.input].history] != 0;
            const bool enabled = value == condition.expected;
            if (frame.enabled_regions[r] != enabled)
            {
                for (auto p = region.first_pass; p < region.first_pass + region.pass_count; ++p)
                    invalidateRecording(slot, p);
            }
            frame.enabled_regions[r] = enabled;
            if (!frame.enabled_regions[r])
            {
                frame.enabled_pass_count -= region.pass_count;
            }
        }
        if (!frame.changed_resources.empty())
        {
            if (auto rebound = rebind(slot); !rebound)
            {
                return cxx::unexpected(rebound.error());
            }
        }
        updateHistoryViews(slot);
        if (frame.viewport != viewport_)
        {
            for (std::uint32_t p = 0; p < frame.passes.size(); ++p)
            {
                const auto* raster = std::get_if<RasterCommands>(&frame.passes[p].compiled_pass->work);
                if (raster && !raster->state_input && !raster->viewport) invalidateRecording(slot, p);
            }
            frame.viewport = viewport_;
        }
        const auto shared_revision = session_->descriptors().bindingRevision();
        if (frame.shared_binding_revision != shared_revision)
        {
            invalidateRecordings(slot);
            frame.shared_binding_revision = shared_revision;
        }
        if (auto parameters = prepareParameters(slot); !parameters)
        {
            return cxx::unexpected(parameters.error());
        }
        prepareStreamedDraws(slot);
        return true;
    }

    void RenderGraphInstance::markParameters(std::uint32_t pass, std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        if (frame.parameter_dirty[pass] == 0)
        {
            frame.parameter_updates.push_back(pass);
        }
        frame.parameter_dirty[pass] = 1;
    }

    void RenderGraphInstance::markInstanceParameters(std::uint32_t offset, std::uint32_t bytes) noexcept
    {
        for (const auto& region : plan_->descriptor_plan_->parameter_regions)
        {
            if (offset >= region.offset + region.layout.size || region.offset >= offset + bytes) continue;
            for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
            {
                for (auto pass : region.passes) markParameters(pass, slot);
            }
        }
    }

    RenderResult<void> RenderGraphInstance::prepareParameters(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        // Unversioned external borrows retain their prior live semantics. Versioned inputs and
        // owned instance defaults are copied only into the FIF slices they actually invalidated.
        for (auto resource : plan_->descriptor_plan_->parameter_resources)
        {
            if (frame.bindings[resource].parameter_revision == 0)
            {
                for (auto pass : plan_->descriptor_plan_->parameter_uses[resource])
                {
                    markParameters(pass, slot);
                }
            }
        }
        for (auto p : frame.parameter_updates)
        {
            if (const auto* raster = std::get_if<RasterCommands>(&frame.passes[p].compiled_pass->work);
                raster && raster->state_input)
            {
                const auto& input = *raster->state_input;
                auto& state = frame.passes[p].raster_state;
                RasterState incoming;
                std::memcpy(
                    &incoming, frame.bindings[input.resource.index].parameters.data() + input.offset, sizeof(incoming)
                );
                if (incoming.viewport != state.viewport || incoming.depth_bias_constant != state.depth_bias_constant ||
                    incoming.depth_bias_slope != state.depth_bias_slope)
                {
                    state = incoming;
                    invalidateRecording(slot, p);
                }
                const bool invalid_bias =
                    !std::isfinite(state.depth_bias_constant) || !std::isfinite(state.depth_bias_slope);
                if (invalid_bias || !state.viewport.validate())
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
            }
            auto& parameters = frame.passes[p].parameters;
            const auto& compiled = plan_->passes()[p];
            for (const auto& mapping : plan_->descriptor_plan_->passes[compiled.source_pass].parameters)
            {
                if (mapping.resource != kGraphInvalidIndex) continue;
                const auto& field = mapping.destination;
                const auto bytes = std::size_t(field.rows) * 4 +
                    (field.columns ? std::size_t(field.columns - 1) * field.matrix_stride : 0);
                auto* destination = parameters.data() + field.offset;
                const auto* source = instance_values_.data() + mapping.source_offset;
                if (std::memcmp(destination, source, bytes) != 0)
                {
                    std::memcpy(destination, source, bytes);
                    invalidateRecording(slot, p);
                }
            }
            for (const auto& copy : parameter_copies_[p])
            {
                const auto* source = frame.bindings[copy.resource].parameters.data();
                auto* destination = parameters.data() + copy.destination_offset;
                if (std::memcmp(destination, source + copy.source_offset, copy.bytes) != 0)
                {
                    std::memcpy(destination, source + copy.source_offset, copy.bytes);
                    invalidateRecording(slot, p);
                }
            }
        }

        for (auto pass : frame.parameter_updates)
        {
            frame.parameter_dirty[pass] = 0;
        }
        frame.parameter_updates.clear();
        return {};
    }

    void RenderGraphInstance::acceptedBatch(std::uint32_t batch, std::size_t queue, std::uint64_t serial) noexcept
    {
        const auto mark = [&](HistoryState::HistorySlot& slot)
        {
            slot.last_use.serials[queue] = std::max(slot.last_use.serials[queue], serial);
            slot.initialized = true;
        };
        if (batch == 0)
        {
            for (auto& history : histories_)
            {
                if (history.clear_previous)
                {
                    mark(history.slots[history.previous]);
                }
                if (history.clear_current)
                {
                    mark(history.slots[history.current]);
                }
            }
        }
        for (auto resource : plan_->submissions_[batch].histories)
        {
            const auto& compiled = plan_->resources_[resource.index];
            auto& history = histories_[compiled.history];
            mark(history.slots[compiled.origin == EGraphOrigin::HISTORY_PREVIOUS ? history.previous : history.current]);
        }
    }

    void RenderGraphInstance::failedSubmission(RenderError error) noexcept
    {
        submission_error_ = error;
    }

    std::uint32_t RenderGraphInstance::recordedPassCount(std::uint32_t slot) const noexcept
    {
        return frames_[slot].enabled_pass_count;
    }

    void RenderGraphInstance::submitted(std::uint32_t slot) noexcept
    {
        publishHistory(slot);
        progress_.last_submitted_slot = slot;
        ++progress_.counts[static_cast<std::size_t>(EExecutionOutcome::EXECUTED)];
    }
} // namespace lux::render::detail
