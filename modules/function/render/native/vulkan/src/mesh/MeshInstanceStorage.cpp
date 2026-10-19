#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialStorage.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshInstanceStorage.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshStorage.hpp>

namespace lux::render::vulkan
{
    struct MeshInstanceStorage::GpuPage final
    {
        std::array<std::unique_ptr<Buffer>, 3> fields;
        RetirementReservation retirement;
    };

    struct MeshInstanceStorage::Page final
    {
        std::array<TransformMatrix, kMeshInstancePageSlots> transforms{};
        std::array<MeshInstanceGpuProperty, kMeshInstancePageSlots> properties{};
        std::array<std::uint32_t, kMeshInstancePageSlots> alive{};
        std::unique_ptr<GpuPage> gpu;
    };

    struct MeshInstanceStorage::PageTable final
    {
        struct Leaf final
        {
            std::unique_ptr<Buffer> buffer;
            RetirementReservation retirement;
        };

        std::unique_ptr<Buffer> root;
        RetirementReservation retirement;
        std::vector<Leaf> leaves;
    };

    namespace
    {
        constexpr std::array<std::size_t, 3> kStride{
            sizeof(TransformMatrix),
            sizeof(MeshInstanceGpuProperty),
            sizeof(std::uint32_t)
        };

        void beforeWrite(VkCommandBuffer command) noexcept
        {
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(command, &dependency);
        }

        void afterWrite(VkCommandBuffer command) noexcept
        {
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(command, &dependency);
        }

        std::uint64_t address(DeviceSession& session, const Buffer& buffer) noexcept
        {
            VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            info.buffer = buffer.native();
            return vkGetBufferDeviceAddress(session.nativeDevice(), &info);
        }
    } // namespace

    MeshInstanceStorage::MeshInstanceStorage(DeviceSession& session, StorageId id) noexcept
        : session_(session), id_(id), paged_(session.enabledFeatures().buffer_device_address)
    {
    }

    MeshInstanceStorage::~MeshInstanceStorage() noexcept
    {
        for (auto& head : bindings_)
        {
            StorageSubscription::disconnect(head);
        }
        for (auto* observer : observers_)
        {
            observer->source_ = nullptr;
            observer->changed(observer->receiver, {}, nullptr);
        }
        for (auto& page : pages_)
        {
            retire(page->gpu);
        }
        retire(flat_);
        if (page_table_)
        {
            for (auto& leaf : page_table_->leaves)
            {
                if (leaf.buffer && !session_.retirement().retire(leaf.retirement, leaf.buffer))
                {
                    std::terminate();
                }
            }
            if (!session_.retirement().retire(page_table_->retirement, page_table_->root))
            {
                std::terminate();
            }
        }
    }

    MeshInstanceStorage::Observer::~Observer() noexcept
    {
        reset();
    }

    void MeshInstanceStorage::Observer::reset() noexcept
    {
        if (source_)
        {
            auto& records = source_->observers_;
            records[position_] = records.back();
            records[position_]->position_ = position_;
            records.pop_back();
            source_ = nullptr;
        }
    }

    void MeshInstanceStorage::observe(Observer& observer) noexcept
    {
        observer.reset();
        observer.source_ = this;
        observer.position_ = observers_.size();
        observers_.push_back(&observer);
    }

    void MeshInstanceStorage::notify(const Entry& entry, bool removed) noexcept
    {
        const auto description = describe(entry);
        for (auto* observer : observers_)
        {
            observer->changed(observer->receiver, {id_, entry.key}, removed ? nullptr : &description);
        }
    }

    std::uint64_t MeshInstanceStorage::capacity() const noexcept
    {
        return std::uint64_t(pages_.size()) * kMeshInstancePageSlots;
    }

    void MeshInstanceStorage::retire(std::unique_ptr<GpuPage>& page) noexcept
    {
        if (page && !session_.retirement().retire(page->retirement, page))
        {
            std::terminate();
        }
    }

    RenderResult<std::unique_ptr<MeshInstanceStorage::GpuPage>> MeshInstanceStorage::allocate(std::size_t slots
    ) noexcept
    {
        auto result = std::make_unique<GpuPage>();
        result->retirement = session_.retirement().reserve();
        const auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT | (paged_ ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
        for (std::size_t field = 0; field < kStride.size(); ++field)
        {
            const auto bytes = slots * kStride[field];
            if (bytes > session_.physicalDevice().properties().properties.limits.maxStorageBufferRange)
            {
                return cxx::unexpected(RenderError{kStorageCapacity});
            }
            auto buffer = session_.allocator().createBuffer(bytes, usage, EMemoryAccess::DEVICE);
            if (!buffer)
            {
                return cxx::unexpected(buffer.error());
            }
            result->fields[field] = std::move(*buffer);
        }
        return result;
    }

    Buffer& MeshInstanceStorage::fieldBuffer(std::uint32_t page, EMeshInstanceBinding field) const noexcept
    {
        const auto& owner = paged_ ? pages_[page]->gpu : flat_;
        return *owner->fields[static_cast<std::size_t>(field)];
    }

    VkDeviceSize MeshInstanceStorage::fieldOffset(std::uint32_t slot, std::size_t stride) const noexcept
    {
        return std::uint64_t(paged_ ? slot % kMeshInstancePageSlots : slot) * stride;
    }

    void MeshInstanceStorage::publishBuffers() noexcept
    {
        for (std::size_t field = 0; field < bindings_.size(); ++field)
        {
            auto* buffer = paged_ ? page_table_->root.get() : flat_->fields[field].get();
            StorageSubscription::publish(bindings_[field], {buffer, 0, buffer->size()});
        }
    }

    MeshInstanceDescription MeshInstanceStorage::describe(const Entry& entry) const noexcept
    {
        const auto slot = entry.key.index;
        return {
            entry.mesh,
            entry.material,
            entry.submesh,
            pages_[slot / kMeshInstancePageSlots]->transforms[slot % kMeshInstancePageSlots]
        };
    }

    exec::task<RenderResult<MeshInstancePublished>> MeshInstanceStorage::create(MeshInstanceDescription input) noexcept
    {
        if (updating_)
        {
            co_return cxx::unexpected(RenderError{kBusy});
        }
        if (!std::ranges::all_of(input.transform, [](float value) { return std::isfinite(value); }))
        {
            co_return cxx::unexpected(RenderError{kStorageContract});
        }
        auto& registry = session_.resources();
        for (auto source : {input.mesh.storage, input.material.storage})
        {
            const auto* owner = registry.owner(source);
            if (!owner || (owner != registry.owner(id_) && owner != &session_.globalStorages()))
            {
                co_return cxx::unexpected(RenderError{kStorageIdentity});
            }
        }
        auto mesh = registry.borrow<vulkan::MeshStorage>(input.mesh.storage);
        auto material = registry.borrow<vulkan::MaterialStorage>(input.material.storage);
        if (!mesh || !material)
        {
            co_return cxx::unexpected(!mesh ? mesh.error() : material.error());
        }
        const auto* description = (*mesh)->find(input.mesh);
        const auto material_slot = (*material)->gpuIndex(input.material);
        if (!description || !material_slot || input.submesh >= description->submeshes.size())
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const auto key = entries_.emplace(Entry{input.mesh, input.material, input.submesh});
        entries_.find(key)->key = key;
        updating_ = true;
        std::unique_ptr<Page> page;
        std::unique_ptr<GpuPage> gpu;
        std::unique_ptr<PageTable> root;
        PageTable::Leaf leaf;
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                updating_ = false;
                entries_.erase(key);
                retire(gpu);
            }
        );
        const auto page_index = key.index / kMeshInstancePageSlots;
        const auto local = key.index % kMeshInstancePageSlots;
        const bool growing = page_index >= pages_.size();
        if (growing)
        {
            page = std::make_unique<Page>();
            auto allocated =
                allocate(paged_ ? kMeshInstancePageSlots : std::size_t(capacity()) + kMeshInstancePageSlots);
            if (!allocated)
            {
                co_return cxx::unexpected(allocated.error());
            }
            gpu = std::move(*allocated);
            if (paged_)
            {
                if (!page_table_)
                {
                    root = std::make_unique<PageTable>();
                    root->retirement = session_.retirement().reserve();
                    auto buffer = session_.allocator().createBuffer(
                        kMeshInstanceTableAxis * sizeof(std::uint64_t),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        EMemoryAccess::DEVICE
                    );
                    if (!buffer)
                    {
                        co_return cxx::unexpected(buffer.error());
                    }
                    root->root = std::move(*buffer);
                }
                const auto leaf_index = page_index / kMeshInstanceTableAxis;
                if (leaf_index >= kMeshInstanceTableAxis)
                {
                    co_return cxx::unexpected(RenderError{kStorageCapacity}); // Entire 32-bit slot address space.
                }
                if (!page_table_ || leaf_index >= page_table_->leaves.size())
                {
                    auto buffer = session_.allocator().createBuffer(
                        kMeshInstanceTableAxis * sizeof(MeshInstancePageGpu),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                        EMemoryAccess::DEVICE
                    );
                    if (!buffer)
                    {
                        co_return cxx::unexpected(buffer.error());
                    }
                    leaf.buffer = std::move(*buffer);
                    leaf.retirement = session_.retirement().reserve();
                }
            }
        }
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        std::optional<CommandBatch> batch;
        for (;;)
        {
            if (stop.stop_requested())
            {
                co_return cxx::unexpected(RenderError{kWorkStopped});
            }
            auto ready = session_.queue().begin();
            if (ready)
            {
                batch.emplace(std::move(*ready));
                break;
            }
            if (ready.error().type != kCapacity)
            {
                co_return cxx::unexpected(ready.error());
            }
            auto completed = co_await session_.queue().whenAvailable();
            if (!completed)
            {
                co_return cxx::unexpected(completed.error());
            }
        }
        // No suspension after begin: all stream writes and page-table publication share one accepted batch.
        const auto command = batch->native();
        beforeWrite(command);
        if (growing)
        {
            for (auto& field : gpu->fields)
            {
                vkCmdFillBuffer(command, field->native(), 0, field->size(), 0);
            }
            if (root)
            {
                vkCmdFillBuffer(command, root->root->native(), 0, root->root->size(), 0);
            }
            if (leaf.buffer)
            {
                vkCmdFillBuffer(command, leaf.buffer->native(), 0, leaf.buffer->size(), 0);
            }
            beforeWrite(command);
            if (!paged_ && flat_)
            {
                for (std::size_t f = 0; f < kStride.size(); ++f)
                {
                    const VkBufferCopy copy{0, 0, flat_->fields[f]->size()};
                    vkCmdCopyBuffer(command, flat_->fields[f]->native(), gpu->fields[f]->native(), 1, &copy);
                }
            }
        }
        const MeshInstanceGpuProperty property{input.mesh.local.index, *material_slot, input.submesh};
        const std::uint32_t alive = 1;
        auto& target = growing ? *gpu : *(paged_ ? pages_[page_index]->gpu : flat_);
        const auto slot = paged_ ? local : key.index;
        vkCmdUpdateBuffer(
            command,
            target.fields[0]->native(),
            std::uint64_t(slot) * kStride[0],
            kStride[0],
            input.transform.data()
        );
        vkCmdUpdateBuffer(command, target.fields[1]->native(), std::uint64_t(slot) * kStride[1], kStride[1], &property);
        vkCmdUpdateBuffer(command, target.fields[2]->native(), std::uint64_t(slot) * kStride[2], kStride[2], &alive);
        if (growing && paged_)
        {
            auto& table = root ? *root : *page_table_;
            auto& selected = leaf.buffer ? leaf : table.leaves[page_index / kMeshInstanceTableAxis];
            const MeshInstancePageGpu addresses{
                address(session_, *gpu->fields[0]),
                address(session_, *gpu->fields[1]),
                address(session_, *gpu->fields[2])
            };
            vkCmdUpdateBuffer(
                command,
                selected.buffer->native(),
                (page_index % kMeshInstanceTableAxis) * sizeof(addresses),
                sizeof(addresses),
                &addresses
            );
            if (leaf.buffer)
            {
                const auto leaf_address = address(session_, *leaf.buffer);
                vkCmdUpdateBuffer(
                    command,
                    table.root->native(),
                    (page_index / kMeshInstanceTableAxis) * sizeof(leaf_address),
                    sizeof(leaf_address),
                    &leaf_address
                );
            }
        }
        afterWrite(command);
        auto accepted = std::move(*batch).submit();
        if (!accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        if (growing)
        {
            if (paged_)
            {
                page->gpu = std::move(gpu);
                if (root)
                {
                    page_table_ = std::move(root);
                }
                if (leaf.buffer)
                {
                    page_table_->leaves.push_back(std::move(leaf));
                }
            }
            else
            {
                retire(flat_);
                flat_ = std::move(gpu);
            }
            pages_.push_back(std::move(page));
            dirty_positions_.resize(std::size_t(capacity()) / kMeshInstanceDirtySlots, UINT32_MAX);
            publishBuffers();
        }
        pages_[page_index]->transforms[local] = input.transform;
        pages_[page_index]->properties[local] = property;
        pages_[page_index]->alive[local] = 1;
        ++written_records_;
        ++live_records_;
        entries_.find(key)->revision = 1;
        updating_ = false;
        notify(*entries_.find(key), false);
        rollback.release();
        co_return MeshInstancePublished{{id_, key}, 1};
    }

    exec::task<RenderResult<TransformApplied>> MeshInstanceStorage::update(TransformBatch input) noexcept
    {
        if (updating_)
        {
            co_return cxx::unexpected(RenderError{kBusy});
        }
        if (input.storage != id_ || input.updates.size() > UINT32_MAX)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        for (const auto& value : input.updates)
        {
            ++checked_updates_;
            const auto* entry = value.instance.storage == id_ ? entries_.find(value.instance.local) : nullptr;
            if (!entry)
            {
                co_return cxx::unexpected(RenderError{kStorageIdentity});
            }
            const bool is_revision_mismatch =
                entry->revision != value.expected_revision || entry->revision == UINT64_MAX;
            const bool is_invalid_transform =
                !std::ranges::all_of(value.transform, [](float element) { return std::isfinite(element); });
            if (is_revision_mismatch || is_invalid_transform)
            {
                co_return cxx::unexpected(RenderError{kStorageContract});
            }
        }
        if (input.updates.empty())
        {
            co_return TransformApplied{input.sequence, 0};
        }
        updating_ = true;

        struct DirtyBlock final
        {
            std::uint32_t index;
            std::array<TransformMatrix, kMeshInstanceDirtySlots> values;
        };

        std::vector<DirtyBlock> dirty;
        std::unique_ptr<Buffer> staged;
        auto retirement = session_.retirement().reserve();
        cxx::scope_exit cleanup(
            [&]() noexcept
            {
                for (const auto& block : dirty)
                {
                    dirty_positions_[block.index] = UINT32_MAX;
                }
                updating_ = false;
                if (staged && !session_.retirement().retire(retirement, staged))
                {
                    std::terminate();
                }
            }
        );
        // Legacy dirty-upload flags collect unique 512-slot blocks, separately from 16K storage pages.
        // Request items are already unique; only dirty block indices are sorted to merge GPU copies.
        for (const auto& value : input.updates)
        {
            const auto slot = value.instance.local.index;
            const auto index = slot / kMeshInstanceDirtySlots;
            auto& position = dirty_positions_[index];
            if (position == UINT32_MAX)
            {
                position = static_cast<std::uint32_t>(dirty.size());
                auto& block = dirty.emplace_back();
                block.index = index;
                const auto first = (index * kMeshInstanceDirtySlots) % kMeshInstancePageSlots;
                std::copy_n(
                    pages_[slot / kMeshInstancePageSlots]->transforms.data() + first,
                    kMeshInstanceDirtySlots,
                    block.values.data()
                );
            }
            dirty[position].values[slot % kMeshInstanceDirtySlots] = value.transform;
        }
        std::ranges::sort(dirty, {}, &DirtyBlock::index);
        const auto block_bytes = sizeof(TransformMatrix) * kMeshInstanceDirtySlots;
        std::vector<std::byte> bytes(dirty.size() * block_bytes);
        for (std::size_t i = 0; i < dirty.size(); ++i)
        {
            std::memcpy(bytes.data() + i * block_bytes, dirty[i].values.data(), block_bytes);
        }
        auto buffer = session_.allocator().createBuffer(
            bytes.size(),
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            EMemoryAccess::DEVICE
        );
        if (!buffer)
        {
            co_return cxx::unexpected(buffer.error());
        }
        staged = std::move(*buffer);
        auto uploaded = co_await session_.uploads().writeBuffer(*staged, bytes);
        if (!uploaded)
        {
            co_return cxx::unexpected(uploaded.error());
        }
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        std::optional<CommandBatch> batch;
        for (;;)
        {
            if (stop.stop_requested())
            {
                co_return cxx::unexpected(RenderError{kWorkStopped});
            }
            auto ready = session_.queue().begin();
            if (ready)
            {
                batch.emplace(std::move(*ready));
                break;
            }
            if (ready.error().type != kCapacity)
            {
                co_return cxx::unexpected(ready.error());
            }
            auto completed = co_await session_.queue().whenAvailable();
            if (!completed)
            {
                co_return cxx::unexpected(completed.error());
            }
        }
        beforeWrite(batch->native());
        for (std::size_t first = 0; first < dirty.size();)
        {
            const auto slot = dirty[first].index * kMeshInstanceDirtySlots;
            const auto page_index = slot / kMeshInstancePageSlots;
            auto end = first + 1;
            while (end < dirty.size() && dirty[end].index == dirty[end - 1].index + 1 &&
                   (!paged_ || dirty[end].index * kMeshInstanceDirtySlots / kMeshInstancePageSlots == page_index))
            {
                ++end;
            }
            auto& target = fieldBuffer(page_index, EMeshInstanceBinding::TRANSFORMS);
            const VkBufferCopy copy{
                first * block_bytes,
                fieldOffset(slot, sizeof(TransformMatrix)),
                (end - first) * block_bytes
            };
            vkCmdCopyBuffer(batch->native(), staged->native(), target.native(), 1, &copy);
            first = end;
        }
        afterWrite(batch->native());
        auto accepted = std::move(*batch).submit();
        if (!accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        for (const auto& block : dirty)
        {
            const auto slot = block.index * kMeshInstanceDirtySlots;
            std::copy(
                block.values.begin(),
                block.values.end(),
                pages_[slot / kMeshInstancePageSlots]->transforms.data() + slot % kMeshInstancePageSlots
            );
        }
        for (const auto& value : input.updates)
        {
            ++entries_.find(value.instance.local)->revision;
        }
        written_records_ += dirty.size() * kMeshInstanceDirtySlots;
        co_return TransformApplied{input.sequence, static_cast<std::uint32_t>(input.updates.size())};
    }

    RenderResult<MeshInstanceInformation> MeshInstanceStorage::query(MeshInstanceId id) const noexcept
    {
        const auto* entry = id.storage == id_ ? entries_.find(id.local) : nullptr;
        if (!entry || !entry->revision)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        return MeshInstanceInformation{{id, entry->revision}, describe(*entry)};
    }

    exec::task<RenderResult<Ack>> MeshInstanceStorage::release(MeshInstancePublished input) noexcept
    {
        if (updating_)
        {
            co_return cxx::unexpected(RenderError{kBusy});
        }
        const auto* entry = input.instance.storage == id_ ? entries_.find(input.instance.local) : nullptr;
        if (!entry)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        if (input.revision != entry->revision)
        {
            co_return cxx::unexpected(RenderError{kStorageContract});
        }
        updating_ = true;
        cxx::scope_exit cleanup([&]() noexcept { updating_ = false; });
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        std::optional<CommandBatch> batch;
        for (;;)
        {
            if (stop.stop_requested())
            {
                co_return cxx::unexpected(RenderError{kWorkStopped});
            }
            auto ready = session_.queue().begin();
            if (ready)
            {
                batch.emplace(std::move(*ready));
                break;
            }
            if (ready.error().type != kCapacity)
            {
                co_return cxx::unexpected(ready.error());
            }
            auto completed = co_await session_.queue().whenAvailable();
            if (!completed)
            {
                co_return cxx::unexpected(completed.error());
            }
        }
        const auto slot = input.instance.local.index;
        auto& target = fieldBuffer(slot / kMeshInstancePageSlots, EMeshInstanceBinding::ALIVE);
        const std::uint32_t dead = 0;
        beforeWrite(batch->native());
        vkCmdUpdateBuffer(batch->native(), target.native(), fieldOffset(slot, sizeof(dead)), sizeof(dead), &dead);
        afterWrite(batch->native());
        auto accepted = std::move(*batch).submit();
        if (!accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        notify(*entry, true);
        pages_[slot / kMeshInstancePageSlots]->alive[slot % kMeshInstancePageSlots] = 0;
        entries_.erase(input.instance.local);
        --live_records_;
        co_return Ack{};
    }

    RenderResult<StorageResource> MeshInstanceStorage::resolve(
        const NativeResourcePlan& plan,
        const MeshInstanceBinding& input,
        StorageSubscription& subscription
    ) noexcept
    {
        const auto* description = std::get_if<GraphBufferDesc>(&plan.description);
        if (!description || input.role > EMeshInstanceBinding::ALIVE || pages_.empty())
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        const auto field = static_cast<std::size_t>(input.role);
        auto* buffer = paged_ ? page_table_->root.get() : flat_->fields[field].get();
        if (description->size > buffer->size())
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        subscription.attach(bindings_[field]);
        return StorageResource{buffer, 0, buffer->size()};
    }

    StorageFactory meshInstanceStorageFactory() noexcept
    {
        static const render::MeshInstanceStorage::Ops ops{
            [](const void* value, MeshInstanceId id) noexcept
            { return static_cast<const MeshInstanceStorage*>(value)->query(id); }
        };
        static const StorageBindingOps binding{
            [](void* value, const NativeResourcePlan& plan, const void* argument, StorageSubscription& subscription
            ) noexcept
            {
                return static_cast<MeshInstanceStorage*>(value)
                    ->resolve(plan, *static_cast<const MeshInstanceBinding*>(argument), subscription);
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
