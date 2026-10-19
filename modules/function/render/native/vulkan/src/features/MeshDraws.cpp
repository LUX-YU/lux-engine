#include "MeshDraws.hpp"
#include "Cleanup.hpp"
#include <bit>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialStorage.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshStorage.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>

namespace lux::render::vulkan
{
    MeshDraws::MeshDraws(
        DeviceSession& session,
        ExecutionContext& context,
        GraphResourceId resource,
        MeshInstanceDrawGroup selection
    ) noexcept
        : session_(session), context_(context), resource_(resource), selection_(selection), words_(5)
    {
    }

    MeshDraws::~MeshDraws() noexcept
    {
        observer_.reset();
        for (auto& slice : gpu_)
        {
            releaseAfterGpu(session_, slice.buffer);
        }
    }

    bool MeshDraws::selected(const MeshInstanceDescription& input) const noexcept
    {
        if (input.mesh != selection_.mesh || input.submesh != selection_.submesh ||
            input.material.storage != selection_.material.storage)
        {
            return false;
        }
        auto material = session_.resources().borrow<MaterialStorage>(selection_.material.storage);
        return material && (*material)->sameTable(input.material, selection_.material);
    }

    void MeshDraws::update(MeshInstanceId id, const MeshInstanceDescription* description) noexcept
    {
        if (id.isNull())
        {
            available_ = false;
            return;
        }
        const auto index = id.local.index;
        const bool present = index < positions_.size() && positions_[index] != UINT32_MAX;
        const bool include = description && selected(*description);
        if (present == include)
        {
            return;
        }
        if (include)
        {
            if (index >= positions_.size())
            {
                positions_.resize(std::bit_ceil(std::size_t(index) + 1), UINT32_MAX);
            }
            positions_[index] = words_[1]++;
            words_.push_back(index);
        }
        else
        {
            const auto position = positions_[index];
            const auto last = words_.back();
            words_[5 + position] = last;
            positions_[last] = position;
            positions_[index] = UINT32_MAX;
            words_.pop_back();
            --words_[1];
        }
        ++revision_;
    }

    RenderResult<ContextData::EPreparation> MeshDraws::prepare(std::uint32_t slot, UploadContext& uploads) noexcept
    {
        if (!available_)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto& slice = gpu_[slot];
        slice.pending_revision = 0;
        if (slice.uploaded_revision == revision_)
        {
            return ContextData::EPreparation::READY;
        }
        const auto bytes = std::max<std::size_t>(24, words_.size() * sizeof(std::uint32_t));
        if (slice.buffer.size() < bytes)
        {
            const auto capacity = std::bit_ceil(bytes);
            if (capacity > session_.physicalDevice().properties().properties.limits.maxStorageBufferRange)
            {
                return cxx::unexpected(RenderError{kStorageCapacity});
            }
            auto buffer = session_.allocator().createBuffer(
                capacity,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                EMemoryAccess::DEVICE
            );
            if (!buffer)
            {
                return cxx::unexpected(buffer.error());
            }
            releaseAfterGpu(session_, slice.buffer);
            slice.buffer = std::move(*buffer);
            slice.uploaded_bytes = 0;
            context_.setResource(resource_.index, slot, {slice.buffer.handle(), 0, slice.buffer.size()});
        }
        const auto source = std::as_bytes(std::span(words_));
        slice.pending_bytes = slice.uploaded_bytes;
        cxx::scope_exit register_ack(
            [&]() noexcept
            {
                if (slice.pending_bytes == slice.uploaded_bytes && !slice.pending_revision)
                {
                    return;
                }
                uploads.afterFrameSubmit(
                    &slice,
                    [](void* value, RenderResult<void> accepted) noexcept
                    {
                        auto& slice = *static_cast<GpuSlice*>(value);
                        if (accepted)
                        {
                            slice.uploaded_bytes = slice.pending_bytes;
                            if (slice.pending_revision)
                            {
                                slice.uploaded_revision = slice.pending_revision;
                                slice.uploaded_bytes = 0;
                            }
                        }
                        slice.pending_revision = 0;
                    }
                );
            }
        );
        while (slice.pending_bytes < source.size())
        {
            auto copied =
                uploads.prepareBuffer({&slice.buffer, slice.pending_bytes, source.subspan(slice.pending_bytes)});
            if (!copied)
            {
                return cxx::unexpected(copied.error());
            }
            if (*copied == 0)
            {
                return ContextData::EPreparation::WAITING;
            }
            slice.pending_bytes += *copied;
        }
        slice.pending_revision = revision_;
        return ContextData::EPreparation::RECORD;
    }

    void MeshDraws::record(VkCommandBuffer command, std::uint32_t slot) noexcept
    {
        auto& slice = gpu_[slot];
        if (!slice.pending_revision)
        {
            return;
        }
        VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = slice.buffer.handle();
        barrier.size = VK_WHOLE_SIZE;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &dependency);
        const auto section = sections_.offset + std::uint64_t(selection_.submesh) * sizeof(MeshGpuSection);
        const std::array<VkBufferCopy, 3> fields{{{section, 0, 4}, {section + 4, 8, 4}, {section + 8, 12, 4}}};
        vkCmdCopyBuffer(
            command,
            sections_.buffer,
            slice.buffer.handle(),
            static_cast<std::uint32_t>(fields.size()),
            fields.data()
        );
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        vkCmdPipelineBarrier2(command, &dependency);
    }

    RenderResult<ContextData> MeshDraws::create(
        DeviceSession& session,
        ExecutionContext& context,
        GraphResourceId resource,
        const void* argument
    ) noexcept
    {
        const auto& configuration = *static_cast<const MeshDrawConfiguration*>(argument);
        const auto& group = configuration.selection;
        auto& registry = session.resources();
        auto instances = registry.borrow<MeshInstanceStorage>(context.storageId(configuration.instances.index));
        auto mesh = registry.borrow<MeshStorage>(group.mesh.storage);
        auto material = registry.borrow<MaterialStorage>(group.material.storage);
        if (!instances || !mesh || !material)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto* description = (*mesh)->find(group.mesh);
        if (!description || group.submesh >= description->submeshes.size() || !(*material)->gpuIndex(group.material))
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto value = std::unique_ptr<MeshDraws>(new MeshDraws(session, context, resource, group));
        const NativeResourcePlan section_plan{
            "sections",
            GraphBufferDesc{description->submeshes.size() * sizeof(MeshGpuSection)},
            EGraphOrigin::IMPORTED
        };
        StorageSubscription unused;
        auto sections = (*mesh)->resolve(
            section_plan,
            MeshBinding{group.mesh, EMeshBuffer::SECTIONS, sizeof(MeshGpuSection)},
            unused
        );
        if (!sections)
        {
            return cxx::unexpected(sections.error());
        }
        value->sections_ = *sections;
        (*instances)
            ->forEach([&](MeshInstanceId id, const MeshInstanceDescription& input) noexcept
                      { value->update(id, &input); });
        value->observer_.receiver = value.get();
        value->observer_.changed =
            [](void* pointer, MeshInstanceId id, const MeshInstanceDescription* description) noexcept
        {
            auto& self = *static_cast<MeshDraws*>(pointer);
            self.update(id, description);
        };
        (*instances)->observe(value->observer_);
        return ContextData{
            {value.release(), [](void* pointer) noexcept { delete static_cast<MeshDraws*>(pointer); }},
            [](void* pointer, std::uint32_t slot, UploadContext& uploads) noexcept
            { return static_cast<MeshDraws*>(pointer)->prepare(slot, uploads); },
            [](void* pointer, VkCommandBuffer command, std::uint32_t slot) noexcept
            { static_cast<MeshDraws*>(pointer)->record(command, slot); }
        };
    }
} // namespace lux::render::vulkan
