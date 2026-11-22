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
#include <lux/engine/render/graph/execute/GraphBindingPlan.hpp>

namespace lux::render::detail
{
    ResourceInput RenderGraphInstance::resourceInput(GraphResourceId resource, std::uint32_t slot) const noexcept
    {
        const auto& input = frames_[slot].bindings[resource.index];
        return {
            input.buffer,
            input.buffer_offset,
            input.buffer_size,
            input.image,
            input.addressed_buffers,
            input.addressed_revision,
            input.image_uses
        };
    }

    bool RenderGraphInstance::requiresProducerPrograms() const noexcept
    {
        return std::ranges::any_of(program_producers_, [&](auto index) noexcept
        {
            const auto& producer = producers_[index];
            return !producer.ready_programs || !rasterProgramMatches(producer, producer.ready_programs->revisions);
        });
    }

    void RenderGraphInstance::captureProgramRevisions(std::vector<std::uint64_t>& revisions) const noexcept
    {
        revisions.clear();
        for (auto index : program_producers_)
        {
            const auto& producer = producers_[index];
            for (auto slot : producer.program_sources) revisions.push_back(programStorageRevision(slot));
        }
    }

    bool RenderGraphInstance::matchesProgramRevisions(std::span<const std::uint64_t> revisions) const noexcept
    {
        for (auto index : program_producers_)
        {
            const auto& producer = producers_[index];
            const auto count = producer.program_sources.size();
            if (revisions.size() < count || !rasterProgramMatches(producer, revisions.first(count))) return false;
            revisions = revisions.subspan(count);
        }
        return revisions.empty();
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

    void RenderGraphInstance::initializeRasterStreams(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        for (const auto& pass : plan_->passes())
        {
            const auto* raster = plan_->rasterCommands(pass);
            const auto* stream = raster ? std::get_if<RasterListCommands>(&raster->draw) : nullptr;
            if (!stream) continue;
            if (stream->producer_stream && frame.stream_inputs.empty())
                frame.stream_inputs.resize(plan_->passes().size());
            frame.raster_draws.emplace_back();
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
        const auto& core = session_->nriCore();
        auto& binding = frame.bindings[resource.index];
        const auto& compiled = plan_->resources()[resource.index];
        if (layers > 1)
        {
            for (const auto [pass, index] : compiled.descriptor_uses)
            {
                const auto& program = plan_->binding_plan_->pass(pass);
                if (!program.bindings[program.uses[index].field].image_arrayed)
                    return cxx::unexpected(RenderError{kGraphInvalidRange, {resource.index, layers}});
            }
        }
        auto description = *binding.image_description;
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
            ImageViewBinding use;
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
        const auto add_view = [&](ImageViewBinding use, nri::TextureView kind, std::uint32_t count) -> RenderResult<void>
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
            view.planes = static_cast<nri::PlaneBits>(use.range.aspects);
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
                if (auto created = add_view(use, kind, layers); !created)
                {
                    return cxx::unexpected(created.error());
                }
            }
            for (const auto [pass, index] : compiled.descriptor_uses)
            {
                const auto& program = plan_->binding_plan_->pass(pass);
                const auto& use = program.uses[index];
                const bool arrayed = program.bindings[use.field].image_arrayed;
                const auto kind = use.kind == EDescriptorKind::STORAGE_IMAGE
                    ? (arrayed ? nri::TextureView::STORAGE_TEXTURE_ARRAY : nri::TextureView::STORAGE_TEXTURE)
                    : (arrayed ? nri::TextureView::TEXTURE_ARRAY : nri::TextureView::TEXTURE);
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
        // The acquired FIF slot has no GPU reader. Adopt all views before releasing the old image.
        invalidateResourceRecordings(slot, resource.index);
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
                    plan_->binding_plan_->pass(pass.compiled_pass->binding_slot).uses[replacement.use.index];
                const nri::UpdateDescriptorRangeDesc
                    update{pass.private_descriptors, use.binding, use.element, destination, 1};
                core.UpdateDescriptorRanges(&update, 1);
                pass.history_views[replacement.use.index] = nullptr;
                const auto history = plan_->resources()[use.resource].history;
                if (history != kGraphInvalidIndex) frame.pending_history_views.mark(history);
            }
            else
            {
                *std::ranges::find(frame.native.views, previous) = replacement.descriptor;
            }
            core.DestroyDescriptor(previous);
        }
        for (auto* previous : selected->views)
        {
            std::erase(frame.native.views, previous);
            core.DestroyDescriptor(previous);
        }
        selected->views = std::move(layer_views);
        frame.native.views.insert(frame.native.views.end(), selected->views.begin(), selected->views.end());
        if (replace_image)
        {
            std::vector<nri::Descriptor*> stream_views;
            collectRasterViews(frame, nullptr, binding.image, stream_views);
            for (auto* view : stream_views) core.DestroyDescriptor(view);
            *std::ranges::find(frame.native.images, binding.image) = image;
            auto* old_image = binding.image;
            binding.select(nullptr, image, core);
            core.DestroyTexture(old_image);
            // Runtime-layer uses are whole-resource. Each compiled transition spans all nominal layers.
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
                binding.select(physical.buffer, physical.image, core);
                binding.buffer_size = physical.buffer_size;
                continue;
            }
            if (resource.origin == EGraphOrigin::FRAME_BUFFER) continue;
            if (resource.origin == EGraphOrigin::IMPORTED)
            {
                if (std::holds_alternative<GraphImageDesc>(resource.description))
                {
                    if (!binding.image)
                    {
                        return cxx::unexpected(RenderError{kDeviceArgument});
                    }
                    binding.select(binding.buffer, binding.image, core);
                    if (auto valid = qualifyImportedImage({i}, *binding.image, target()); !valid) return valid;
                }
                continue;
            }
            if (i == plan.output())
            {
                binding.select(nullptr, output_.frames[slot].image, core);
                continue;
            }
            if (const auto* image = std::get_if<GraphImageDesc>(&resource.description); image && image->runtime_layers)
            {
                if (auto created = createResource(i, binding); !created)
                {
                    return created;
                }
                frame.native.images.push_back(binding.image);
                frame.layered_images.push_back({GraphResourceId{i}});
                continue;
            }
            const auto physical = plan.physical_resources_.resource_slots[i];
            binding = frame.bindings[plan.physical_resources_.slots[physical.index].resource.index];
        }
        for (auto& binding : frame.bindings) binding.select(binding.buffer, binding.image, core);
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
                frame.native.images.push_back(binding.image);
                if (alias_memory)
                {
                    core.GetTextureMemoryDesc(*binding.image, nri::MemoryLocation::DEVICE, requirements[s]);
                }
            }
            else
            {
                frame.native.buffers.push_back(binding.buffer);
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
        const auto placements = instantiatePlacements(
            physical, requirements, session.description().graphicsAPI
        );
        std::vector<nri::AllocateMemoryDesc> descriptions(physical.slots.size());
        for (std::uint32_t s = 0; s < placements.size(); ++s)
        {
            auto& description = descriptions[placements[s].root.index];
            const auto& required = requirements[s];
            description.size = std::max(description.size, required.size);
            description.type = required.type;
            description.vma.enable = !required.mustBeDedicated;
            description.vma.alignment = std::max(description.vma.alignment, required.alignment);
            const auto* image =
                std::get_if<GraphImageDesc>(&plan_->resources()[physical.slots[s].resource.index].description);
            description.allowMultisampleTextures |= image && image->samples > 1;
        }
        std::vector<MemoryGroupId> root_to_group(placements.size());
        for (std::uint32_t s = 0; s < placements.size(); ++s)
        {
            if (placements[s].root.index != s)
            {
                continue;
            }
            nri::Memory* memory = nullptr;
            const auto allocated = core.AllocateMemory(session.device(), descriptions[s], memory);
            if (allocated != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(allocated));
            }
            root_to_group[s] = {static_cast<std::uint32_t>(frame.native.memories.size())};
            frame.native.memories.push_back({memory, descriptions[s].size});
        }
        std::vector<nri::BindBufferMemoryDesc> buffers;
        std::vector<nri::BindTextureMemoryDesc> images;
        frame.memory_bindings.resize(placements.size());
        for (std::uint32_t s = 0; s < placements.size(); ++s)
        {
            const auto& placement = placements[s];
            const auto group = root_to_group[placement.root.index];
            frame.memory_bindings[s] = {group, placement.reason};
            auto* memory = frame.native.memories[group.index].memory;
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
        if (frame.memory_bindings.empty()) return;
        const auto barriers = instantiateAliasBarriers(
            plan_->physical_resources_, frame.memory_bindings, frame.passes.size()
        );
        for (std::size_t p = 0; p < barriers.size(); ++p) frame.passes[p].alias_barrier = barriers[p];
    }

    RenderResult<nri::BufferDesc>
    RenderGraphInstance::bufferDescription(const CompiledResource& resource) const noexcept
    {
        return bufferDescription(resource, std::get<GraphBufferDesc>(resource.description).size);
    }

    RenderResult<nri::BufferDesc>
    RenderGraphInstance::bufferDescription(const CompiledResource& resource, std::uint64_t bytes) const noexcept
    {
        const auto& limits = session_->description();
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

    RenderResult<void> RenderGraphInstance::qualifyImportedImage(
        GraphResourceId id, const nri::Texture& image, const TargetDesc& target
    ) const noexcept
    {
        const auto& resource = plan_->resources()[id.index];
        const auto* expected = std::get_if<GraphImageDesc>(&resource.description);
        if (!expected) return cxx::unexpected(RenderError{kStorageMismatch});
        const auto& actual = session_->nriCore().GetTextureDesc(image);
        auto width = expected->width, height = expected->height;
        if (resource.target_sized)
        {
            const auto resolved = textureDescription(resource, target);
            if (!resolved) return cxx::unexpected(resolved.error());
            width = resolved->width;
            height = resolved->height;
        }
        const bool wrong_extent = expected->extent.source != EImageExtentSource::IMPORTED &&
            (actual.width != width || actual.height != height);
        const bool wrong_shape = actual.type != nri::TextureType::TEXTURE_2D || actual.depth != 1 ||
            actual.mipNum != expected->mips || actual.layerNum != expected->layers ||
            actual.sampleNum != expected->samples;
        const bool wrong_format = actual.format != imageFormat(expected->format).native;
        const bool wrong_sharing = resource.concurrent_image_read && actual.sharingMode != nri::SharingMode::CONCURRENT;
        const bool missing_usage = (actual.usage & resource.image_usage) != static_cast<unsigned>(resource.image_usage);
        if (wrong_extent || wrong_shape || wrong_format || missing_usage || wrong_sharing)
            return cxx::unexpected(RenderError{kDeviceArgument});
        return {};
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
            std::size_t prepared_buffers = 0;
            for (const auto& prepared : frame.native.prepared_buffers)
            {
                if (!prepared.buffer) continue;
                nri::MemoryDesc memory{};
                session_->nriCore().GetBufferMemoryDesc(*prepared.buffer, nri::MemoryLocation::DEVICE, memory);
                bytes += memory.size;
                ++prepared_buffers;
            }
            for (auto* buffer : frame.native.buffers)
            {
                nri::MemoryDesc memory{};
                session_->nriCore().GetBufferMemoryDesc(*buffer, nri::MemoryLocation::DEVICE, memory);
                bytes += memory.size;
            }
            for (auto* image : frame.native.images)
            {
                nri::MemoryDesc memory{};
                session_->nriCore().GetTextureMemoryDesc(*image, nri::MemoryLocation::DEVICE, memory);
                bytes += memory.size;
            }
            const auto allocated = std::accumulate(
                frame.native.memories.begin(),
                frame.native.memories.end(),
                std::uint64_t{0},
                [](auto sum, const auto& allocation) { return sum + allocation.bytes; }
            );
            std::size_t command_count = 0;
            for (const auto& batch : frame.native.batches)
                command_count += std::ranges::count_if(
                    batch.commands, [](auto* command) noexcept { return command != nullptr; }
                );
            output += "{\"slot\":" + std::to_string(i) +
                      ",\"imported_revision\":" + std::to_string(frame.imported_revision) +
                      ",\"owned_command_buffers\":" + std::to_string(command_count) +
                      ",\"owned_buffers\":" +
                          std::to_string(frame.native.buffers.size() + prepared_buffers) +
                      ",\"owned_textures\":" + std::to_string(frame.native.images.size()) +
                      ",\"allocation_requirement_bytes\":" + std::to_string(bytes) +
                      ",\"explicit_memory_count\":" + std::to_string(frame.native.memories.size()) +
                      ",\"explicit_memory_bytes\":" + std::to_string(allocated) + ",\"memory_bindings\":[";
            constexpr std::array
                reasons{
                    "excluded", "dedicated", "rendering_scope", "distinct_requirements", "incompatible_layout",
                    "unordered", "aliased"
                };
            for (std::size_t s = 0; s < frame.memory_bindings.size(); ++s)
            {
                if (s)
                {
                    output += ',';
                }
                const auto& binding = frame.memory_bindings[s];
                output += "{\"allocation\":" + std::to_string(binding.group.index) + ",\"reason\":\"" +
                          reasons[static_cast<std::size_t>(binding.reason)] + "\"}";
            }
            output += "],\"alias_barriers\":[";
            bool first_barrier = true;
            for (std::size_t p = 0; p < frame.passes.size(); ++p)
            {
                const auto& barrier = frame.passes[p].alias_barrier;
                if (!barrier) continue;
                if (!first_barrier) output += ',';
                first_barrier = false;
                output += "{\"pass\":" + std::to_string(p) +
                    ",\"before_access\":" + std::to_string(static_cast<std::uint32_t>(barrier->before.access)) +
                    ",\"before_stages\":" + std::to_string(static_cast<std::uint32_t>(barrier->before.stages)) +
                    ",\"after_access\":" + std::to_string(static_cast<std::uint32_t>(barrier->after.access)) +
                    ",\"after_stages\":" + std::to_string(static_cast<std::uint32_t>(barrier->after.stages)) + "}";
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
                output += "{\"sample\":" + std::to_string(history.sample_sequences[s]) +
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

    void RenderGraphInstance::collectRasterViews(
        FrameResources& frame, nri::Buffer* buffer, nri::Texture* image, std::vector<nri::Descriptor*>& views
    ) noexcept
    {
        if (!buffer && !image) return;
        if (image)
        {
            std::erase_if(frame.native.raster_attachment_views, [&](const auto& attachment) noexcept
            {
                if (attachment.image != image) return false;
                views.push_back(attachment.view);
                return true;
            });
        }
        for (auto& record : frame.native.raster_bindings)
        {
            for (std::size_t i = 0; i < record.inputs.size(); ++i)
            {
                const auto& input = record.inputs[i];
                const bool matches = (buffer && input.buffer == buffer) || (image && input.image == image);
                if (!matches) continue;
                if (auto* view = std::exchange(record.views[i], nullptr)) views.push_back(view);
                record.inputs[i] = {};
            }
        }
    }

    void RenderGraphInstance::retireRasterViews(FrameResources& frame, nri::Buffer* buffer, nri::Texture* image) noexcept
    {
        std::vector<nri::Descriptor*> views;
        collectRasterViews(frame, buffer, image, views);
        if (!views.empty())
        {
            session_->deferRelease([core = &session_->nriCore(), views = std::move(views)]() noexcept
            {
                for (auto* view : views) core->DestroyDescriptor(view);
            });
        }
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
            binding.image_uses = value.image_uses;
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
            // A pool rebuild must replay only views whose owners still hold the allocation.
            retireRasterViews(frame, binding.buffer != value.buffer ? binding.buffer : nullptr,
                binding.image != value.image ? binding.image : nullptr);
            binding.select(value.buffer, value.image, session_->nriCore());
            binding.buffer_offset = value.offset;
            binding.buffer_size = value.size;
            if (changed_physical) invalidateResourceRecordings(current, resource);
            if (changed_physical && !binding.dirty)
            {
                binding.dirty = true;
                frame.changed_resources.push_back(resource);
            }
        }
    }
} // namespace lux::render::detail
