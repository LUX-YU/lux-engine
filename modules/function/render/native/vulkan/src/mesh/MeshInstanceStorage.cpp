#include "Cleanup.hpp"
#include "DirtyRanges.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialStorage.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshInstanceStorage.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshStorage.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>

namespace lux::render::vulkan
{
    struct MeshInstanceStorage::GpuPage final
    {
        gapi::vk::VmaBuffer transforms, properties, alive;

        gapi::vk::VmaBuffer& field(std::size_t index) noexcept
        {
            return index == 0 ? transforms : index == 1 ? properties : alive;
        }
    };

    struct MeshInstanceStorage::Page final
    {
        std::array<TransformMatrix, kMeshInstancePageSlots> transforms{};
        std::array<MeshInstanceGpuProperty, kMeshInstancePageSlots> properties{};
        std::array<std::uint32_t, kMeshInstancePageSlots> alive{};
        std::array<GpuPage, 3> gpu;
    };

    struct MeshInstanceStorage::PageTable final
    {
        struct Leaf final
        {
            gapi::vk::VmaBuffer buffer;
            std::array<MeshInstancePageGpu, kMeshInstanceTableAxis> addresses{};
        };

        gapi::vk::VmaBuffer root;
        std::array<std::uint64_t, kMeshInstanceTableAxis> addresses{};
        std::vector<Leaf> leaves;
        DirtyRanges roots_dirty, pages_dirty;
        std::vector<DirtyWordMask> roots_accepted, pages_accepted;
    };

    // Concrete replica and update data. This private aggregate keeps DirtyRanges out of the
    // installed header; it owns no execution state, identity directory or cleanup scope.
    struct MeshInstanceStorage::FrameData final
    {
        struct Slice final
        {
            GpuPage flat;
            std::unique_ptr<PageTable> table;
            std::uint32_t known_pages = 0;
            std::array<DirtyRanges, 3> dirty;
            std::array<std::vector<DirtyWordMask>, 3> accepted;
            std::vector<DirtyWordMask> selected;
        };

        std::array<Slice, 3> slices;
        std::vector<BufferWrite> writes;
        std::vector<MeshInstanceGpuProperty> properties;
        std::vector<std::uint32_t> heads, links, blocks;

        template <class Record> void order(std::size_t count, std::uint64_t capacity, Record record) noexcept
        {
            heads.resize(
                static_cast<std::size_t>((capacity + kMeshInstanceDirtySlots - 1) / kMeshInstanceDirtySlots),
                UINT32_MAX
            );
            links.resize(count);
            blocks.clear();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const auto block = record(i) / kMeshInstanceDirtySlots;
                if (heads[block] == UINT32_MAX)
                {
                    blocks.push_back(block);
                }
                links[i] = heads[block];
                heads[block] = i;
            }
            std::ranges::sort(blocks); // Copy-range indexes only; business input order is untouched.
        }

        template <class Record, class Visit> void forEachOrdered(Record record, Visit visit) const noexcept
        {
            std::array<std::uint32_t, kMeshInstanceDirtySlots> indices;
            for (auto block : blocks)
            {
                std::array<std::uint64_t, kMeshInstanceDirtySlots / 64> bits{};
                for (auto i = heads[block]; i != UINT32_MAX; i = links[i])
                {
                    const auto local = record(i) % kMeshInstanceDirtySlots;
                    indices[local] = i;
                    bits[local / 64] |= std::uint64_t{1} << (local % 64);
                }
                for (std::uint32_t word = 0; word < bits.size(); ++word)
                {
                    auto valid = bits[word];
                    while (valid)
                    {
                        const auto local = word * 64 + static_cast<unsigned>(std::countr_zero(valid));
                        visit(block * kMeshInstanceDirtySlots + local, indices[local]);
                        valid &= valid - 1;
                    }
                }
            }
        }

        void clearOrder() noexcept
        {
            for (auto block : blocks)
            {
                heads[block] = UINT32_MAX;
            }
            blocks.clear();
            writes.clear();
        }
    };

    namespace
    {
        constexpr std::array<std::size_t, 3> kStride{
            sizeof(TransformMatrix),
            sizeof(MeshInstanceGpuProperty),
            sizeof(std::uint32_t)
        };

        std::span<const std::byte> emptyField(std::size_t field) noexcept
        {
            static constexpr std::array<TransformMatrix, 2> transforms{};
            static constexpr std::array<MeshInstanceGpuProperty, 2> properties{};
            static constexpr std::array<std::uint32_t, 2> alive{};
            if (field == 0)
            {
                return std::as_bytes(std::span(transforms));
            }
            if (field == 1)
            {
                return std::as_bytes(std::span(properties));
            }
            return std::as_bytes(std::span(alive));
        }

        std::uint64_t address(DeviceSession& session, const gapi::vk::VmaBuffer& buffer) noexcept
        {
            VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            info.buffer = buffer.handle();
            return vkGetBufferDeviceAddress(session.nativeDevice(), &info);
        }

        template <class Visit> bool visitRuns(DirtyWordMask mask, Visit visit) noexcept
        {
            auto bits = mask.bits;
            while (bits)
            {
                const auto first = static_cast<unsigned>(std::countr_zero(bits));
                const auto count = static_cast<unsigned>(std::countr_one(bits >> first));
                if (!visit(mask.word_index * 64 + first, count))
                {
                    return false;
                }
                const auto done = count == 64 ? UINT64_MAX : ((std::uint64_t{1} << count) - 1) << first;
                bits &= ~done;
            }
            return true;
        }
    } // namespace

    MeshInstanceStorage::MeshInstanceStorage(DeviceSession& session, StorageId id) noexcept
        : session_(session), id_(id), paged_(session.enabledFeatures().buffer_device_address),
          frames_(std::make_unique<FrameData>())
    {
    }

    void MeshInstanceStorage::release(GpuPage& page) noexcept
    {
        if (!page.transforms)
        {
            return;
        }
        session_.deferRelease(
            [page = std::move(page), allocator = session_.allocator().native()]() mutable noexcept
            {
                page.transforms.release(allocator);
                page.properties.release(allocator);
                page.alive.release(allocator);
            }
        );
    }

    void MeshInstanceStorage::release(PageTable& table) noexcept
    {
        session_.deferRelease(
            [root = std::move(table.root), leaves = std::move(table.leaves), allocator = session_.allocator().native()](
            ) mutable noexcept
            {
                for (auto& leaf : leaves)
                {
                    leaf.buffer.release(allocator);
                }
                root.release(allocator);
            }
        );
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
            for (auto& slice : page->gpu)
            {
                release(slice);
            }
        }
        for (auto& slice : frames_->slices)
        {
            release(slice.flat);
            if (slice.table)
            {
                release(*slice.table);
            }
        }
    }

    std::uint64_t MeshInstanceStorage::capacity() const noexcept
    {
        return std::uint64_t(pages_.size()) * kMeshInstancePageSlots;
    }

    RenderResult<void> MeshInstanceStorage::allocate(GpuPage& output, std::size_t slots) noexcept
    {
        GpuPage candidate;
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                for (std::size_t field = 0; field < kStride.size(); ++field)
                {
                    candidate.field(field).release(session_.allocator().native());
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
            candidate.field(field) = std::move(*buffer);
        }
        output = std::move(candidate);
        rollback.release();
        return {};
    }

    gapi::vk::VmaBuffer& MeshInstanceStorage::fieldBuffer(
        std::uint32_t frame,
        std::uint32_t page,
        std::size_t field
    ) noexcept
    {
        return paged_ ? pages_[page]->gpu[frame].field(field) : frames_->slices[frame].flat.field(field);
    }

    VkDeviceSize MeshInstanceStorage::fieldOffset(std::uint32_t record, std::size_t stride) const noexcept
    {
        return std::uint64_t(paged_ ? record % kMeshInstancePageSlots : record) * stride;
    }

    void MeshInstanceStorage::publishBuffers(std::uint32_t frame) noexcept
    {
        auto& slice = frames_->slices[frame];
        for (std::size_t field = 0; field < bindings_.size(); ++field)
        {
            auto& buffer = paged_ ? slice.table->root : slice.flat.field(field);
            StorageSubscription::publish(bindings_[field], frame, {buffer.handle(), 0, buffer.size()});
        }
    }

    void MeshInstanceStorage::mark(std::uint32_t frame, std::uint32_t record, std::uint8_t fields) noexcept
    {
        auto& slice = frames_->slices[frame];
        for (std::size_t field = 0; field < kStride.size(); ++field)
        {
            if (!(fields & (1u << field)))
            {
                continue;
            }
            slice.dirty[field].grow(capacity());
            slice.dirty[field].mark(record);
        }
    }

    RenderResult<void> MeshInstanceStorage::ensureFrame(std::uint32_t frame) noexcept
    {
        auto& slice = frames_->slices[frame];
        const bool allocated = paged_ ? slice.table && slice.table->root : bool(slice.flat.transforms);
        if (allocated && slice.known_pages == pages_.size())
        {
            return {};
        }
        const auto first_page = slice.known_pages;
        if (!paged_)
        {
            if (slice.flat.transforms.size() < std::max<std::uint64_t>(capacity(), 2) * sizeof(TransformMatrix))
            {
                GpuPage candidate;
                if (auto result = allocate(candidate, std::max<std::uint64_t>(capacity(), 2)); !result)
                {
                    return result;
                }
                release(slice.flat);
                slice.flat = std::move(candidate);
                for (auto& dirty : slice.dirty)
                {
                    dirty.grow(std::max<std::uint64_t>(capacity(), 2));
                    dirty.markRange(0, std::max<std::uint64_t>(capacity(), 2));
                }
                publishBuffers(frame);
            }
        }
        else
        {
            if (!slice.table)
            {
                slice.table = std::make_unique<PageTable>();
            }
            auto& table = *slice.table;
            const auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                               VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            if (!table.root)
            {
                auto root = session_.allocator().createBuffer(sizeof(table.addresses), usage, EMemoryAccess::DEVICE);
                if (!root)
                {
                    return cxx::unexpected(root.error());
                }
                table.root = std::move(*root);
                table.roots_dirty.grow(kMeshInstanceTableAxis);
                table.roots_dirty.markRange(0, kMeshInstanceTableAxis);
                publishBuffers(frame);
            }
            const auto leaf_count = (pages_.size() + kMeshInstanceTableAxis - 1) / kMeshInstanceTableAxis;
            while (table.leaves.size() < leaf_count)
            {
                PageTable::Leaf leaf;
                auto buffer = session_.allocator().createBuffer(sizeof(leaf.addresses), usage, EMemoryAccess::DEVICE);
                if (!buffer)
                {
                    return cxx::unexpected(buffer.error());
                }
                leaf.buffer = std::move(*buffer);
                const auto index = table.leaves.size();
                table.addresses[index] = address(session_, leaf.buffer);
                table.roots_dirty.mark(static_cast<std::uint32_t>(index));
                table.leaves.push_back(std::move(leaf));
                table.pages_dirty.grow(table.leaves.size() * kMeshInstanceTableAxis);
                table.pages_dirty.markRange(index * kMeshInstanceTableAxis, kMeshInstanceTableAxis);
            }
            for (std::uint32_t page = first_page; page < pages_.size(); ++page)
            {
                auto& gpu = pages_[page]->gpu[frame];
                if (!gpu.transforms)
                {
                    if (auto result = allocate(gpu, kMeshInstancePageSlots); !result)
                    {
                        return result;
                    }
                }
                auto& leaf = table.leaves[page / kMeshInstanceTableAxis];
                leaf.addresses[page % kMeshInstanceTableAxis] = {
                    address(session_, gpu.transforms),
                    address(session_, gpu.properties),
                    address(session_, gpu.alive)
                };
                table.pages_dirty.mark(page);
            }
        }
        if (paged_)
        {
            for (auto& dirty : slice.dirty)
            {
                dirty.grow(capacity());
                dirty.markRange(
                    std::uint64_t(first_page) * kMeshInstancePageSlots,
                    (pages_.size() - first_page) * kMeshInstancePageSlots
                );
            }
        }
        slice.known_pages = static_cast<std::uint32_t>(pages_.size());
        return {};
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
        return createBatch(std::move(input));
    }

    exec::task<RenderResult<std::vector<MeshInstanceId>>> MeshInstanceStorage::create(
        std::vector<MeshInstanceDescription> input
    ) noexcept
    {
        return createBatch(std::move(input));
    }

    template <class Input>
    exec::task<RenderResult<MeshInstanceStorage::CreateValue<Input>>> MeshInstanceStorage::createBatch(Input input
    ) noexcept
    {
        if (updating_)
        {
            co_return cxx::unexpected(RenderError{kBusy});
        }
        const auto descriptions = [&]() noexcept -> std::span<const MeshInstanceDescription>
        {
            if constexpr (std::same_as<Input, MeshInstanceDescription>)
            {
                return {&input, 1};
            }
            else
            {
                return input;
            }
        }();
        if (descriptions.size() > UINT32_MAX - live_records_)
        {
            co_return cxx::unexpected(RenderError{kStorageCapacity});
        }
        auto& data = *frames_;
        data.properties.clear();
        data.properties.reserve(descriptions.size());
        for (const auto& description : descriptions)
        {
            auto property = resolveReferences(description);
            if (!property)
            {
                co_return cxx::unexpected(property.error());
            }
            data.properties.push_back(*property);
        }
        using Ids = std::conditional_t<
            std::same_as<Input, MeshInstanceDescription>,
            std::array<MeshInstanceId, 1>,
            std::vector<MeshInstanceId>>;
        Ids ids;
        if constexpr (!std::same_as<Input, MeshInstanceDescription>)
        {
            ids.resize(descriptions.size());
        }
        if (descriptions.empty())
        {
            co_return CreateValue<Input>{};
        }
        updating_ = true;
        std::size_t reserved = 0;
        const auto old_pages = pages_.size();
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                for (std::size_t i = 0; i < reserved; ++i)
                {
                    entries_.erase(ids[i].local);
                }
                for (std::uint32_t frame = 0; frame < session_.framesInFlight(); ++frame)
                {
                    auto& slice = data.slices[frame];
                    // Unpublished slots remain dead in the CPU authority. Any accepted partial writes
                    // are restored before a later Draw; no business state is rolled forward on failure.
                    for (std::size_t i = 0; i < reserved; ++i)
                    {
                        if (ids[i].local.index < old_pages * kMeshInstancePageSlots)
                        {
                            mark(frame, ids[i].local.index, 7);
                        }
                    }
                    if (pages_.size() != old_pages)
                    {
                        for (auto& dirty : slice.dirty)
                        {
                            dirty.acknowledgeRange(
                                old_pages * kMeshInstancePageSlots,
                                (pages_.size() - old_pages) * kMeshInstancePageSlots
                            );
                        }
                        if (slice.table)
                        {
                            auto& table = *slice.table;
                            for (std::size_t page = old_pages;
                                 page < std::min(pages_.size(), table.leaves.size() * kMeshInstanceTableAxis);
                                 ++page)
                            {
                                table.leaves[page / kMeshInstanceTableAxis].addresses[page % kMeshInstanceTableAxis] = {
                                };
                                table.pages_dirty.mark(static_cast<std::uint32_t>(page));
                            }
                        }
                        slice.known_pages = std::min(slice.known_pages, static_cast<std::uint32_t>(old_pages));
                    }
                    if (old_pages == 0)
                    {
                        release(slice.flat);
                        if (slice.table)
                        {
                            release(*slice.table);
                            slice.table.reset();
                        }
                        for (auto& dirty : slice.dirty)
                        {
                            dirty.clear();
                        }
                        for (auto* head : bindings_)
                        {
                            StorageSubscription::publish(head, frame, {});
                        }
                    }
                }
                for (std::size_t page = old_pages; page < pages_.size(); ++page)
                {
                    for (auto& slice : pages_[page]->gpu)
                    {
                        release(slice);
                    }
                }
                pages_.resize(old_pages);
                data.clearOrder();
                updating_ = false;
            }
        );
        for (std::size_t i = 0; i < descriptions.size(); ++i)
        {
            const auto& description = descriptions[i];
            const auto key = entries_.emplace(Entry{description.mesh, description.material, description.submesh});
            ids[i] = {id_, key};
            ++reserved;
            while (pages_.size() <= key.index / kMeshInstancePageSlots)
            {
                pages_.push_back(std::make_unique<Page>());
            }
        }
        // Allocate logical-bit storage before any fallible native preparation so rollback can
        // acknowledge a partially prepared tail without inspecting a stale GPU object.
        for (std::uint32_t frame = 0; frame < session_.framesInFlight(); ++frame)
        {
            for (auto& dirty : data.slices[frame].dirty)
            {
                dirty.grow(capacity());
            }
        }
        std::uint32_t frame = 0;
        while (!session_.uploads().tryBeginFrameWrite(frame))
        {
            auto ready = co_await session_.uploads().whenFrameWritable(
                descriptions.size() *
                (sizeof(TransformMatrix) + sizeof(MeshInstanceGpuProperty) + sizeof(std::uint32_t))
            );
            if (!ready)
            {
                co_return cxx::unexpected(ready.error());
            }
        }
        cxx::scope_exit finish([&]() noexcept { session_.uploads().finishFrameWrite(frame); });
        if (auto prepared = ensureFrame(frame); !prepared)
        {
            co_return cxx::unexpected(prepared.error());
        }
        data.order(descriptions.size(), capacity(), [&](std::size_t i) noexcept { return ids[i].local.index; });
        data.writes.clear();
        data.writes.reserve(descriptions.size() * 3);
        const std::uint32_t alive = 1;
        for (std::size_t field = 0; field < kStride.size(); ++field)
        {
            data.forEachOrdered(
                [&](std::size_t i) noexcept { return ids[i].local.index; },
                [&](std::uint32_t record, std::size_t i) noexcept
                {
                    const auto bytes = field == 0   ? std::as_bytes(std::span(descriptions[i].transform))
                                       : field == 1 ? std::as_bytes(std::span(&data.properties[i], 1))
                                                    : std::as_bytes(std::span(&alive, 1));
                    data.writes.push_back(
                        {&fieldBuffer(frame, record / kMeshInstancePageSlots, field),
                         fieldOffset(record, kStride[field]),
                         bytes}
                    );
                }
            );
        }
        if (auto accepted = co_await session_.uploads().writeBuffers(data.writes, {}, frame); !accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        for (std::size_t i = 0; i < descriptions.size(); ++i)
        {
            const auto record = ids[i].local.index;
            auto& page = *pages_[record / kMeshInstancePageSlots];
            const auto local = record % kMeshInstancePageSlots;
            page.transforms[local] = descriptions[i].transform;
            page.properties[local] = data.properties[i];
            page.alive[local] = 1;
            entries_.find(ids[i].local)->key = ids[i].local;
            for (std::uint32_t other = 0; other < session_.framesInFlight(); ++other)
            {
                if (other != frame)
                {
                    mark(other, record, 7);
                }
            }
            for (auto& dirty : data.slices[frame].dirty)
            {
                dirty.acknowledge({record / 64, std::uint64_t{1} << (record % 64)});
            }
        }
        live_records_ += static_cast<std::uint32_t>(descriptions.size());
        for (const auto id : ids)
        {
            notify(*entries_.find(id.local), false);
        }
        data.clearOrder();
        updating_ = false;
        rollback.release();
        if constexpr (std::same_as<Input, MeshInstanceDescription>)
        {
            co_return ids[0];
        }
        else
        {
            co_return std::move(ids);
        }
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
            if (!entry || entry->key.isNull())
            {
                co_return cxx::unexpected(RenderError{kStorageIdentity});
            }
            const bool invalid_transform =
                !std::ranges::all_of(value.transform, [](float element) { return std::isfinite(element); });
            if (invalid_transform)
            {
                co_return cxx::unexpected(RenderError{kStorageContract});
            }
        }
        if (input.updates.empty())
        {
            co_return TransformApplied{input.sequence, 0};
        }
        updating_ = true;
        auto& data = *frames_;
        cxx::scope_exit cleanup(
            [&]() noexcept
            {
                data.clearOrder();
                updating_ = false;
            }
        );
        std::uint32_t frame = 0;
        while (!session_.uploads().tryBeginFrameWrite(frame))
        {
            auto ready = co_await session_.uploads().whenFrameWritable(input.updates.size() * sizeof(TransformMatrix));
            if (!ready)
            {
                co_return cxx::unexpected(ready.error());
            }
        }
        cxx::scope_exit finish([&]() noexcept { session_.uploads().finishFrameWrite(frame); });
        if (auto prepared = ensureFrame(frame); !prepared)
        {
            co_return cxx::unexpected(prepared.error());
        }
        data.order(
            input.updates.size(),
            capacity(),
            [&](std::size_t i) noexcept { return input.updates[i].instance.local.index; }
        );
        data.forEachOrdered(
            [&](std::size_t i) noexcept { return input.updates[i].instance.local.index; },
            [&](std::uint32_t record, std::size_t i) noexcept
            {
                data.writes.push_back(
                    {&fieldBuffer(frame, record / kMeshInstancePageSlots, 0),
                     fieldOffset(record, kStride[0]),
                     std::as_bytes(std::span(input.updates[i].transform))}
                );
            }
        );
        if (auto accepted = co_await session_.uploads().writeBuffers(data.writes, {}, frame); !accepted)
        {
            for (const auto& value : input.updates)
            {
                mark(frame, value.instance.local.index, 1);
            }
            co_return cxx::unexpected(accepted.error());
        }
        for (const auto& value : input.updates)
        {
            const auto record = value.instance.local.index;
            pages_[record / kMeshInstancePageSlots]->transforms[record % kMeshInstancePageSlots] = value.transform;
            for (std::uint32_t other = 0; other < session_.framesInFlight(); ++other)
            {
                if (other != frame)
                {
                    mark(other, record, 1);
                }
            }
            data.slices[frame].dirty[0].acknowledge({record / 64, std::uint64_t{1} << (record % 64)});
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
        auto& data = *frames_;
        data.properties.clear();
        data.properties.reserve(input.updates.size());
        for (const auto& value : input.updates)
        {
            const auto* entry = value.instance.storage == id_ ? entries_.find(value.instance.local) : nullptr;
            if (!entry || entry->key.isNull())
            {
                co_return cxx::unexpected(RenderError{kStorageIdentity});
            }
            auto property = resolveReferences(value.description);
            if (!property)
            {
                co_return cxx::unexpected(property.error());
            }
            data.properties.push_back(*property);
        }
        if (input.updates.empty())
        {
            co_return TransformApplied{input.sequence, 0};
        }
        updating_ = true;
        cxx::scope_exit cleanup(
            [&]() noexcept
            {
                data.clearOrder();
                updating_ = false;
            }
        );
        std::uint32_t frame = 0;
        while (!session_.uploads().tryBeginFrameWrite(frame))
        {
            auto ready = co_await session_.uploads().whenFrameWritable(
                input.updates.size() * (sizeof(TransformMatrix) + sizeof(MeshInstanceGpuProperty))
            );
            if (!ready)
            {
                co_return cxx::unexpected(ready.error());
            }
        }
        cxx::scope_exit finish([&]() noexcept { session_.uploads().finishFrameWrite(frame); });
        if (auto prepared = ensureFrame(frame); !prepared)
        {
            co_return cxx::unexpected(prepared.error());
        }
        data.order(
            input.updates.size(),
            capacity(),
            [&](std::size_t i) noexcept { return input.updates[i].instance.local.index; }
        );
        for (std::size_t field = 0; field < 2; ++field)
        {
            data.forEachOrdered(
                [&](std::size_t i) noexcept { return input.updates[i].instance.local.index; },
                [&](std::uint32_t record, std::size_t i) noexcept
                {
                    const auto bytes = field == 0 ? std::as_bytes(std::span(input.updates[i].description.transform))
                                                  : std::as_bytes(std::span(&data.properties[i], 1));
                    data.writes.push_back(
                        {&fieldBuffer(frame, record / kMeshInstancePageSlots, field),
                         fieldOffset(record, kStride[field]),
                         bytes}
                    );
                }
            );
        }
        if (auto accepted = co_await session_.uploads().writeBuffers(data.writes, {}, frame); !accepted)
        {
            for (const auto& value : input.updates)
            {
                mark(frame, value.instance.local.index, 3);
            }
            co_return cxx::unexpected(accepted.error());
        }
        for (std::size_t i = 0; i < input.updates.size(); ++i)
        {
            const auto& value = input.updates[i];
            auto& entry = *entries_.find(value.instance.local);
            entry.mesh = value.description.mesh;
            entry.material = value.description.material;
            entry.submesh = value.description.submesh;
            const auto record = value.instance.local.index;
            auto& page = *pages_[record / kMeshInstancePageSlots];
            page.transforms[record % kMeshInstancePageSlots] = value.description.transform;
            page.properties[record % kMeshInstancePageSlots] = data.properties[i];
            for (std::uint32_t other = 0; other < session_.framesInFlight(); ++other)
            {
                if (other != frame)
                {
                    mark(other, record, 3);
                }
            }
            for (std::size_t field = 0; field < 2; ++field)
            {
                data.slices[frame].dirty[field].acknowledge({record / 64, std::uint64_t{1} << (record % 64)});
            }
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
        if (!entry || entry->key.isNull())
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        updating_ = true;
        cxx::scope_exit cleanup([&]() noexcept { updating_ = false; });
        std::uint32_t frame = 0;
        while (!session_.uploads().tryBeginFrameWrite(frame))
        {
            auto ready = co_await session_.uploads().whenFrameWritable(sizeof(std::uint32_t));
            if (!ready)
            {
                co_return cxx::unexpected(ready.error());
            }
        }
        cxx::scope_exit finish([&]() noexcept { session_.uploads().finishFrameWrite(frame); });
        if (auto prepared = ensureFrame(frame); !prepared)
        {
            co_return cxx::unexpected(prepared.error());
        }
        const auto record = input.local.index;
        const std::uint32_t dead = 0;
        const BufferWrite write{
            &fieldBuffer(frame, record / kMeshInstancePageSlots, 2),
            fieldOffset(record, sizeof(dead)),
            std::as_bytes(std::span(&dead, 1))
        };
        if (auto accepted = co_await session_.uploads().writeBuffers(std::span(&write, 1), {}, frame); !accepted)
        {
            mark(frame, record, 4);
            co_return cxx::unexpected(accepted.error());
        }
        notify(*entry, true);
        pages_[record / kMeshInstancePageSlots]->alive[record % kMeshInstancePageSlots] = 0;
        for (std::uint32_t other = 0; other < session_.framesInFlight(); ++other)
        {
            if (other != frame)
            {
                mark(other, record, 4);
            }
        }
        frames_->slices[frame].dirty[2].acknowledge({record / 64, std::uint64_t{1} << (record % 64)});
        entries_.erase(input.local);
        --live_records_;
        co_return Ack{};
    }

    RenderResult<bool> MeshInstanceStorage::prepareFrame(std::uint32_t frame, UploadContext& uploads) noexcept
    {
        if (auto prepared = ensureFrame(frame); !prepared)
        {
            return cxx::unexpected(prepared.error());
        }
        auto& slice = frames_->slices[frame];
        const bool fields_clean = std::ranges::all_of(slice.dirty, [](const auto& dirty) { return dirty.empty(); });
        const bool table_clean = !slice.table || (slice.table->roots_dirty.empty() && slice.table->pages_dirty.empty());
        if (fields_clean && table_clean)
        {
            return true;
        }
        bool ready = true;
        bool copied_any = false;
        // Every source is authoritative CPU data or an address table for this physical slice.
        // The selected masks are bounded by the current staging window, never all live records.
        auto stage = [&](DirtyRanges& dirty, std::vector<DirtyWordMask>& accepted, auto source
                     ) noexcept -> RenderResult<bool>
        {
            accepted.clear();
            dirty.select(uploads.windowRegionLimit(), slice.selected);
            std::uint64_t covered = 0;
            RenderResult<bool> result = true;
            for (auto mask : slice.selected)
            {
                const auto completed = visitRuns(
                    mask,
                    [&](std::uint32_t first, std::uint32_t count) noexcept
                    {
                        auto [write, stride] = source(first, count);
                        auto copied = uploads.prepareBuffer(write, stride);
                        if (!copied)
                        {
                            result = cxx::unexpected(copied.error());
                            return false;
                        }
                        const auto records = static_cast<unsigned>(*copied / stride);
                        if (records)
                        {
                            copied_any = true;
                            const auto bits =
                                records == 64 ? UINT64_MAX : ((std::uint64_t{1} << records) - 1) << (first % 64);
                            accepted.push_back({first / 64, bits});
                            covered += records;
                        }
                        return records == count;
                    }
                );
                if (!completed)
                {
                    break;
                }
            }
            if (!result)
            {
                return result;
            }
            return covered == dirty.recordCount();
        };
        // Register only real work, still before the caller can submit this synchronous segment.
        // This also runs on failure so any already prepared ranges receive the failed acknowledgement.
        cxx::scope_exit register_ack(
            [&]() noexcept
            {
                if (!copied_any)
                {
                    return;
                }
                uploads.afterFrameSubmit(
                    &slice,
                    [](void* value, RenderResult<void> accepted) noexcept
                    {
                        auto& slice = *static_cast<FrameData::Slice*>(value);
                        if (accepted)
                        {
                            for (std::size_t field = 0; field < kStride.size(); ++field)
                            {
                                slice.dirty[field].acknowledge(slice.accepted[field]);
                            }
                            if (slice.table)
                            {
                                slice.table->roots_dirty.acknowledge(slice.table->roots_accepted);
                                slice.table->pages_dirty.acknowledge(slice.table->pages_accepted);
                            }
                        }
                        for (auto& masks : slice.accepted)
                        {
                            masks.clear();
                        }
                        if (slice.table)
                        {
                            slice.table->roots_accepted.clear();
                            slice.table->pages_accepted.clear();
                        }
                    }
                );
            }
        );
        if (slice.table)
        {
            auto& table = *slice.table;
            auto roots = stage(
                table.roots_dirty,
                table.roots_accepted,
                [&](std::uint32_t first, std::uint32_t count) noexcept
                {
                    return std::pair{
                        BufferWrite{
                            &table.root,
                            std::uint64_t(first) * sizeof(std::uint64_t),
                            std::as_bytes(std::span(table.addresses).subspan(first, count))
                        },
                        sizeof(std::uint64_t)
                    };
                }
            );
            if (!roots)
            {
                return cxx::unexpected(roots.error());
            }
            ready &= *roots;
            auto pages = stage(
                table.pages_dirty,
                table.pages_accepted,
                [&](std::uint32_t first, std::uint32_t count) noexcept
                {
                    auto& leaf = table.leaves[first / kMeshInstanceTableAxis];
                    const auto local = first % kMeshInstanceTableAxis;
                    return std::pair{
                        BufferWrite{
                            &leaf.buffer,
                            std::uint64_t(local) * sizeof(MeshInstancePageGpu),
                            std::as_bytes(std::span(leaf.addresses).subspan(local, count))
                        },
                        sizeof(MeshInstancePageGpu)
                    };
                }
            );
            if (!pages)
            {
                return cxx::unexpected(pages.error());
            }
            ready &= *pages;
        }
        for (std::size_t field = 0; field < kStride.size(); ++field)
        {
            auto fields = stage(
                slice.dirty[field],
                slice.accepted[field],
                [&](std::uint32_t first, std::uint32_t count) noexcept
                {
                    const auto local = first % kMeshInstancePageSlots;
                    const auto source = [&]() noexcept -> std::span<const std::byte>
                    {
                        if (pages_.empty())
                        {
                            return emptyField(field);
                        }
                        const auto& page = *pages_[first / kMeshInstancePageSlots];
                        if (field == 0)
                        {
                            return std::as_bytes(std::span(page.transforms));
                        }
                        if (field == 1)
                        {
                            return std::as_bytes(std::span(page.properties));
                        }
                        return std::as_bytes(std::span(page.alive));
                    }();
                    return std::pair{
                        BufferWrite{
                            &fieldBuffer(frame, first / kMeshInstancePageSlots, field),
                            fieldOffset(first, kStride[field]),
                            source.subspan(local * kStride[field], count * kStride[field])
                        },
                        kStride[field]
                    };
                }
            );
            if (!fields)
            {
                return cxx::unexpected(fields.error());
            }
            ready &= *fields;
        }
        return ready;
    }

    RenderResult<void> MeshInstanceStorage::resolve(
        const NativeResourcePlan& plan,
        const MeshInstanceBinding& input,
        StorageSubscription& subscription,
        std::span<StorageResource> output
    ) noexcept
    {
        const auto* description = std::get_if<GraphBufferDesc>(&plan.description);
        if (!description || input.role > EMeshInstanceBinding::ALIVE)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        const auto field = static_cast<std::size_t>(input.role);
        const auto bytes = paged_ ? kMeshInstanceTableAxis * sizeof(std::uint64_t)
                                  : std::max<std::uint64_t>(2, capacity()) * kStride[field];
        if (description->size > bytes)
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        subscription.attach(bindings_[field]);
        for (std::uint32_t frame = 0; frame < output.size(); ++frame)
        {
            auto& slice = frames_->slices[frame];
            const auto handle =
                paged_ ? (slice.table ? slice.table->root.handle() : VK_NULL_HANDLE) : slice.flat.field(field).handle();
            output[frame] = {handle, 0, bytes};
        }
        return {};
    }

    StorageFactory meshInstanceStorageFactory() noexcept
    {
        static const StorageBindingOps binding{
            [](void* value,
               const NativeResourcePlan& plan,
               const void* argument,
               StorageSubscription& subscription,
               std::span<StorageResource> output) noexcept
            {
                return static_cast<MeshInstanceStorage*>(value)
                    ->resolve(plan, *static_cast<const MeshInstanceBinding*>(argument), subscription, output);
            },
            [](void* value, std::uint32_t frame, UploadContext& uploads) noexcept
            { return static_cast<MeshInstanceStorage*>(value)->prepareFrame(frame, uploads); }
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
