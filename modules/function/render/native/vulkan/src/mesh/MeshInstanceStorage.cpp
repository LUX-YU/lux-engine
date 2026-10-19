#include "Cleanup.hpp"
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
        std::array<gapi::vk::VmaBuffer, 3> fields;
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
            gapi::vk::VmaBuffer buffer;
        };

        gapi::vk::VmaBuffer root;

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

        std::uint64_t address(DeviceSession& session, const gapi::vk::VmaBuffer& buffer) noexcept
        {
            VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            info.buffer = buffer.handle();
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
        releaseAfterGpu(session_, update_buffer_);
        if (page_table_)
        {
            session_.deferRelease(
                [table = std::move(page_table_), allocator = session_.allocator().native()]() mutable noexcept
                {
                    for (auto& leaf : table->leaves)
                    {
                        leaf.buffer.release(allocator);
                    }
                    table->root.release(allocator);
                    table.reset();
                }
            );
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
        if (!page)
        {
            return;
        }
        session_.deferRelease(
            [page = std::move(page), allocator = session_.allocator().native()]() mutable noexcept
            {
                for (auto& field : page->fields)
                {
                    field.release(allocator);
                }
                page.reset();
            }
        );
    }

    RenderResult<std::unique_ptr<MeshInstanceStorage::GpuPage>> MeshInstanceStorage::allocate(std::size_t slots
    ) noexcept
    {
        auto result = std::make_unique<GpuPage>();
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                for (auto& field : result->fields)
                {
                    field.release(session_.allocator().native());
                }
            }
        );

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
        rollback.release();
        return result;
    }

    gapi::vk::VmaBuffer& MeshInstanceStorage::fieldBuffer(std::uint32_t page, EMeshInstanceBinding field) const noexcept
    {
        const auto& owner = paged_ ? pages_[page]->gpu : flat_;
        return owner->fields[static_cast<std::size_t>(field)];
    }

    VkDeviceSize MeshInstanceStorage::fieldOffset(std::uint32_t slot, std::size_t stride) const noexcept
    {
        return std::uint64_t(paged_ ? slot % kMeshInstancePageSlots : slot) * stride;
    }

    void MeshInstanceStorage::publishBuffers() noexcept
    {
        for (std::size_t field = 0; field < bindings_.size(); ++field)
        {
            auto* buffer = paged_ ? &page_table_->root : &flat_->fields[field];
            StorageSubscription::publish(bindings_[field], {buffer->handle(), 0, buffer->size()});
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

    RenderResult<MeshInstanceGpuProperty> MeshInstanceStorage::resolveReferences(const MeshInstanceDescription& input
    ) const noexcept
    {
        if (!std::ranges::all_of(input.transform, [](float value) { return std::isfinite(value); }))
        {
            return cxx::unexpected(RenderError{kStorageContract});
        }
        auto& registry = session_.resources();
        for (auto source : {input.mesh.storage, input.material.storage})
        {
            const auto* owner = registry.owner(source);
            if (!owner || (owner != registry.owner(id_) && owner != &session_.globalStorages()))
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
        }
        auto mesh = registry.borrow<vulkan::MeshStorage>(input.mesh.storage);
        auto material = registry.borrow<vulkan::MaterialStorage>(input.material.storage);
        if (!mesh || !material)
        {
            return cxx::unexpected(!mesh ? mesh.error() : material.error());
        }
        const auto* description = (*mesh)->find(input.mesh);
        const auto material_slot = (*material)->gpuIndex(input.material);
        if (!description || !material_slot || input.submesh >= description->submeshes.size())
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        return MeshInstanceGpuProperty{input.mesh.local.index, *material_slot, input.submesh};
    }

    exec::task<RenderResult<MeshInstanceId>> MeshInstanceStorage::create(MeshInstanceDescription input) noexcept
    {
        if (updating_)
        {
            co_return cxx::unexpected(RenderError{kBusy});
        }
        const auto property = resolveReferences(input);
        if (!property)
        {
            co_return cxx::unexpected(property.error());
        }
        const auto key = entries_.emplace(Entry{input.mesh, input.material, input.submesh});
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
                releaseAfterGpu(session_, leaf.buffer);
                if (root)
                {
                    releaseAfterGpu(session_, root->root);
                }
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
                }
            }
        }
        std::uint32_t frame_slot = 0;
        while (!session_.uploads().tryBeginFrameWrite(frame_slot))
        {
            if (auto ready = co_await session_.uploads().whenFrameWritable(
                    sizeof(TransformMatrix) + sizeof(MeshInstanceGpuProperty) + sizeof(std::uint32_t) +
                    sizeof(MeshInstancePageGpu) + sizeof(std::uint64_t)
                );
                !ready)
            {
                co_return cxx::unexpected(ready.error());
            }
        }
        cxx::scope_exit finish_frame([&]() noexcept { session_.uploads().finishFrameWrite(frame_slot); });
        std::array<BufferFill, 5> fills;
        std::array<BufferCopy, 3> copies;
        std::array<BufferWrite, 5> writes;
        std::size_t fill_count = 0, copy_count = 0, write_count = 0;
        if (growing)
        {
            for (auto& field : gpu->fields)
            {
                fills[fill_count++] = {&field, 0, field.size(), 0};
            }
            if (root)
            {
                fills[fill_count++] = {&root->root, 0, root->root.size(), 0};
            }
            if (leaf.buffer)
            {
                fills[fill_count++] = {&leaf.buffer, 0, leaf.buffer.size(), 0};
            }
            if (!paged_ && flat_)
            {
                for (std::size_t f = 0; f < kStride.size(); ++f)
                {
                    copies[copy_count++] = {&flat_->fields[f], &gpu->fields[f], {0, 0, flat_->fields[f].size()}};
                }
            }
        }
        const std::uint32_t alive = 1;
        auto& target = growing ? *gpu : *(paged_ ? pages_[page_index]->gpu : flat_);
        const auto slot = paged_ ? local : key.index;
        writes[write_count++] =
            {&target.fields[0], std::uint64_t(slot) * kStride[0], std::as_bytes(std::span(input.transform))};
        writes[write_count++] =
            {&target.fields[1], std::uint64_t(slot) * kStride[1], std::as_bytes(std::span(&*property, 1))};
        writes[write_count++] =
            {&target.fields[2], std::uint64_t(slot) * kStride[2], std::as_bytes(std::span(&alive, 1))};
        MeshInstancePageGpu addresses{};
        std::uint64_t leaf_address = 0;
        if (growing && paged_)
        {
            auto& table = root ? *root : *page_table_;
            auto& selected = leaf.buffer ? leaf : table.leaves[page_index / kMeshInstanceTableAxis];
            addresses = {
                address(session_, gpu->fields[0]),
                address(session_, gpu->fields[1]),
                address(session_, gpu->fields[2])
            };
            writes[write_count++] = {
                &selected.buffer,
                (page_index % kMeshInstanceTableAxis) * sizeof(addresses),
                std::as_bytes(std::span(&addresses, 1))
            };
            if (leaf.buffer)
            {
                leaf_address = address(session_, leaf.buffer);
                writes[write_count++] = {
                    &table.root,
                    (page_index / kMeshInstanceTableAxis) * sizeof(leaf_address),
                    std::as_bytes(std::span(&leaf_address, 1))
                };
            }
        }
        if (auto accepted = co_await session_.uploads().updateBuffers(
                std::span(writes).first(write_count),
                std::span(copies).first(copy_count),
                std::span(fills).first(fill_count)
            );
            !accepted)
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
        pages_[page_index]->properties[local] = *property;
        pages_[page_index]->alive[local] = 1;
        ++live_records_;
        entries_.find(key)->key = key;
        updating_ = false;
        notify(*entries_.find(key), false);
        rollback.release();
        co_return MeshInstanceId{id_, key};
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
            const auto* entry = value.instance.storage == id_ ? entries_.find(value.instance.local) : nullptr;
            if (!entry)
            {
                co_return cxx::unexpected(RenderError{kStorageIdentity});
            }
            const bool is_invalid_transform =
                !std::ranges::all_of(value.transform, [](float element) { return std::isfinite(element); });
            if (is_invalid_transform)
            {
                co_return cxx::unexpected(RenderError{kStorageContract});
            }
        }
        if (input.updates.empty())
        {
            co_return TransformApplied{input.sequence, 0};
        }
        updating_ = true;

        auto& dirty = dirty_blocks_;
        auto& bytes = update_bytes_;
        dirty.clear();
        bytes.clear();
        constexpr auto block_bytes = sizeof(TransformMatrix) * kMeshInstanceDirtySlots;
        cxx::scope_exit cleanup(
            [&]() noexcept
            {
                for (const auto index : dirty)
                {
                    dirty_positions_[index] = UINT32_MAX;
                }
                updating_ = false;
            }
        );
        // Keep one sparse preparation buffer, distinct from the authoritative 16K-slot CPU pages.
        // Client items are unique. The index sort only merges adjacent GPU copies, not business updates.
        for (const auto& value : input.updates)
        {
            const auto slot = value.instance.local.index;
            const auto index = slot / kMeshInstanceDirtySlots;
            auto& position = dirty_positions_[index];
            if (position == UINT32_MAX)
            {
                position = static_cast<std::uint32_t>(dirty.size());
                dirty.push_back(index);
                bytes.resize(dirty.size() * block_bytes);
                const auto first = (index * kMeshInstanceDirtySlots) % kMeshInstancePageSlots;
                std::memcpy(
                    bytes.data() + std::size_t(position) * block_bytes,
                    pages_[slot / kMeshInstancePageSlots]->transforms.data() + first,
                    block_bytes
                );
            }
            std::memcpy(
                bytes.data() + std::size_t(position) * block_bytes +
                    (slot % kMeshInstanceDirtySlots) * sizeof(TransformMatrix),
                value.transform.data(),
                sizeof(TransformMatrix)
            );
        }
        std::ranges::sort(dirty);
        if (bytes.size() > update_buffer_.size())
        {
            auto buffer = session_.allocator().createBuffer(
                std::bit_ceil(bytes.size()),
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                EMemoryAccess::DEVICE
            );
            if (!buffer)
            {
                co_return cxx::unexpected(buffer.error());
            }
            releaseAfterGpu(session_, update_buffer_);
            update_buffer_ = std::move(*buffer);
        }
        std::uint32_t frame_slot = 0;
        while (!session_.uploads().tryBeginFrameWrite(frame_slot))
        {
            if (auto ready = co_await session_.uploads().whenFrameWritable(bytes.size()); !ready)
            {
                co_return cxx::unexpected(ready.error());
            }
        }
        cxx::scope_exit finish_frame([&]() noexcept { session_.uploads().finishFrameWrite(frame_slot); });
        const BufferWrite write{&update_buffer_, 0, bytes};
        auto uploaded = co_await session_.uploads().writeBuffers(std::span(&write, 1));
        if (!uploaded)
        {
            co_return cxx::unexpected(uploaded.error());
        }
        std::vector<BufferCopy> transfers;
        transfers.reserve(dirty.size());
        for (std::size_t first = 0; first < dirty.size();)
        {
            const auto slot = dirty[first] * kMeshInstanceDirtySlots;
            const auto page_index = slot / kMeshInstancePageSlots;
            auto end = first + 1;
            while (end < dirty.size() && dirty[end] == dirty[end - 1] + 1 &&
                   dirty_positions_[dirty[end]] == dirty_positions_[dirty[end - 1]] + 1 &&
                   (!paged_ || dirty[end] * kMeshInstanceDirtySlots / kMeshInstancePageSlots == page_index))
            {
                ++end;
            }
            auto& target = fieldBuffer(page_index, EMeshInstanceBinding::TRANSFORMS);
            const VkBufferCopy copy{
                std::size_t(dirty_positions_[dirty[first]]) * block_bytes,
                fieldOffset(slot, sizeof(TransformMatrix)),
                (end - first) * block_bytes
            };
            transfers.push_back({&update_buffer_, &target, copy});
            first = end;
        }
        if (auto accepted = co_await session_.uploads().updateBuffers({}, transfers); !accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        for (const auto index : dirty)
        {
            const auto slot = index * kMeshInstanceDirtySlots;
            std::memcpy(
                pages_[slot / kMeshInstancePageSlots]->transforms.data() + slot % kMeshInstancePageSlots,
                bytes.data() + std::size_t(dirty_positions_[index]) * block_bytes,
                block_bytes
            );
        }
        co_return TransformApplied{input.sequence, static_cast<std::uint32_t>(input.updates.size())};
    }

    exec::task<RenderResult<TransformApplied>> MeshInstanceStorage::update(MeshInstanceBatch input) noexcept
    {
        if (updating_)
        {
            co_return cxx::unexpected(RenderError{kBusy});
        }
        if (input.storage != id_ || input.updates.size() > UINT32_MAX)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto& properties = update_properties_;
        properties.clear();
        properties.reserve(input.updates.size());
        for (const auto& value : input.updates)
        {
            const auto* entry = value.instance.storage == id_ ? entries_.find(value.instance.local) : nullptr;
            if (!entry)
            {
                co_return cxx::unexpected(RenderError{kStorageIdentity});
            }
            const auto property = resolveReferences(value.description);
            if (!property)
            {
                co_return cxx::unexpected(property.error());
            }
            properties.push_back(*property);
        }
        if (input.updates.empty())
        {
            co_return TransformApplied{input.sequence, 0};
        }
        updating_ = true;
        cxx::scope_exit cleanup([&]() noexcept { updating_ = false; });
        std::uint32_t frame_slot = 0;
        while (!session_.uploads().tryBeginFrameWrite(frame_slot))
        {
            if (auto ready = co_await session_.uploads().whenFrameWritable(
                    input.updates.size() * (sizeof(TransformMatrix) + sizeof(MeshInstanceGpuProperty))
                );
                !ready)
            {
                co_return cxx::unexpected(ready.error());
            }
        }
        cxx::scope_exit finish_frame([&]() noexcept { session_.uploads().finishFrameWrite(frame_slot); });
        std::vector<BufferWrite> writes;
        writes.reserve(input.updates.size() * 2);
        for (std::size_t i = 0; i < input.updates.size(); ++i)
        {
            const auto& value = input.updates[i];
            const auto slot = value.instance.local.index;
            const auto page = slot / kMeshInstancePageSlots;
            writes.push_back(
                {&fieldBuffer(page, EMeshInstanceBinding::TRANSFORMS),
                 fieldOffset(slot, kStride[0]),
                 std::as_bytes(std::span(value.description.transform))}
            );
            writes.push_back(
                {&fieldBuffer(page, EMeshInstanceBinding::PROPERTIES),
                 fieldOffset(slot, kStride[1]),
                 std::as_bytes(std::span(&properties[i], 1))}
            );
        }
        if (auto accepted = co_await session_.uploads().updateBuffers(writes); !accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        for (std::size_t i = 0; i < input.updates.size(); ++i)
        {
            const auto& value = input.updates[i];
            auto& entry = *entries_.find(value.instance.local);
            entry.mesh = value.description.mesh;
            entry.material = value.description.material;
            entry.submesh = value.description.submesh;
            const auto slot = value.instance.local.index;
            auto& page = *pages_[slot / kMeshInstancePageSlots];
            page.transforms[slot % kMeshInstancePageSlots] = value.description.transform;
            page.properties[slot % kMeshInstancePageSlots] = properties[i];
            notify(entry, false);
        }
        co_return TransformApplied{input.sequence, static_cast<std::uint32_t>(input.updates.size())};
    }

    exec::task<RenderResult<Ack>> MeshInstanceStorage::release(MeshInstanceId input) noexcept
    {
        if (updating_)
        {
            co_return cxx::unexpected(RenderError{kBusy});
        }
        const auto* entry = input.storage == id_ ? entries_.find(input.local) : nullptr;
        if (!entry)
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        updating_ = true;
        cxx::scope_exit cleanup([&]() noexcept { updating_ = false; });
        std::uint32_t frame_slot = 0;
        while (!session_.uploads().tryBeginFrameWrite(frame_slot))
        {
            if (auto ready = co_await session_.uploads().whenFrameWritable(sizeof(std::uint32_t)); !ready)
            {
                co_return cxx::unexpected(ready.error());
            }
        }
        cxx::scope_exit finish_frame([&]() noexcept { session_.uploads().finishFrameWrite(frame_slot); });
        const auto slot = input.local.index;
        auto& target = fieldBuffer(slot / kMeshInstancePageSlots, EMeshInstanceBinding::ALIVE);
        const std::uint32_t dead = 0;
        const BufferWrite write{&target, fieldOffset(slot, sizeof(dead)), std::as_bytes(std::span(&dead, 1))};
        if (auto accepted = co_await session_.uploads().updateBuffers(std::span(&write, 1)); !accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        notify(*entry, true);
        pages_[slot / kMeshInstancePageSlots]->alive[slot % kMeshInstancePageSlots] = 0;
        entries_.erase(input.local);
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
        auto* buffer = paged_ ? &page_table_->root : &flat_->fields[field];
        if (description->size > buffer->size())
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        subscription.attach(bindings_[field]);
        return StorageResource{buffer->handle(), 0, buffer->size()};
    }

    StorageFactory meshInstanceStorageFactory() noexcept
    {
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
            nullptr,
            &binding
        );
    }
} // namespace lux::render::vulkan
