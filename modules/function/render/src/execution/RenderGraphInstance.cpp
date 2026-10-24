#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/device/DescriptorTable.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <lux/engine/render/execution/FrameExecution.hpp>
#include <lux/engine/render/execution/FrameResources.hpp>
#include <lux/engine/render/execution/PassExtent.hpp>
#include <lux/engine/render/execution/RenderGraphInstance.hpp>
#include <lux/engine/render/graph/PhysicalResourcePlan.hpp>
#include <lux/engine/render/graph/PipelineLayoutPlan.hpp>
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
            input.parameter_revision
        };
    }

    RenderGraphInstance::RenderGraphInstance(
        DeviceSession& session,
        std::uint32_t frame_count,
        const RenderPlan& plan,
        TargetOutput output,
        std::uint32_t parameter_bytes
    ) noexcept
        : session_(&session), plan_(&plan), target_(output.frames.empty() ? plan.target() : output.description),
          output_(output)
    {
        instance_values_.resize(std::max(plan.descriptor_plan_->instance_parameters.size, parameter_bytes));
        condition_values_.resize(plan.condition_count_);
        parameter_copies_.resize(plan.passes().size());
        parameter_uses_.resize(plan.resources().size());
        if (!plan.passes().empty())
        {
            const auto& last = plan.passes().back();
            parameter_defaults_.resize(last.parameter_offset + last.parameter_size);
        }
        for (const auto& pass : plan.passes())
        {
            const auto& initial = plan.descriptor_plan_->passes[pass.source_pass].initial_parameters;
            std::ranges::copy(initial, parameter_defaults_.begin() + pass.parameter_offset);
        }
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
            frame.parameters = parameter_defaults_;
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

    RenderResult<void> RenderGraphInstance::bindSampler(
        std::uint32_t input,
        nri::Descriptor& sampler,
        bool comparison
    ) noexcept
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
            !output_.frames.empty() && (!plan.compatible(target_) || output_.frames.size() != frames_.size());
        const bool missing_target = plan.output() != kGraphInvalidIndex && output_.frames.empty();
        if (wrong_device || wrong_target || missing_target)
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
            physical.parameters.resize(pass.parameter_size);
            setResource(vertex.resource.index, slot, {physical.vertices, 0, physical.vertex_capacity});
            setResource(list->index_resource.resource.index, slot, {physical.indices, 0, physical.index_capacity});
        }
        return {};
    }

    ResourceUploader::Task RenderGraphInstance::updateDrawList(
        GraphPassId id,
        DrawListData input,
        FrameExecution& execution,
        ResourceUploader& uploads
    ) noexcept
    {
        const auto index = id.index < plan_->pass_indices_.size() ? plan_->pass_indices_[id.index] : kGraphInvalidIndex;
        if (index == kGraphInvalidIndex)
        {
            co_return cxx::unexpected(RenderError{kGraphUnknownPass});
        }
        const auto& pass = plan_->passes()[index];
        const auto* raster = std::get_if<RasterCommands>(&pass.work);
        const auto* list = raster ? std::get_if<IndexedListCommands>(&raster->draw) : nullptr;
        if (!list)
        {
            co_return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const auto vertex = raster->vertices.front();
        const auto index_bytes = list->type == EIndexType::UINT16 ? 2u : 4u;
        const bool invalid_data =
            input.vertices.size() % vertex.stride != 0 || input.indices.size() % index_bytes != 0 ||
            input.parameters.size() != pass.parameter_size || input.vertices.size() > SIZE_MAX - input.indices.size();
        if (invalid_data)
        {
            co_return cxx::unexpected(RenderError{kDeviceArgument});
        }
        const auto indices = input.indices.size() / index_bytes;
        for (const auto& draw : input.draws)
        {
            const bool invalid_range = draw.first_index > indices || draw.index_count > indices - draw.first_index;
            const bool invalid_clip = draw.x > target_.width || draw.y > target_.height ||
                                      draw.width > target_.width - draw.x || draw.height > target_.height - draw.y;
            if (invalid_range || invalid_clip || draw.selection >= list->selection_count)
            {
                co_return cxx::unexpected(RenderError{kDeviceArgument});
            }
        }
        std::uint32_t slot = 0;
        while (!execution.tryBeginWrite(slot))
        {
            auto ready = co_await execution.whenWritable(input.vertices.size() + input.indices.size());
            if (!ready)
            {
                co_return cxx::unexpected(ready.error());
            }
        }
        SubmissionFrontiers progress;
        cxx::scope_exit finish([&]() noexcept { execution.finishWrite(slot, progress); });
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
        // The selected slice is reusable. If an accepted copy later fails, no old draw range may
        // consume partially replaced bytes; the failed complete frame requests a fresh Client snapshot.
        physical.ranges.clear();
        if (auto grown = grow(
                physical.vertices,
                physical.vertex_capacity,
                input.vertices.size(),
                nri::BufferUsageBits::VERTEX_BUFFER
            );
            !grown)
        {
            co_return cxx::unexpected(grown.error());
        }
        setResource(vertex.resource.index, slot, {physical.vertices, 0, physical.vertex_capacity});
        if (auto grown = grow(
                physical.indices,
                physical.index_capacity,
                input.indices.size(),
                nri::BufferUsageBits::INDEX_BUFFER
            );
            !grown)
        {
            co_return cxx::unexpected(grown.error());
        }
        setResource(list->index_resource.resource.index, slot, {physical.indices, 0, physical.index_capacity});
        // Buffer copies require four-byte sizes. Keep the original stream borrowed; only the
        // final 1-3 bytes need local padding (common for an odd count of uint16 UI indices).
        // These coroutine-local tails stay alive through the actual uploader completion.
        std::array<std::array<std::byte, 4>, 2> tails{};
        std::array<BufferWrite, 4> writes;
        std::size_t count = 0;
        const auto append = [&](nri::Buffer* buffer, std::span<const std::byte> source, auto& tail) noexcept
        {
            const auto aligned = source.size() & ~std::size_t{3};
            if (aligned != 0)
            {
                writes[count++] = {buffer, 0, source.first(aligned)};
            }
            if (aligned != source.size())
            {
                std::memcpy(tail.data(), source.data() + aligned, source.size() - aligned);
                writes[count++] = {buffer, aligned, tail};
            }
        };
        append(physical.vertices, input.vertices, tails[0]);
        append(physical.indices, input.indices, tails[1]);
        if (count != 0)
        {
            auto copied = co_await uploads.writeBuffers(std::span(writes).first(count), {}, slot, &progress);
            if (!copied)
            {
                co_return cxx::unexpected(copied.error());
            }
        }
        physical.ranges.assign(input.draws.begin(), input.draws.end());
        std::ranges::copy(input.parameters, physical.parameters.begin());
        co_return RenderResult<void>{};
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
                    const bool extent_mismatch = image->extent.source != EImageExtentSource::IMPORTED &&
                                                 (actual.width != image->width || actual.height != image->height);
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

    RenderResult<nri::TextureDesc> RenderGraphInstance::textureDescription(const CompiledResource& resource
    ) const noexcept
    {
        return textureDescription(resource, target_);
    }

    RenderResult<nri::TextureDesc> RenderGraphInstance::textureDescription(
        const CompiledResource& resource,
        const TargetDesc& target
    ) const noexcept
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
        if (!width || !height || width > max_extent || height > max_extent)
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
        return createResource(index, binding, target_);
    }

    RenderResult<void> RenderGraphInstance::createResource(
        std::uint32_t index,
        Binding& binding,
        const TargetDesc& target
    ) noexcept
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
        const bool produced = std::ranges::all_of(
            histories_,
            [&](const auto& history)
            {
                return std::ranges::all_of(
                    history.plan->writers,
                    [&](auto writer)
                    {
                        const auto region = plan_->passes()[writer].condition_region;
                        return region == kGraphInvalidIndex || frame.enabled_regions[region] != 0;
                    }
                );
            }
        );
        if (!produced)
        {
            return;
        }
        for (auto& history : histories_)
        {
            history.publish();
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
                parameter_uses_[input.resource.index].push_back(p);
            }
            if (std::holds_alternative<TransferWork>(compiled_pass.work))
            {
                continue;
            }
            for (const auto& mapping : layout.passes[compiled_pass.source_pass].parameters)
            {
                const auto& source = mapping.resource == kGraphInvalidIndex
                                         ? ResourceInput{nullptr, 0, 0, instance_values_, &layout.instance_parameters}
                                         : ResourceInput{
                                               nullptr,
                                               0,
                                               0,
                                               frames_[0].bindings[mapping.resource].parameters,
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
                    {mapping.resource, field->offset, destination.offset, std::uint32_t(bytes)}
                );
                auto& consumers = mapping.resource == kGraphInvalidIndex ? instance_parameter_uses_
                                                                         : parameter_uses_[mapping.resource];
                consumers.push_back(p);
            }
        }
        const auto deduplicate = [](auto& indices)
        {
            std::ranges::sort(indices);
            indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        };
        deduplicate(instance_parameter_uses_);
        for (std::uint32_t resource = 0; resource < parameter_uses_.size(); ++resource)
        {
            auto& consumers = parameter_uses_[resource];
            deduplicate(consumers);
            if (!consumers.empty())
            {
                parameter_resources_.push_back(resource);
            }
        }
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            for (std::uint32_t pass = 0; pass < plan_->passes().size(); ++pass)
            {
                const auto* raster = std::get_if<RasterCommands>(&plan_->passes()[pass].work);
                if (!parameter_copies_[pass].empty() || (raster && raster->state_input))
                {
                    markParameters(pass, slot);
                }
            }
        }
        nri::DescriptorPoolDesc pool{};
        for (const auto& pass : plan_->passes())
        {
            const auto& program = layout.passes[pass.source_pass];
            if (!program.private_bindings.empty())
            {
                ++pool.descriptorSetMaxNum;
            }
            for (const auto& binding : program.private_bindings)
            {
                switch (binding.kind)
                {
                case EDescriptorKind::TYPED_STORAGE_BUFFER:
                    pool.storageBufferMaxNum += binding.count;
                    break;
                case EDescriptorKind::STORAGE_BUFFER:
                    pool.storageStructuredBufferMaxNum += binding.count;
                    break;
                case EDescriptorKind::UNIFORM_BUFFER:
                    pool.constantBufferMaxNum += binding.count;
                    break;
                case EDescriptorKind::STORAGE_IMAGE:
                    pool.storageTextureMaxNum += binding.count;
                    break;
                case EDescriptorKind::SAMPLED_IMAGE:
                    pool.textureMaxNum += binding.count;
                    break;
                case EDescriptorKind::SAMPLER:
                    pool.samplerMaxNum += binding.count;
                    break;
                }
            }
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
            for (auto& pass : frame.passes)
            {
                const auto& program = layout.passes[pass.compiled_pass->source_pass];
                pass.descriptors.resize(unsigned(program.shared) + unsigned(!program.private_bindings.empty()));
                pass.private_views.resize(program.uses.size());
                pass.history_views.resize(program.uses.size());
                if (program.shared)
                {
                    pass.descriptors[0] = session_->descriptors().descriptors();
                }
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
                    pass.descriptors[program.private_set] = pass.private_descriptors;
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
            output += "{\"slot\":" + std::to_string(i) +
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

    void RenderGraphInstance::setResource(
        std::uint32_t resource,
        std::uint32_t slot,
        const ResourceInput& value
    ) noexcept
    {
        const auto first = slot == UINT32_MAX ? 0 : slot;
        const auto end = slot == UINT32_MAX ? static_cast<std::uint32_t>(frames_.size()) : slot + 1;
        for (auto current = first; current < end; ++current)
        {
            auto& frame = frames_[current];
            auto& binding = frame.bindings[resource];
            const bool changed_physical = binding.buffer != value.buffer || binding.buffer_offset != value.offset ||
                                          binding.buffer_size != value.size || binding.image != value.image;
            if (changed_physical && plan_->resources()[resource].origin == EGraphOrigin::IMPORTED)
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
                for (auto pass : parameter_uses_[resource])
                {
                    markParameters(pass, current);
                }
            }
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
            }
            source.dirty = false;
        }
        frame.changed_resources.clear();
        return {};
    }

    RenderResult<void> RenderGraphInstance::setInstanceParameters(std::span<const std::byte> bytes) noexcept
    {
        if (bytes.size() != instance_values_.size())
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        std::ranges::copy(bytes, instance_values_.begin());
        markInstanceParameters();
        return {};
    }

    RenderResult<void> RenderGraphInstance::setInputs(
        std::span<const std::byte> parameters,
        std::span<const std::uint8_t> conditions
    ) noexcept
    {
        if (parameters.size() != instance_values_.size())
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        if (auto accepted = setConditions(conditions); !accepted)
        {
            return accepted;
        }
        std::ranges::copy(parameters, instance_values_.begin());
        markInstanceParameters();
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

    RenderResult<void> RenderGraphInstance::setParameters(GraphPassId id, std::span<const std::byte> bytes) noexcept
    {
        const auto index = id.index < plan_->pass_indices_.size() ? plan_->pass_indices_[id.index] : kGraphInvalidIndex;
        if (index == kGraphInvalidIndex || bytes.size() != plan_->passes()[index].parameter_size)
        {
            return cxx::unexpected(RenderError{kDeviceArgument});
        }
        std::ranges::copy(bytes, parameter_defaults_.begin() + plan_->passes()[index].parameter_offset);
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            markParameters(index, slot, true);
        }
        return {};
    }

    std::span<const std::byte> RenderGraphInstance::parameters(GraphPassId id) const noexcept
    {
        const auto index = id.index < plan_->pass_indices_.size() ? plan_->pass_indices_[id.index] : kGraphInvalidIndex;
        return index == kGraphInvalidIndex
                   ? std::span<const std::byte>{}
                   : std::span(parameter_defaults_)
                         .subspan(plan_->passes()[index].parameter_offset, plan_->passes()[index].parameter_size);
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
            frame.enabled_regions[r] = value == condition.expected;
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
        frame.viewport = viewport_;
        if (auto parameters = prepareParameters(slot); !parameters)
        {
            return cxx::unexpected(parameters.error());
        }
        return true;
    }

    void RenderGraphInstance::markParameters(std::uint32_t pass, std::uint32_t slot, bool defaults) noexcept
    {
        auto& frame = frames_[slot];
        if (frame.parameter_dirty[pass] == 0)
        {
            frame.parameter_updates.push_back(pass);
        }
        frame.parameter_dirty[pass] |= defaults ? 3 : 2;
    }

    void RenderGraphInstance::markInstanceParameters() noexcept
    {
        for (std::uint32_t slot = 0; slot < frames_.size(); ++slot)
        {
            for (auto pass : instance_parameter_uses_)
            {
                markParameters(pass, slot);
            }
        }
    }

    RenderResult<void> RenderGraphInstance::prepareParameters(std::uint32_t slot) noexcept
    {
        auto& frame = frames_[slot];
        // Unversioned external borrows retain their prior live semantics. Versioned inputs and
        // owned instance defaults are copied only into the FIF slices they actually invalidated.
        for (auto resource : parameter_resources_)
        {
            if (frame.bindings[resource].parameter_revision == 0)
            {
                for (auto pass : parameter_uses_[resource])
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
                std::memcpy(
                    &state,
                    frame.bindings[input.resource.index].parameters.data() + input.offset,
                    sizeof(state)
                );
                const bool invalid_bias =
                    !std::isfinite(state.depth_bias_constant) || !std::isfinite(state.depth_bias_slope);
                if (invalid_bias || !state.viewport.validate())
                {
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                }
            }
            auto& parameters = frame.passes[p].parameters;
            const auto& compiled = plan_->passes()[p];
            if ((frame.parameter_dirty[p] & 1) != 0)
            {
                std::ranges::copy(
                    std::span(parameter_defaults_).subspan(compiled.parameter_offset, compiled.parameter_size),
                    parameters.begin()
                );
            }
            for (const auto& copy : parameter_copies_[p])
            {
                const auto* source = copy.resource == kGraphInvalidIndex
                                         ? instance_values_.data()
                                         : frame.bindings[copy.resource].parameters.data();
                std::memcpy(parameters.data() + copy.destination_offset, source + copy.source_offset, copy.bytes);
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
