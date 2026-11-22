#include <lux/engine/render/graph/execute/PreparedBufferUpload.hpp>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/device/BufferAccess.hpp>
#include <lux/engine/render/device/CompiledPrograms.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/device/JobGpuResources.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/graph/execute/PhysicalFrameAllocation.hpp>
#include <lux/engine/render/device/ImageFormat.hpp>
#include <algorithm>
#include <bit>
#include <limits>
#include <utility>

namespace lux::render::detail
{
    nri::Result createGraphBufferView(DeviceSession& session, nri::BufferViewDesc view, nri::Descriptor*& descriptor) noexcept
    {
        const auto& core = session.nriCore();
        const auto& limits = session.description();
        if (view.type == nri::BufferView::CONSTANT_BUFFER)
        {
            const auto alignment = limits.memoryAlignment.constantBufferOffset;
            const auto max_range = limits.memory.constantBufferMaxRange;
            const bool invalid_offset = alignment == 0 || view.offset % alignment != 0;
            const bool invalid_size = view.size == 0 || view.size > max_range;
            if (invalid_offset || invalid_size) return nri::Result::INVALID_ARGUMENT;
            if (limits.graphicsAPI == nri::GraphicsAPI::D3D12)
            {
                const auto padding = (alignment - view.size % alignment) % alignment;
                if (padding > max_range - view.size) return nri::Result::INVALID_ARGUMENT;
                view.size += padding;
            }
            const auto capacity = core.GetBufferDesc(*view.buffer).size;
            const bool outside_buffer = view.offset > capacity || view.size > capacity - view.offset;
            if (outside_buffer) return nri::Result::INVALID_ARGUMENT;
        }
        return core.CreateBufferView(view, descriptor);
    }

    RenderResult<nri::BufferDesc>
    graphBufferDescription(DeviceSession& session, const CompiledResource& resource, std::uint64_t bytes) noexcept
    {
        const auto& limits = session.description();
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
    graphTextureDescription(DeviceSession& session, const CompiledResource& resource, const TargetDesc& target) noexcept
    {
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

    RenderResult<FrameResourceBinding> createGraphResource(
        DeviceSession& session, const CompiledResource& resource, const TargetDesc& target, ETransientAllocation mode
    ) noexcept
    {
        FrameResourceBinding binding;
        const bool placed = mode != ETransientAllocation::ALIAS_MEMORY;
        const auto& core = session.nriCore();
        if (std::holds_alternative<GraphImageDesc>(resource.description))
        {
            auto description = graphTextureDescription(session, resource, target);
            if (!description)
            {
                return cxx::unexpected(description.error());
            }
            const auto status = placed
                ? core.CreatePlacedTexture(session.device(), NriDeviceHeap, *description, binding.image)
                : core.CreateTexture(session.device(), *description, binding.image);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
        }
        else
        {
            const auto size = std::get<GraphBufferDesc>(resource.description).size;
            const auto description = graphBufferDescription(session, resource, size);
            if (!description)
            {
                return cxx::unexpected(description.error());
            }
            const auto status = placed
                ? core.CreatePlacedBuffer(session.device(), NriDeviceHeap, *description, binding.buffer)
                : core.CreateBuffer(session.device(), *description, binding.buffer);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
            binding.buffer_size = size;
        }
        return binding;
    }

    namespace
    {
        // All handles created by this operation enter the destination native owner immediately.
        // Live bindings participate only in requirement queries and in the memory migration closure.
        class PhysicalAllocationOperation final
        {
        public:
            PhysicalAllocationOperation(
                JobFrameResources& owner, DeviceSession& session, const PhysicalFrameContract& contract
            ) noexcept
                : owner_(owner), session_(session), contract_(contract), plan_(contract.graph.physicalResources()),
                  objects_(plan_.slots.size())
            {
                changes_.replaced_slots.resize(plan_.slots.size());
            }

            RenderResult<PhysicalFrameChanges> prepare() noexcept
            {
                const bool explicit_memory = plan_.allocation == ETransientAllocation::ALIAS_MEMORY;
                if (explicit_memory) requirements_.resize(plan_.slots.size());
                for (std::uint32_t slot = 0; slot < plan_.slots.size(); ++slot)
                {
                    const auto index = plan_.slots[slot].resource.index;
                    const auto& resource = contract_.graph.resources()[index];
                    bool replace = !contract_.live;
                    if (contract_.live && resource.target_sized)
                    {
                        auto desc = graphTextureDescription(session_, resource, contract_.target);
                        if (!desc) return cxx::unexpected(desc.error());
                        const auto& live = session_.nriCore().GetTextureDesc(*contract_.live->bindings[index].image);
                        replace = live.width != desc->width || live.height != desc->height;
                    }
                    if (replace)
                    {
                        if (auto made = create(slot); !made) return cxx::unexpected(made.error());
                    }
                    if (explicit_memory)
                    {
                        const auto& selected = replace ? objects_[slot] : contract_.live->bindings[index];
                        requirements_[slot] = memoryRequirements(selected);
                    }
                }

                if (explicit_memory)
                {
                    auto placements = resolveGroups();
                    if (!placements) return cxx::unexpected(placements.error());
                    if (auto bound = bindGroups(*placements); !bound) return cxx::unexpected(bound.error());
                }
                for (std::uint32_t r = 0; r < plan_.resource_slots.size(); ++r)
                {
                    const auto slot = plan_.resource_slots[r];
                    if (slot.valid() && changes_.replaced_slots[slot.index])
                        changes_.resources.push_back({r, objects_[slot.index]});
                }
                return std::move(changes_);
            }

        private:
            RenderResult<void> create(std::uint32_t slot) noexcept
            {
                const auto& resource = contract_.graph.resources()[plan_.slots[slot].resource.index];
                auto object = createGraphResource(session_, resource, contract_.target, plan_.allocation);
                if (!object) return cxx::unexpected(object.error());
                if (object->image) owner_.images.push_back(object->image);
                else owner_.buffers.push_back(object->buffer);
                objects_[slot] = *object;
                changes_.replaced_slots[slot] = true;
                return {};
            }

            nri::MemoryDesc memoryRequirements(const FrameResourceBinding& binding) const noexcept
            {
                nri::MemoryDesc result{};
                const auto& core = session_.nriCore();
                if (binding.image) core.GetTextureMemoryDesc(*binding.image, nri::MemoryLocation::DEVICE, result);
                else core.GetBufferMemoryDesc(*binding.buffer, nri::MemoryLocation::DEVICE, result);
                return result;
            }

            RenderResult<std::vector<RootMemoryPlacement>> resolveGroups() noexcept
            {
                for (;;)
                {
                    auto placements = instantiatePlacements(plan_, requirements_, session_.description().graphicsAPI);
                    if (!contract_.live) return placements;
                    expandMemoryMigration(contract_.live->placements, placements, changes_.replaced_slots);
                    bool requirements_changed = false;
                    for (std::uint32_t s = 0; s < placements.size(); ++s)
                    {
                        if (!changes_.replaced_slots[s] || objects_[s].image || objects_[s].buffer) continue;
                        if (auto made = create(s); !made) return cxx::unexpected(made.error());
                        const auto actual = memoryRequirements(objects_[s]);
                        const auto previous = std::exchange(requirements_[s], actual);
                        const bool different_shape = actual.size != previous.size ||
                            actual.alignment != previous.alignment;
                        const bool different_heap = actual.type != previous.type ||
                            actual.mustBeDedicated != previous.mustBeDedicated;
                        requirements_changed |= different_shape || different_heap;
                    }
                    // Each retry has materialized at least one new slot. Use the actual native object's requirements.
                    if (!requirements_changed) return placements;
                }
            }

            RenderResult<void> bindGroups(std::span<const RootMemoryPlacement> placements) noexcept
            {
                std::vector<nri::AllocateMemoryDesc> allocations(placements.size());
                for (std::uint32_t s = 0; s < placements.size(); ++s)
                {
                    auto& allocation = allocations[placements[s].root.index];
                    const auto& required = requirements_[s];
                    allocation.size = std::max(allocation.size, required.size);
                    allocation.type = required.type;
                    allocation.vma.enable = !required.mustBeDedicated;
                    allocation.vma.alignment = std::max(allocation.vma.alignment, required.alignment);
                    const auto& resource = contract_.graph.resources()[plan_.slots[s].resource.index];
                    const auto* image = std::get_if<GraphImageDesc>(&resource.description);
                    allocation.allowMultisampleTextures |= image && image->samples > 1;
                }
                const auto& core = session_.nriCore();
                std::vector<MemoryGroupId> groups(placements.size());
                std::vector<nri::BindTextureMemoryDesc> images;
                std::vector<nri::BindBufferMemoryDesc> buffers;
                for (std::uint32_t s = 0; s < placements.size(); ++s)
                {
                    const auto root = placements[s].root.index;
                    if (s == root)
                    {
                        groups[root] = {static_cast<std::uint32_t>(changes_.memory_sources.size())};
                        if (changes_.replaced_slots[s])
                        {
                            nri::Memory* memory = nullptr;
                            const auto status = core.AllocateMemory(session_.device(), allocations[s], memory);
                            if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
                            const auto index = static_cast<std::uint32_t>(owner_.memories.size());
                            owner_.memories.push_back({memory, allocations[s].size});
                            changes_.memory_sources.push_back({EAllocationOwner::CANDIDATE, index});
                        }
                        else
                        {
                            const auto live = contract_.live->placements[s].group;
                            changes_.memory_sources.push_back({EAllocationOwner::LIVE, live.index});
                        }
                    }
                    const auto group = groups[root];
                    changes_.placements.push_back({group, placements[s].reason});
                    if (!changes_.replaced_slots[s]) continue;
                    auto* memory = owner_.memories[changes_.memory_sources[group.index].index].memory;
                    if (objects_[s].image) images.push_back({objects_[s].image, memory, 0});
                    else buffers.push_back({objects_[s].buffer, memory, 0});
                }
                if (!buffers.empty())
                {
                    const auto count = static_cast<std::uint32_t>(buffers.size());
                    const auto status = core.BindBufferMemory(buffers.data(), count);
                    if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
                }
                if (!images.empty())
                {
                    const auto count = static_cast<std::uint32_t>(images.size());
                    const auto status = core.BindTextureMemory(images.data(), count);
                    if (status != nri::Result::SUCCESS) return cxx::unexpected(nativeError(status));
                }
                return {};
            }

            JobFrameResources& owner_;
            DeviceSession& session_;
            const PhysicalFrameContract& contract_;
            const PhysicalResourcePlan& plan_;
            std::vector<FrameResourceBinding> objects_;
            std::vector<nri::MemoryDesc> requirements_;
            PhysicalFrameChanges changes_;
        };
    } // namespace

    RenderResult<PhysicalFrameChanges>
    JobFrameResources::preparePhysical(DeviceSession& session, const PhysicalFrameContract& contract) noexcept
    {
        const auto buffer_begin = buffers.size(), image_begin = images.size(), memory_begin = memories.size();
        cxx::scope_exit rollback([&]() noexcept
        {
            const auto& core = session.nriCore();
            for (auto* image : std::span{images}.subspan(image_begin)) core.DestroyTexture(image);
            for (auto* buffer : std::span{buffers}.subspan(buffer_begin)) core.DestroyBuffer(buffer);
            for (auto& allocation : std::span{memories}.subspan(memory_begin)) core.FreeMemory(allocation.memory);
            images.resize(image_begin);
            buffers.resize(buffer_begin);
            memories.resize(memory_begin);
        });
        auto changes = PhysicalAllocationOperation(*this, session, contract).prepare();
        if (changes) rollback.release();
        return changes;
    }

    JobGpuResources::JobGpuResources(DeviceSession& device, std::uint32_t frame_count, std::uint64_t serial) noexcept
        : session(device), generation(serial), accepted(device.makeFrontiers()), frames(frame_count)
    {
    }

    const DynamicRasterPrograms* JobGpuResources::dynamicRasterPrograms(std::uint32_t list) const noexcept
    {
        return list < raster_programs_.size() ? raster_programs_[list].get() : nullptr;
    }

    RenderResult<RasterProgramSelection>
    JobGpuResources::selectRasterPrograms(std::uint32_t pass, const CompiledRasterStream& stream) const noexcept
    {
        if (!stream.runtime) return RasterProgramSelection{stream.programs, 0};
        const auto* dynamic = dynamicRasterPrograms(stream.list);
        if (!dynamic) return cxx::unexpected(RenderError{kFrameContract, {pass}});
        return RasterProgramSelection{dynamic->programs, dynamic->revision};
    }

    void JobGpuResources::releaseRasterPrograms(const DynamicRasterPrograms& selected) noexcept
    {
        const auto id = [&](std::size_t i) noexcept { return selected.programs[i].cache_id; };
        session.programs().release(selected.programs.size(), id, accepted);
    }

    void JobGpuResources::adoptRasterPrograms(std::uint32_t list, std::unique_ptr<DynamicRasterPrograms> candidate) noexcept
    {
        if (list >= raster_programs_.size()) raster_programs_.resize(std::size_t(list) + 1);
        auto& previous = raster_programs_[list];
        if (previous) releaseRasterPrograms(*previous);
        previous = std::move(candidate);
    }

    JobGpuResources::~JobGpuResources() noexcept
    {
        for (const auto& selected : raster_programs_)
            if (selected) releaseRasterPrograms(*selected);
        auto release = [frames = std::move(frames), histories = std::move(histories),
                        core = &session.nriCore()]() mutable noexcept
        {
            for (auto& frame : frames) frame.release(*core);
            for (auto& history : histories) history.release(*core);
        };
        session.deferRelease(std::move(accepted), std::move(release));
    }

    void JobHistoryResources::release(const nri::CoreInterface& core) noexcept
    {
        for (auto& slot : slots)
        {
            for (auto* view : slot.views)
            {
                core.DestroyDescriptor(view);
            }
            for (auto* view : slot.initial_views)
            {
                core.DestroyDescriptor(view);
            }
            core.DestroyTexture(slot.image);
            core.DestroyBuffer(slot.buffer);
        }
    }

    void JobFrameResources::NativeBatch::release(const nri::CoreInterface& core) const noexcept
    {
        for (auto* command : commands) if (command) core.DestroyCommandBuffer(command);
        for (auto* allocator : allocators) if (allocator) core.DestroyCommandAllocator(allocator);
    }

    void JobGpuResources::settlePreparedBuffer(
        std::uint32_t slot, const PreparedBufferCopy& copy, SubmissionPoint point, RenderError error
    ) noexcept
    {
        auto& replica = frames[slot].prepared_buffers[copy.buffer];
        const bool wrong_range = copy.begin >= copy.end || copy.end > copy.size;
        if (replica.generation != copy.generation || wrong_range) std::terminate();
        if (error.type)
        {
            if (error.type == kOutcomeUnknown) replica.accepted = {};
            return;
        }
        if (!point.serial) std::terminate();
        accepted.accept(point);
        auto& contents = replica.accepted;
        if (!copy.begin) contents = {copy.revision, copy.size, 0};
        const bool wrong_selection = contents.revision != copy.revision || contents.size != copy.size;
        if (wrong_selection || contents.prefix != copy.begin) std::terminate();
        contents.prefix = copy.end;
    }

    void JobFrameResources::collectRasterViews(
        nri::Buffer* buffer, nri::Texture* image, std::vector<nri::Descriptor*>& views
    ) noexcept
    {
        if (!buffer && !image) return;
        if (image)
        {
            std::erase_if(raster_attachment_views, [&](const auto& attachment) noexcept
            {
                if (attachment.image != image) return false;
                views.push_back(attachment.view);
                return true;
            });
        }
        for (auto& record : raster_bindings)
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

    void JobFrameResources::release(const nri::CoreInterface& nri_core) noexcept
    {
        for (const auto& batch : batches) batch.release(nri_core);
        nri_core.DestroyDescriptorPool(descriptor_pool);
        for (const auto& binding : raster_bindings)
            for (auto* view : binding.views) nri_core.DestroyDescriptor(view);
        for (const auto& attachment : raster_attachment_views)
            nri_core.DestroyDescriptor(attachment.view);
        for (const auto& pass_views : private_views)
        {
            for (auto* view : pass_views)
            {
                nri_core.DestroyDescriptor(view);
            }
        }
        for (auto* view : views)
        {
            nri_core.DestroyDescriptor(view);
        }
        for (auto* image : images)
        {
            nri_core.DestroyTexture(image);
        }
        for (auto* buffer : buffers)
        {
            nri_core.DestroyBuffer(buffer);
        }
        for (const auto& buffer : prepared_buffers)
            if (buffer.buffer) nri_core.DestroyBuffer(buffer.buffer);
        for (const auto& allocation : memories)
        {
            nri_core.FreeMemory(allocation.memory);
        }
    }

    RenderResult<void>
    JobFrameResources::createBatches(DeviceSession& session, std::span<const SubmissionBatch> schedule) noexcept
    {
        if (!batches.empty()) return cxx::unexpected(RenderError{kDeviceArgument});
        const auto& core = session.nriCore();
        std::vector<NativeBatch> candidate(schedule.size());
        cxx::scope_exit rollback([&]() noexcept
        {
            for (const auto& batch : candidate) batch.release(core);
        });
        for (std::size_t batch = 0; batch < schedule.size(); ++batch)
        {
            const auto& description = schedule[batch];
            const bool has_tail = description.output_begin != kGraphInvalidIndex;
            const std::uint32_t first_part = has_tail && description.output_begin == description.first_pass ? 1u : 0u;
            const std::uint32_t end_part = has_tail ? 2u : 1u;
            for (auto part = first_part; part < end_part; ++part)
            {
                auto result = core.CreateCommandAllocator(
                    session.queue(description.queue).native(), candidate[batch].allocators[part]
                );
                if (result == nri::Result::SUCCESS)
                {
                    result = core.CreateCommandBuffer(
                        *candidate[batch].allocators[part], candidate[batch].commands[part]
                    );
                }
                if (result != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(result));
                }
            }
        }
        batches = std::move(candidate);
        rollback.release();
        return {};
    }
} // namespace lux::render::detail
