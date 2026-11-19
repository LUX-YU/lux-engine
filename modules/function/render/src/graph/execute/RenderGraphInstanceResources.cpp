#include <algorithm>
#include <bit>
#include <numeric>
#include <limits>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/execute/PassExtent.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/graph/execute/FrameResources.hpp>
#include <lux/engine/render/graph/compile/PhysicalResourcePlan.hpp>
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>

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

    RenderResult<void> RenderGraphInstance::createData(
        const GraphAssemblyPlan& assembly,
        std::span<const InstanceDataRecipe> recipes,
        ResourceRegistry& resources,
        std::span<const ResourceInstanceId> resolved
    ) noexcept
    {
        resources_ = &resources;
        if (!data_.empty()) return cxx::unexpected(RenderError{kDeviceArgument});
        source_pass_slots_ = assembly.source_pass_slots;
        cxx::scope_exit release_source_mapping([&]() noexcept { source_pass_slots_ = {}; });
        for (auto index : assembly.instance_recipes)
        {
            if (index >= recipes.size()) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            const auto& recipe = recipes[index];
            const auto [offset, bytes] = plan_->parameterRegion(recipe.parameter_group, recipe.parameter_source);
            if (recipe.parameter_bytes > bytes) return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
            auto created = recipe.create(*session_, resources, *this, resolved, recipe.configuration.data());
            if (!created) return cxx::unexpected(created.error());
            data_.push_back({recipe.inputs, std::move(*created), offset, bytes, {}});
            data_.back().inputs.resize(recipe.inputs.size());
        }
        return {};
    }

    RenderResult<bool> RenderGraphInstance::prepareData(
        std::uint32_t slot, ResourceUploader& uploads, Preparation& preparation
    ) noexcept
    {
        for (auto& data : data_)
        {
            for (std::size_t i = 0; i < data.inputs.size(); ++i)
                data.inputs[i] = resourceInput(data.input_slots[i], slot);
            const InstanceInputs input{slot,
                instanceParameters().subspan(data.parameter_offset, data.parameter_bytes), data.inputs};
            auto ready = data.value->prepare(input, uploads, preparation);
            if (!ready || !*ready) return ready;
        }
        return true;
    }

    bool RenderGraphInstance::requiresDataPrograms() const noexcept
    {
        return std::ranges::any_of(data_, [](const auto& data) { return data.value->requiresPrograms(); });
    }

    void RenderGraphInstance::captureProgramRevisions(std::vector<std::uint64_t>& revisions) const noexcept
    {
        revisions.clear();
        revisions.reserve(data_.size());
        for (const auto& data : data_) revisions.push_back(data.value->programRevision());
    }

    bool RenderGraphInstance::matchesProgramRevisions(std::span<const std::uint64_t> revisions) const noexcept
    {
        return std::ranges::equal(data_, revisions, {}, [](const auto& data) noexcept
        {
            return data.value->programRevision();
        });
    }

    exec::task<RenderResult<bool>> RenderGraphInstance::prepareDataPrograms(
        execution::CpuScheduler cpu, std::span<const std::uint64_t> revisions, stdexec::inplace_stop_token stop
    ) noexcept
    {
        if (stop.stop_requested() || !matchesProgramRevisions(revisions)) co_return false;
        for (std::size_t i = 0; i < data_.size(); ++i)
        {
            auto& data = *data_[i].value;
            if (!data.requiresPrograms()) continue;
            auto compiled = co_await data.preparePrograms(cpu, revisions[i], stop);
            if (stop.stop_requested() || !matchesProgramRevisions(revisions)) co_return false;
            if (!compiled || !*compiled) co_return compiled;
        }
        co_return true;
    }

    void RenderGraphInstance::bindImage(GraphResourceId resource, std::uint32_t slot, nri::Texture& image) noexcept
    {
        auto binding = resourceInput(resource, slot);
        if (binding.image == &image)
        {
            return;
        }
        binding.image = &image;
        setResource(resource.index, slot, binding);
    }

    RenderResult<void> RenderGraphInstance::createDrawListResources(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        const auto& core = session_->nriCore();
        for (std::size_t p = 0; p < plan_->passes().size(); ++p)
        {
            const auto& pass = plan_->passes()[p];
            const auto* raster = plan_->rasterCommands(pass);
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
                {physical.vertex_capacity, 0, nri::BufferUsageBits::VERTEX},
                physical.vertices
            );
            if (status == nri::Result::SUCCESS)
            {
                status = core.CreatePlacedBuffer(
                    session_->device(),
                    NriDeviceHeap,
                    {physical.index_capacity, 0, nri::BufferUsageBits::INDEX},
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

    RenderResult<DrawListResources*> RenderGraphInstance::prepareDrawList(
        const CompiledPass& pass,
        std::uint32_t slot,
        std::size_t vertex_bytes,
        std::size_t index_bytes
    ) noexcept
    {
        const auto* raster = plan_->rasterCommands(pass);
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
        physical.draws.parameters.resize(pass.parameter_size);
        auto vertices =
            grow(physical.vertices, physical.vertex_capacity, vertex_bytes, nri::BufferUsageBits::VERTEX);
        if (!vertices)
        {
            return cxx::unexpected(vertices.error());
        }
        setResource(vertex.resource.index, slot, {physical.vertices, 0, physical.vertex_capacity});
        auto indices = grow(physical.indices, physical.index_capacity, index_bytes, nri::BufferUsageBits::INDEX);
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
            for (const auto& write : list.upload_writes)
            {
                auto staged = uploads.stageBuffer(write);
                if (!staged)
                {
                    return cxx::unexpected(staged.error());
                }
            }
        }
        return true;
    }

    void RenderGraphInstance::finishDrawListUploads(std::uint32_t slot, RenderResult<void> accepted) noexcept
    {
        for (auto& list : frames_[slot].draw_lists)
        {
            list.upload_writes.clear();
            if (!accepted)
            {
                list.draws.packets.clear();
                list.draws.reads.clear();
            }
        }
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
                const auto& program = plan_->descriptor_plan_->passes[pass];
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
                    plan_->descriptor_plan_->passes[pass.compiled_pass->binding_slot].uses[replacement.use.index];
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
                    const auto description = bufferDescription(resource);
                    if (!description)
                    {
                        return cxx::unexpected(description.error());
                    }
                    created = core.CreateBuffer(session.device(), *description, binding.buffer);
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

    RenderResult<nri::BufferDesc>
    RenderGraphInstance::bufferDescription(const CompiledResource& resource) const noexcept
    {
        const auto& limits = session_->description();
        auto bytes = std::get<GraphBufferDesc>(resource.description).size;
        const bool needs_padding = limits.graphicsAPI == nri::GraphicsAPI::D3D12 &&
            (resource.buffer_usage & nri::BufferUsageBits::CONSTANT) != 0;
        if (needs_padding)
        {
            const auto alignment = limits.memoryAlignment.constantBufferOffset;
            if (alignment == 0) return cxx::unexpected(RenderError{kDeviceUnsupported});
            const auto padding = (alignment - bytes % alignment) % alignment;
            if (bytes > std::numeric_limits<std::uint64_t>::max() - padding)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            // Descriptor padding is part of the native allocation, outside the graph's logical buffer range.
            bytes += padding;
        }
        return nri::BufferDesc{bytes, 4, resource.buffer_usage};
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
            const auto description = bufferDescription(resource);
            if (!description)
            {
                return cxx::unexpected(description.error());
            }
            const auto status = core.CreatePlacedBuffer(session.device(), NriDeviceHeap, *description, binding.buffer);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
            binding.buffer_size = size;
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
            for (const auto& batch : frame.command_batches)
                command_count += std::ranges::count_if(
                    batch.commands, [](auto* command) noexcept { return command != nullptr; }
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
                    output += std::to_string(slot.last_use.serials()[q]);
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

    std::vector<TargetId> RenderGraphInstance::sampledTargets() const noexcept
    {
        std::vector<TargetId> result;
        for (const auto& item : data_)
            for (auto target : item.value->sampledTargets())
                if (std::ranges::find(result, target) == result.end()) result.push_back(target);
        return result;
    }
} // namespace lux::render::detail
