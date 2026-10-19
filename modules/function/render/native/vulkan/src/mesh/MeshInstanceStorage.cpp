#include "MeshDraws.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshInstanceStorage.hpp>

namespace lux::render::vulkan
{
    struct MeshInstanceStorage::Backing final
    {
        std::unique_ptr<Buffer> buffer;
        RetirementReservation retirement;
    };

    struct MeshInstanceStorage::Use final
    {
        MeshInstanceStorage* storage;
        PreparedStorageBinding& binding;
        std::uint32_t record_count;
        std::size_t position = 0;

        ~Use() noexcept
        {
            if (storage)
            {
                storage->unsubscribe(*this);
            }
        }

        void update(Backing& value) noexcept
        {
            binding.buffer = value.buffer.get();
            binding.publishUpdate();
        }
    };

    MeshInstanceStorage::MeshInstanceStorage(DeviceSession& session, StorageId id) noexcept : session_(session), id_(id)
    {
    }

    MeshInstanceStorage::~MeshInstanceStorage() noexcept
    {
        for (auto* use : uses_)
        {
            use->storage = nullptr;
        }
        retire(current_);
        for (auto& [selection, subscribers] : draw_subscribers_)
        {
            for (auto* draw : subscribers)
            {
                draw->disconnect();
            }
        }
    }

    void MeshInstanceStorage::retire(std::unique_ptr<Backing>& value) noexcept
    {
        if (value)
        {
            if (!session_.retirement().retire(value->retirement, value))
            {
                std::terminate();
            }
        }
    }

    void MeshInstanceStorage::unsubscribe(Use& use) noexcept
    {
        uses_[use.position] = uses_.back();
        uses_[use.position]->position = use.position;
        uses_.pop_back();
    }

    RenderResult<std::unique_ptr<MeshInstanceStorage::Backing>> MeshInstanceStorage::allocate(
        std::size_t bytes,
        VkBufferUsageFlags usage
    ) noexcept
    {
        const bool exceeds_device =
            bytes > session_.physicalDevice().properties().properties.limits.maxStorageBufferRange;
        if (exceeds_device)
        {
            return cxx::unexpected(RenderError{kStorageCapacity});
        }
        auto buffer = session_.allocator().createBuffer(
            bytes,
            usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            EMemoryAccess::DEVICE
        );
        if (!buffer)
        {
            return cxx::unexpected(buffer.error());
        }
        auto result = std::make_unique<Backing>(std::move(*buffer), session_.retirement().reserve());
        return result;
    }

    void MeshInstanceStorage::recordWrite(
        VkCommandBuffer command,
        Buffer& target,
        VkDeviceSize offset,
        std::span<const std::byte> bytes
    ) noexcept
    {
        VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = target.native();
        barrier.offset = offset;
        barrier.size = bytes.size();
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &dependency);
        for (std::size_t position = 0; position < bytes.size(); position += 65536)
        {
            vkCmdUpdateBuffer(
                command,
                target.native(),
                offset + position,
                std::min<std::size_t>(65536, bytes.size() - position),
                bytes.data() + position
            );
        }
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        vkCmdPipelineBarrier2(command, &dependency);
    }

    void MeshInstanceStorage::publish(std::unique_ptr<Backing> value) noexcept
    {
        retire(current_);
        current_ = std::move(value);
        for (auto* use : uses_)
        {
            use->update(*current_);
        }
    }

    MeshInstanceDescription MeshInstanceStorage::describe(const Entry& entry) const noexcept
    {
        return {entry.mesh, entry.material, entry.submesh, shadow_[entry.key.index]};
    }

    RenderResult<MeshInstancePublished> MeshInstanceStorage::create(const MeshInstanceDescription& input) noexcept
    {
        if (!std::ranges::all_of(input.transform, [](float x) { return std::isfinite(x); }))
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        auto& registry = session_.resources();
        for (auto source : {input.mesh.storage, input.material.storage})
        {
            auto* owner = registry.owner(source);
            if (!owner || (owner != registry.owner(id_) && owner != &session_.globalStorages()))
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
        }
        auto mesh = registry.tryGet<render::MeshStorage>(input.mesh.storage);
        if (!mesh)
        {
            return cxx::unexpected(mesh.error());
        }
        auto mesh_info = mesh->query(input.mesh);
        if (!mesh_info)
        {
            return cxx::unexpected(mesh_info.error());
        }
        if (input.submesh >= mesh_info->description.submeshes.size())
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        auto material = registry.tryGet<render::MaterialStorage>(input.material.storage);
        if (!material)
        {
            return cxx::unexpected(material.error());
        }
        if (auto info = material->query(input.material); !info)
        {
            return cxx::unexpected(info.error());
        }
        const auto key = entries_.emplace(Entry{input.mesh, input.material, input.submesh});
        entries_.find(key)->key = key;
        cxx::scope_exit rollback([&]() noexcept { entries_.erase(key); });
        const auto subscribers = draw_subscribers_.find({input.mesh, input.material, input.submesh});
        const bool exceeds_binding =
            std::ranges::any_of(uses_, [&](const auto* use) { return key.index >= use->record_count; });
        const bool exceeds_group =
            subscribers != draw_subscribers_.end() &&
            std::ranges::any_of(subscribers->second, [&](const auto* draw) { return key.index >= draw->capacity(); });
        if (exceeds_binding || exceeds_group)
        {
            return cxx::unexpected(RenderError{kStorageCapacity});
        }
        const bool growing = key.index >= shadow_.size();
        std::vector<TransformMatrix> grown;
        std::unique_ptr<Backing> candidate;
        if (growing)
        {
            const auto capacity = (std::size_t(key.index) / kPageSize + 1) * kPageSize;
            auto allocated = allocate(capacity * sizeof(TransformMatrix), 0);
            if (!allocated)
            {
                return cxx::unexpected(allocated.error());
            }
            candidate = std::move(*allocated);
            grown = shadow_;
            grown.resize(capacity);
            grown[key.index] = input.transform;
        }
        auto batch = session_.queue().begin();
        if (!batch)
        {
            return cxx::unexpected(batch.error());
        }
        if (growing)
        {
            recordWrite(batch->native(), *candidate->buffer, 0, std::as_bytes(std::span(grown)));
        }
        else
        {
            recordWrite(
                batch->native(),
                *current_->buffer,
                std::uint64_t(key.index) * sizeof(TransformMatrix),
                std::as_bytes(std::span(&input.transform, 1))
            );
        }
        auto accepted = std::move(*batch).submit();
        if (!accepted)
        {
            return cxx::unexpected(accepted.error());
        }
        if (growing)
        {
            shadow_ = std::move(grown);
            page_positions_.resize(shadow_.size() / kPageSize, UINT32_MAX);
            publish(std::move(candidate));
            written_records_ += shadow_.size();
        }
        else
        {
            shadow_[key.index] = input.transform;
            ++written_records_;
        }
        if (subscribers != draw_subscribers_.end())
        {
            for (auto* draw : subscribers->second)
            {
                draw->added(key.index);
            }
        }
        rollback.release();
        return MeshInstancePublished{{id_, key}, 1};
    }

    RenderResult<TransformApplied> MeshInstanceStorage::update(const TransformBatch& input) noexcept
    {
        if (input.storage != id_ || input.updates.size() > UINT32_MAX)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        for (const auto& value : input.updates)
        {
            ++checked_updates_;
            const auto* entry = value.instance.storage == id_ ? entries_.find(value.instance.local) : nullptr;
            if (!entry)
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            const bool revision_mismatch = value.expected_revision != entry->revision || entry->revision == UINT64_MAX;
            const bool invalid_transform =
                !std::ranges::all_of(value.transform, [](float x) { return std::isfinite(x); });
            if (revision_mismatch || invalid_transform)
            {
                return cxx::unexpected(RenderError{kStorageContract});
            }
        }
        if (input.updates.empty())
        {
            return TransformApplied{input.sequence, 0};
        }
        auto batch = session_.queue().begin();
        if (!batch)
        {
            return cxx::unexpected(batch.error());
        }
        pages_.clear();
        cxx::scope_exit clear_pages(
            [&]() noexcept
            {
                for (const auto& page : pages_)
                {
                    page_positions_[page.index] = UINT32_MAX;
                }
            }
        );
        // Legacy TPagedGpuStream's unique dirty-page list. Request elements are already unique;
        // no Server sort/dedup and no walk over all live slots or previously dirty banks.
        for (const auto& value : input.updates)
        {
            const auto page_index = value.instance.local.index / kPageSize;
            auto& position = page_positions_[page_index];
            if (position == UINT32_MAX)
            {
                position = static_cast<std::uint32_t>(pages_.size());
                auto& page = pages_.emplace_back();
                page.index = page_index;
                std::copy_n(shadow_.data() + std::size_t(page_index) * kPageSize, kPageSize, page.values.data());
            }
            pages_[position].values[value.instance.local.index % kPageSize] = value.transform;
        }
        for (const auto& page : pages_)
        {
            recordWrite(
                batch->native(),
                *current_->buffer,
                std::uint64_t(page.index) * kPageSize * sizeof(TransformMatrix),
                std::as_bytes(std::span(page.values))
            );
        }
        auto accepted = std::move(*batch).submit();
        if (!accepted)
        {
            return cxx::unexpected(accepted.error());
        }
        for (const auto& page : pages_)
        {
            std::copy(page.values.begin(), page.values.end(), shadow_.begin() + std::size_t(page.index) * kPageSize);
        }
        for (const auto& value : input.updates)
        {
            ++entries_.find(value.instance.local)->revision;
        }
        written_records_ += pages_.size() * kPageSize;
        return TransformApplied{input.sequence, static_cast<std::uint32_t>(input.updates.size())};
    }

    RenderResult<MeshInstanceInformation> MeshInstanceStorage::query(MeshInstanceId id) const noexcept
    {
        const auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        return MeshInstanceInformation{{id, entry->revision}, describe(*entry)};
    }

    RenderResult<void> MeshInstanceStorage::release(MeshInstancePublished input) noexcept
    {
        const auto* entry = input.instance.storage == id_ ? entries_.find(input.instance.local) : nullptr;
        if (!entry)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (input.revision != entry->revision)
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        const auto subscribers = draw_subscribers_.find({entry->mesh, entry->material, entry->submesh});
        if (subscribers != draw_subscribers_.end())
        {
            for (auto* draw : subscribers->second)
            {
                draw->removed(input.instance.local.index);
            }
        }
        entries_.erase(input.instance.local);
        return {};
    }

    RenderResult<void> MeshInstanceStorage::prepare(
        const NativeResourcePlan& plan,
        const MeshInstanceBinding& input,
        PreparedStorageBinding& binding
    ) noexcept
    {
        const auto* description = std::get_if<GraphBufferDesc>(&plan.description);
        const bool indirect = input.role == EMeshInstanceBinding::DRAW;
        const auto bytes = indirect ? (std::uint64_t(input.record_count) + 5) * 4
                                    : std::uint64_t(input.record_count) * sizeof(TransformMatrix);
        const bool invalid_role = input.role != EMeshInstanceBinding::MATRICES && !indirect;
        const bool invalid_range = !description || !input.record_count || input.record_count > shadow_.size() ||
                                   (description && description->size != bytes);
        if (invalid_role || invalid_range || (indirect && !input.group))
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        if (indirect)
        {
            return MeshDraws::prepare(*this, input, binding);
        }
        auto use = std::make_unique<Use>(this, binding, input.record_count, uses_.size());
        uses_.push_back(use.get());
        use->update(*current_);
        binding.reference = {use.release(), [](void* value) noexcept { delete static_cast<Use*>(value); }};
        return {};
    }

    StorageFactory meshInstanceStorageFactory() noexcept
    {
        static const render::MeshInstanceStorage::Ops ops{
            [](const void* value, MeshInstanceId id) noexcept
            { return static_cast<const MeshInstanceStorage*>(value)->query(id); }
        };
        static const StorageBindingOps binding{
            [](void* value, const NativeResourcePlan& plan, const void* argument, PreparedStorageBinding& prepared
            ) noexcept
            {
                return static_cast<MeshInstanceStorage*>(value)
                    ->prepare(plan, *static_cast<const MeshInstanceBinding*>(argument), prepared);
            }
        };
        static const auto create =
            +[](DeviceSession& session, StorageId id) noexcept -> RenderResult<std::unique_ptr<MeshInstanceStorage>>
        { return std::make_unique<MeshInstanceStorage>(session, id); };
        return makeStorageFactory<render::MeshInstanceStorage, MeshInstanceStorage, DeviceSession, create>(
            ops,
            &binding
        );
    }
} // namespace lux::render::vulkan
