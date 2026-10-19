#include "MeshDraws.hpp"
#include "Cleanup.hpp"
#include <bit>
#include <lux/engine/render/native/vulkan/material/MaterialStorage.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshStorage.hpp>

namespace lux::render::vulkan
{
    MeshDraws::MeshDraws(
        DeviceSession& session,
        ExecutionContext& context,
        std::uint32_t index,
        GraphResourceId resource,
        MeshInstanceDrawGroup selection
    ) noexcept
        : session_(session), context_(context), data_index_(index), resource_(resource), selection_(selection),
          words_(5)
    {
    }

    MeshDraws::~MeshDraws() noexcept
    {
        observer_.reset();
        releaseAfterGpu(session_, buffer_);
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
    }

    RenderResult<void> MeshDraws::prepare() noexcept
    {
        if (!available_)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto bytes = std::max<std::size_t>(24, words_.size() * sizeof(std::uint32_t));
        if (!buffer_ || buffer_.size() < bytes)
        {
            const auto size = std::bit_ceil(bytes);
            if (size > session_.physicalDevice().properties().properties.limits.maxStorageBufferRange)
            {
                return cxx::unexpected(RenderError{kStorageCapacity});
            }
            auto buffer = session_.allocator().createBuffer(
                size,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                EMemoryAccess::DEVICE
            );
            if (!buffer)
            {
                return cxx::unexpected(buffer.error());
            }
            if (buffer_)
            {
                releaseAfterGpu(session_, buffer_);
            }
            buffer_ = std::move(*buffer);
            context_.setResource(resource_.index, {buffer_.handle(), 0, buffer_.size()});
        }
        return {};
    }

    void MeshDraws::record(VkCommandBuffer command) noexcept
    {
        VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer_.handle();
        barrier.size = VK_WHOLE_SIZE;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &dependency);
        const auto bytes = std::as_bytes(std::span(words_));
        for (std::size_t offset = 0; offset < bytes.size(); offset += 65536)
        {
            vkCmdUpdateBuffer(
                command,
                buffer_.handle(),
                offset,
                std::min<std::size_t>(65536, bytes.size() - offset),
                bytes.data() + offset
            );
        }
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        // Geometry fields come from Storage's actual GPU section table. The Feature supplies only
        // its selection count and member list; Storage never constructs a draw command.
        vkCmdPipelineBarrier2(command, &dependency);
        const auto section = sections_.offset + std::uint64_t(selection_.submesh) * sizeof(MeshGpuSection);
        const std::array<VkBufferCopy, 3> fields{{{section, 0, 4}, {section + 4, 8, 4}, {section + 8, 12, 4}}};
        vkCmdCopyBuffer(
            command,
            sections_.buffer,
            buffer_.handle(),
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
        std::uint32_t index,
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
        auto value = std::unique_ptr<MeshDraws>(new MeshDraws(session, context, index, resource, group));
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
        if (auto prepared = value->prepare(); !prepared)
        {
            return cxx::unexpected(prepared.error());
        }
        value->observer_.receiver = value.get();
        value->observer_.changed =
            [](void* pointer, MeshInstanceId id, const MeshInstanceDescription* description) noexcept
        {
            auto& self = *static_cast<MeshDraws*>(pointer);
            const auto count = self.words_[1];
            self.update(id, description);
            if (!self.available_ || count != self.words_[1])
            {
                self.context_.invalidateData(self.data_index_);
            }
        };
        (*instances)->observe(value->observer_);
        return ContextData{
            {value.release(), [](void* pointer) noexcept { delete static_cast<MeshDraws*>(pointer); }},
            [](void* pointer) noexcept { return static_cast<MeshDraws*>(pointer)->prepare(); },
            [](void* pointer, VkCommandBuffer command) noexcept { static_cast<MeshDraws*>(pointer)->record(command); }
        };
    }
} // namespace lux::render::vulkan
