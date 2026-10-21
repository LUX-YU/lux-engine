#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/graph/RenderPlan.hpp>
#include <lux/engine/render/memory/Cleanup.hpp>
#include <lux/engine/render/memory/DirtyRanges.hpp>
#include <lux/engine/render/storage/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/storage/RasterMaterialTableStorage.hpp>
#include <lux/engine/render/storage/RasterMeshInstanceStorage.hpp>
#include <lux/engine/render/upload/ResourceUploader.hpp>

namespace lux::render::detail
{
    struct RasterMeshInstanceStorage::GpuPage final
    {
        nri::Buffer *transforms = nullptr, *properties = nullptr, *alive = nullptr;

        nri::Buffer*& field(std::size_t index) noexcept
        {
            return index == 0 ? transforms : index == 1 ? properties : alive;
        }
    };

    struct RasterMeshInstanceStorage::Page final
    {
        std::array<TransformMatrix, kMeshInstancePageSlots> transforms{};
        std::array<MeshInstanceGpuProperty, kMeshInstancePageSlots> properties{};
        std::array<std::uint32_t, kMeshInstancePageSlots> alive{};
        std::array<GpuPage, 3> gpu;
    };

    struct RasterMeshInstanceStorage::PageTable final
    {
        struct Leaf final
        {
            nri::Buffer* buffer = nullptr;
            std::array<MeshInstancePageGpu, kMeshInstanceTableAxis> addresses{};
        };

        nri::Buffer* root = nullptr;
        std::array<std::uint64_t, kMeshInstanceTableAxis> addresses{};
        std::vector<Leaf> leaves;
        DirtyRanges roots_dirty, pages_dirty;
        std::vector<DirtyWordMask> roots_accepted, pages_accepted;
    };

    // Concrete replica and update data. This private aggregate keeps DirtyRanges out of the
    // installed header; it owns no execution state, identity directory or cleanup scope.
    struct RasterMeshInstanceStorage::FrameData final
    {
        struct Slice final
        {
            GpuPage flat;
            GpuPage pending_flat;
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

        template <class Record, class Visit>
        void forEachOrdered(Record record, Visit visit, std::vector<DirtyWordMask>* masks = nullptr) const noexcept
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
                    if (masks && valid)
                    {
                        masks->push_back({block * (kMeshInstanceDirtySlots / 64) + word, valid});
                    }
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

        std::uint64_t address(DeviceSession& session, const nri::Buffer* buffer) noexcept
        {
            return session.nriCore().GetBufferDeviceAddress(*buffer);
        }

    } // namespace

    RasterMeshInstanceStorage::RasterMeshInstanceStorage(StorageCreateContext& dependencies, StorageId id) noexcept
        : session_(dependencies.device), uploads_(*dependencies.uploads), frame_execution_(*dependencies.frames),
          resources_(dependencies.resources), id_(id), paged_(dependencies.paged_instances),
          frames_(std::make_unique<FrameData>())
    {
    }

    void RasterMeshInstanceStorage::release(GpuPage& page) noexcept
    {
        if (!page.transforms)
        {
            return;
        }
        session_.deferRelease(
            [page = std::exchange(page, {}), destroy = session_.nriCore().DestroyBuffer]() mutable noexcept
            {
                destroy(page.transforms);
                destroy(page.properties);
                destroy(page.alive);
            }
        );
    }

    void RasterMeshInstanceStorage::release(PageTable& table) noexcept
    {
        session_.deferRelease(
            [root = std::exchange(table.root, nullptr),
             leaves = std::move(table.leaves),
             destroy = session_.nriCore().DestroyBuffer]() mutable noexcept
            {
                for (auto& leaf : leaves)
                {
                    destroy(leaf.buffer);
                }
                destroy(root);
            }
        );
    }

    RasterMeshInstanceStorage::~RasterMeshInstanceStorage() noexcept
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
            release(slice.pending_flat);
            if (slice.table)
            {
                release(*slice.table);
            }
        }
    }

    std::uint64_t RasterMeshInstanceStorage::capacity() const noexcept
    {
        return std::uint64_t(pages_.size()) * kMeshInstancePageSlots;
    }

    RenderResult<void> RasterMeshInstanceStorage::allocate(GpuPage& output, std::size_t slots) noexcept
    {
        GpuPage candidate;
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                for (std::size_t field = 0; field < kStride.size(); ++field)
                {
                    session_.nriCore().DestroyBuffer(candidate.field(field));
                }
            }
        );
        const auto usage = nri::BufferUsageBits::SHADER_RESOURCE_STORAGE;
        for (std::size_t field = 0; field < kStride.size(); ++field)
        {
            const auto bytes = slots * kStride[field];
            if (bytes > session_.description().memory.storageBufferMaxRange)
            {
                return cxx::unexpected(RenderError{kStorageCapacity});
            }
            const nri::BufferDesc description{bytes, 4, usage};
            const auto created =
                session_.nriCore()
                    .CreatePlacedBuffer(session_.device(), NriDeviceHeap, description, candidate.field(field));
            if (created != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(created));
            }
        }
        output = std::move(candidate);
        rollback.release();
        return {};
    }

    nri::Buffer* RasterMeshInstanceStorage::fieldBuffer(
        std::uint32_t frame,
        std::uint32_t page,
        std::size_t field
    ) noexcept
    {
        auto& slice = frames_->slices[frame];
        return paged_ ? pages_[page]->gpu[frame].field(field)
                      : (slice.pending_flat.transforms ? slice.pending_flat : slice.flat).field(field);
    }

    std::uint64_t RasterMeshInstanceStorage::fieldOffset(std::uint32_t record, std::size_t stride) const noexcept
    {
        return std::uint64_t(paged_ ? record % kMeshInstancePageSlots : record) * stride;
    }

    void RasterMeshInstanceStorage::publishBuffers(std::uint32_t frame) noexcept
    {
        auto& slice = frames_->slices[frame];
        for (std::size_t field = 0; field < bindings_.size(); ++field)
        {
            auto& buffer = paged_ ? slice.table->root : slice.flat.field(field);
            StorageSubscription::publish(
                bindings_[field],
                frame,
                {buffer, 0, session_.nriCore().GetBufferDesc(*buffer).size}
            );
        }
    }

    void RasterMeshInstanceStorage::mark(std::uint32_t frame, std::uint32_t record, std::uint8_t fields) noexcept
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

    RenderResult<void> RasterMeshInstanceStorage::ensureFrame(std::uint32_t frame) noexcept
    {
        auto& slice = frames_->slices[frame];
        const bool allocated =
            paged_ ? slice.table && slice.table->root : slice.pending_flat.transforms || slice.flat.transforms;
        if (allocated && slice.known_pages == pages_.size())
        {
            return {};
        }
        const auto first_page = slice.known_pages;
        if (!paged_)
        {
            const auto required = std::max<std::uint64_t>(capacity(), 2);
            const auto* buffer = slice.pending_flat.transforms ? slice.pending_flat.transforms : slice.flat.transforms;
            const bool growing =
                !buffer || session_.nriCore().GetBufferDesc(*buffer).size < required * sizeof(TransformMatrix);
            if (growing)
            {
                GpuPage candidate;
                if (auto result = allocate(candidate, std::bit_ceil(required)); !result)
                {
                    return result;
                }
                release(slice.pending_flat);
                slice.pending_flat = std::exchange(candidate, {});
            }
            for (auto& dirty : slice.dirty)
            {
                dirty.grow(required);
                const auto first =
                    growing ? 0 : std::min<std::uint64_t>(required, std::uint64_t(first_page) * kMeshInstancePageSlots);
                dirty.markRange(first, required - first);
            }
        }
        else
        {
            if (!slice.table)
            {
                slice.table = std::make_unique<PageTable>();
            }
            auto& table = *slice.table;
            const auto usage = nri::BufferUsageBits::SHADER_RESOURCE_STORAGE;
            if (!table.root)
            {
                const nri::BufferDesc description{sizeof(table.addresses), 4, usage};
                const auto created =
                    session_.nriCore().CreatePlacedBuffer(session_.device(), NriDeviceHeap, description, table.root);
                if (created != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(created));
                }
                table.roots_dirty.grow(kMeshInstanceTableAxis);
                table.roots_dirty.markRange(0, kMeshInstanceTableAxis);
                publishBuffers(frame);
            }
            const auto leaf_count = (pages_.size() + kMeshInstanceTableAxis - 1) / kMeshInstanceTableAxis;
            while (table.leaves.size() < leaf_count)
            {
                PageTable::Leaf leaf;
                const nri::BufferDesc description{sizeof(leaf.addresses), 4, usage};
                const auto created =
                    session_.nriCore().CreatePlacedBuffer(session_.device(), NriDeviceHeap, description, leaf.buffer);
                if (created != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(created));
                }
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

    RasterMeshInstanceStorage::Observer::~Observer() noexcept
    {
        reset();
    }

    void RasterMeshInstanceStorage::Observer::reset() noexcept
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

    void RasterMeshInstanceStorage::observe(Observer& observer) noexcept
    {
        observer.reset();
        observer.source_ = this;
        observer.position_ = observers_.size();
        observers_.push_back(&observer);
    }

    void RasterMeshInstanceStorage::notify(const Entry& entry, bool removed) noexcept
    {
        const auto description = selection(entry);
        for (auto* observer : observers_)
        {
            observer->changed(observer->receiver, {id_, entry.key}, removed ? nullptr : &description);
        }
    }

    RasterMeshInstanceStorage::Selection RasterMeshInstanceStorage::selection(const Entry& entry) const noexcept
    {
        return {entry.mesh, entry.material, entry.submesh};
    }

    RenderResult<MeshInstanceGpuProperty> RasterMeshInstanceStorage::resolveReferences(
        const MeshInstanceDescription& input
    ) const noexcept
    {
        auto& registry = resources_;
        for (auto source : {input.mesh.storage, input.material.storage})
        {
            const auto* storage_set = registry.storageSet(source);
            if (!storage_set || (storage_set != registry.storageSet(id_) && !storage_set->isGlobal()))
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
        }
        auto mesh = registry.borrow<detail::IndexedTriangleMeshStorage>(input.mesh.storage);
        auto material = registry.borrow<detail::RasterMaterialTableStorage>(input.material.storage);
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

    exec::task<RenderResult<MeshInstanceId>> RasterMeshInstanceStorage::create(MeshInstanceDescription input) noexcept
    {
        return createBatch(std::move(input));
    }

    exec::task<RenderResult<std::vector<MeshInstanceId>>> RasterMeshInstanceStorage::create(
        std::span<const MeshInstanceDescription> input
    ) noexcept
    {
        return createBatch(std::move(input));
    }

    template <class Input>
    exec::task<RenderResult<RasterMeshInstanceStorage::CreateValue<Input>>> RasterMeshInstanceStorage::createBatch(
        Input input
    ) noexcept
    {
        if (updating_)
        {
            co_return cxx::unexpected(RenderError{kDeviceBusy});
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
                for (std::uint32_t frame = 0; frame < frame_execution_.frameCount(); ++frame)
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
                        if (slice.pending_flat.transforms)
                        {
                            release(slice.pending_flat);
                            slice.known_pages = 0;
                        }
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
        for (std::uint32_t frame = 0; frame < frame_execution_.frameCount(); ++frame)
        {
            for (auto& dirty : data.slices[frame].dirty)
            {
                dirty.grow(capacity());
            }
        }
        std::uint32_t frame = 0;
        while (!frame_execution_.tryBeginWrite(frame))
        {
            auto ready = co_await frame_execution_.whenWritable(
                descriptions.size() *
                (sizeof(TransformMatrix) + sizeof(MeshInstanceGpuProperty) + sizeof(std::uint32_t))
            );
            if (!ready)
            {
                co_return cxx::unexpected(ready.error());
            }
        }
        SubmissionFrontiers progress;
        cxx::scope_exit finish([&]() noexcept { frame_execution_.finishWrite(frame, progress); });
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
                        {fieldBuffer(frame, record / kMeshInstancePageSlots, field),
                         fieldOffset(record, kStride[field]),
                         bytes}
                    );
                }
            );
        }
        if (auto accepted = co_await uploads_.writeBuffers(data.writes, {}, frame, &progress); !accepted)
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
            for (std::uint32_t other = 0; other < frame_execution_.frameCount(); ++other)
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

    exec::task<RenderResult<TransformApplied>> RasterMeshInstanceStorage::update(TransformBatch input) noexcept
    {
        if (updating_)
        {
            co_return cxx::unexpected(RenderError{kDeviceBusy});
        }
        const bool invalid_input =
            input.instances.size() != input.transforms.size() || input.instances.size() > UINT32_MAX;
        if (invalid_input)
        {
            co_return cxx::unexpected(RenderError{kStorageContract});
        }
        const auto count = input.instances.size();
        const auto first_record = count ? input.instances.front().index : 0u;
        std::uint64_t next_record = first_record;
        bool contiguous = true;
        for (const auto key : input.instances)
        {
            const auto* entry = entries_.find(key);
            if (!entry || entry->key.isNull())
            {
                co_return cxx::unexpected(RenderError{kStorageIdentity});
            }
            contiguous &= key.index == next_record;
            ++next_record;
        }
        if (count == 0)
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
        const auto range_count =
            contiguous
                ? (first_record % kMeshInstancePageSlots + count + kMeshInstancePageSlots - 1) / kMeshInstancePageSlots
                : count;
        data.writes.reserve(range_count);
        for (std::uint32_t slot = 0; slot < frame_execution_.frameCount(); ++slot)
        {
            data.slices[slot].dirty[0].grow(capacity());
        }
        std::uint32_t frame = 0;
        while (!frame_execution_.tryBeginWrite(frame))
        {
            auto ready = co_await frame_execution_.whenWritable(count * sizeof(TransformMatrix));
            if (!ready)
            {
                co_return cxx::unexpected(ready.error());
            }
        }
        SubmissionFrontiers progress;
        cxx::scope_exit finish([&]() noexcept { frame_execution_.finishWrite(frame, progress); });
        if (auto prepared = ensureFrame(frame); !prepared)
        {
            co_return cxx::unexpected(prepared.error());
        }
        auto& changed = data.slices[frame].selected;
        changed.clear();
        if (contiguous)
        {
            for (std::size_t i = 0; i < count;)
            {
                const auto record = input.instances[i].index;
                const auto length =
                    std::min<std::size_t>(count - i, kMeshInstancePageSlots - record % kMeshInstancePageSlots);
                data.writes.push_back(
                    {fieldBuffer(frame, record / kMeshInstancePageSlots, 0),
                     fieldOffset(record, kStride[0]),
                     std::as_bytes(input.transforms.subspan(i, length))}
                );
                i += length;
            }
        }
        else
        {
            data.order(count, capacity(), [&](std::size_t i) noexcept { return input.instances[i].index; });
            data.forEachOrdered(
                [&](std::size_t i) noexcept { return input.instances[i].index; },
                [&](std::uint32_t record, std::size_t i) noexcept
                {
                    data.writes.push_back(
                        {fieldBuffer(frame, record / kMeshInstancePageSlots, 0),
                         fieldOffset(record, kStride[0]),
                         std::as_bytes(input.transforms.subspan(i, 1))}
                    );
                },
                &changed
            );
        }
        const auto mark_changed = [&](DirtyRanges& dirty) noexcept
        {
            if (contiguous)
            {
                dirty.markRange(first_record, count);
            }
            else
            {
                for (auto mask : changed)
                {
                    dirty.mark(mask);
                }
            }
        };
        auto accepted = co_await uploads_.writeBuffers(data.writes, {}, frame, &progress);
        if (!accepted)
        {
            // The adopted CPU page is unchanged. Repair all affected records before this slice can Draw.
            mark_changed(data.slices[frame].dirty[0]);
            co_return cxx::unexpected(accepted.error());
        }
        if (contiguous)
        {
            for (std::size_t i = 0; i < count;)
            {
                const auto record = input.instances[i].index;
                const auto local = record % kMeshInstancePageSlots;
                const auto length = std::min<std::size_t>(count - i, kMeshInstancePageSlots - local);
                std::memcpy(
                    pages_[record / kMeshInstancePageSlots]->transforms.data() + local,
                    input.transforms.data() + i,
                    length * sizeof(TransformMatrix)
                );
                i += length;
            }
        }
        else
        {
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto record = input.instances[i].index;
                pages_[record / kMeshInstancePageSlots]->transforms[record % kMeshInstancePageSlots] =
                    input.transforms[i];
            }
        }
        for (std::uint32_t other = 0; other < frame_execution_.frameCount(); ++other)
        {
            auto& dirty = data.slices[other].dirty[0];
            if (other != frame)
            {
                mark_changed(dirty);
            }
            else if (contiguous)
            {
                dirty.acknowledgeRange(first_record, count);
            }
            else
            {
                dirty.acknowledge(changed);
            }
        }
        co_return TransformApplied{input.sequence, static_cast<std::uint32_t>(count)};
    }

    exec::task<RenderResult<TransformApplied>> RasterMeshInstanceStorage::update(MeshInstanceBatch input) noexcept
    {
        if (updating_)
        {
            co_return cxx::unexpected(RenderError{kDeviceBusy});
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
        while (!frame_execution_.tryBeginWrite(frame))
        {
            auto ready = co_await frame_execution_.whenWritable(
                input.updates.size() * (sizeof(TransformMatrix) + sizeof(MeshInstanceGpuProperty))
            );
            if (!ready)
            {
                co_return cxx::unexpected(ready.error());
            }
        }
        SubmissionFrontiers progress;
        cxx::scope_exit finish([&]() noexcept { frame_execution_.finishWrite(frame, progress); });
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
                        {fieldBuffer(frame, record / kMeshInstancePageSlots, field),
                         fieldOffset(record, kStride[field]),
                         bytes}
                    );
                }
            );
        }
        if (auto accepted = co_await uploads_.writeBuffers(data.writes, {}, frame, &progress); !accepted)
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
            for (std::uint32_t other = 0; other < frame_execution_.frameCount(); ++other)
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

    exec::task<RenderResult<Ack>> RasterMeshInstanceStorage::release(MeshInstanceId input) noexcept
    {
        if (updating_)
        {
            co_return cxx::unexpected(RenderError{kDeviceBusy});
        }
        const auto* entry = input.storage == id_ ? entries_.find(input.local) : nullptr;
        if (!entry || entry->key.isNull())
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        updating_ = true;
        cxx::scope_exit cleanup([&]() noexcept { updating_ = false; });
        std::uint32_t frame = 0;
        while (!frame_execution_.tryBeginWrite(frame))
        {
            auto ready = co_await frame_execution_.whenWritable(sizeof(std::uint32_t));
            if (!ready)
            {
                co_return cxx::unexpected(ready.error());
            }
        }
        SubmissionFrontiers progress;
        cxx::scope_exit finish([&]() noexcept { frame_execution_.finishWrite(frame, progress); });
        if (auto prepared = ensureFrame(frame); !prepared)
        {
            co_return cxx::unexpected(prepared.error());
        }
        const auto record = input.local.index;
        const std::uint32_t dead = 0;
        const BufferWrite write{
            fieldBuffer(frame, record / kMeshInstancePageSlots, 2),
            fieldOffset(record, sizeof(dead)),
            std::as_bytes(std::span(&dead, 1))
        };
        if (auto accepted = co_await uploads_.writeBuffers(std::span(&write, 1), {}, frame, &progress); !accepted)
        {
            mark(frame, record, 4);
            co_return cxx::unexpected(accepted.error());
        }
        notify(*entry, true);
        pages_[record / kMeshInstancePageSlots]->alive[record % kMeshInstancePageSlots] = 0;
        for (std::uint32_t other = 0; other < frame_execution_.frameCount(); ++other)
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

    RenderResult<bool> RasterMeshInstanceStorage::prepareFrame(std::uint32_t frame, ResourceUploader& uploads) noexcept
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
            adoptFrameBuffers(frame);
            return true;
        }
        bool ready = true;
        bool copied_any = false;
        // Every source is authoritative CPU data or an address table for this physical slice.
        // The selected masks are bounded by the current staging window, never all live records.
        auto stage = [&](DirtyRanges& dirty,
                         std::vector<DirtyWordMask>& accepted,
                         std::size_t stride,
                         std::uint32_t page_records,
                         auto source) noexcept -> RenderResult<bool>
        {
            accepted.clear();
            const auto word_limit = (uploads.windowByteLimit() / stride + 63) / 64 + uploads.windowRegionLimit();
            dirty.select(word_limit, slice.selected);
            std::uint64_t covered = 0;
            RenderResult<void> result;
            const bool complete = DirtyRanges::visitRanges(
                slice.selected,
                page_records,
                [&](std::uint32_t first, std::uint32_t count) noexcept
                {
                    while (count)
                    {
                        auto [write, unit] = source(first, count);
                        auto copied = uploads.stageBuffer(write, unit);
                        if (!copied)
                        {
                            result = cxx::unexpected(copied.error());
                            return false;
                        }
                        const auto records = static_cast<std::uint32_t>(*copied / stride);
                        if (records == 0)
                        {
                            return false;
                        }
                        copied_any = true;
                        DirtyRanges::appendRange(accepted, first, records);
                        covered += records;
                        first += records;
                        count -= records;
                    }
                    return true;
                }
            );
            if (!result)
            {
                return cxx::unexpected(result.error());
            }
            return complete && covered == dirty.recordCount();
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
                frame_execution_.afterSubmit(
                    this,
                    [](void* value, std::uint32_t frame, RenderResult<void> accepted) noexcept
                    {
                        auto& storage = *static_cast<RasterMeshInstanceStorage*>(value);
                        storage.finishFramePreparation(frame, accepted);
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
                sizeof(std::uint64_t),
                static_cast<std::uint32_t>(table.addresses.size()),
                [&](std::uint32_t first, std::uint32_t count) noexcept
                {
                    return std::pair{
                        BufferWrite{
                            table.root,
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
                sizeof(MeshInstancePageGpu),
                kMeshInstanceTableAxis,
                [&](std::uint32_t first, std::uint32_t count) noexcept
                {
                    auto& leaf = table.leaves[first / kMeshInstanceTableAxis];
                    const auto local = first % kMeshInstanceTableAxis;
                    return std::pair{
                        BufferWrite{
                            leaf.buffer,
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
                kStride[field],
                kMeshInstancePageSlots,
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
                            fieldBuffer(frame, first / kMeshInstancePageSlots, field),
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
        // A replacement is published after this Copy submission is accepted. Its first Draw
        // is recorded on the next pass through preparation, without a CPU/GPU wait.
        return ready && !slice.pending_flat.transforms;
    }

    void RasterMeshInstanceStorage::adoptFrameBuffers(std::uint32_t frame) noexcept
    {
        auto& slice = frames_->slices[frame];
        if (!slice.pending_flat.transforms)
        {
            return;
        }
        release(slice.flat);
        slice.flat = std::exchange(slice.pending_flat, {});
        publishBuffers(frame);
    }

    void RasterMeshInstanceStorage::finishFramePreparation(std::uint32_t frame, RenderResult<void> accepted) noexcept
    {
        auto& slice = frames_->slices[frame];
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
            if (std::ranges::all_of(slice.dirty, [](const auto& dirty) { return dirty.empty(); }))
            {
                adoptFrameBuffers(frame);
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

    RenderResult<void> RasterMeshInstanceStorage::resolve(
        const CompiledResource& plan,
        const MeshInstanceBinding& input,
        StorageSubscription& subscription,
        std::span<ResourceInput> output
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
            const auto handle = paged_ ? (slice.table ? slice.table->root : nullptr) : slice.flat.field(field);
            output[frame] = {handle, 0, handle ? session_.nriCore().GetBufferDesc(*handle).size : bytes};
        }
        return {};
    }

    StorageFactory meshInstanceStorageFactory() noexcept
    {
        static const StorageBindingOps binding{
            [](void* value,
               const CompiledResource& plan,
               const void* argument,
               StorageSubscription& subscription,
               std::span<ResourceInput> output) noexcept
            {
                return static_cast<RasterMeshInstanceStorage*>(value)
                    ->resolve(plan, *static_cast<const MeshInstanceBinding*>(argument), subscription, output);
            },
            [](void* value, std::uint32_t frame, ResourceUploader& uploads) noexcept
            { return static_cast<RasterMeshInstanceStorage*>(value)->prepareFrame(frame, uploads); }
        };
        static const auto create = +[](StorageCreateContext& dependencies, StorageId id
                                    ) noexcept -> RenderResult<std::unique_ptr<RasterMeshInstanceStorage>>
        {
            if (!dependencies.uploads || !dependencies.frames)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            return std::make_unique<RasterMeshInstanceStorage>(dependencies, id);
        };
        return makeStorageFactory<
            render::RasterMeshInstanceStorageDescriptor,
            RasterMeshInstanceStorage,
            StorageCreateContext,
            create>(nullptr, &binding);
    }
} // namespace lux::render::detail
