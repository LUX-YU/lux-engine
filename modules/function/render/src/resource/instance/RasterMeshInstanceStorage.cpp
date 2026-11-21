#include <lux/engine/render/runtime/FrameProgress.hpp>
#include <lux/engine/render/extensions/upload/Preparation.hpp>
#include <lux/engine/render/extensions/FrameDataCall.hpp>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <set>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <lux/engine/render/graph/RenderGraphExecutable.hpp>
#include <lux/engine/render/memory/Cleanup.hpp>
#include <lux/engine/render/memory/DirtyRanges.hpp>
#include <lux/engine/render/resource/ResourceSet.hpp>
#include <lux/engine/render/resource/mesh/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/resource/material/RasterMaterialStorage.hpp>
#include <lux/engine/render/resource/instance/RasterMeshInstanceStorage.hpp>
#include <lux/engine/render/resource/transform/TransformStorage.hpp>

namespace lux::render::detail
{
    namespace
    {
        constexpr std::uint32_t kRenderEnabledBit = 1;

        exec::task<InstanceCommands::CreateInstance::Response>
        createInstance(RasterMeshInstanceStorage& resource, InstanceCommands::CreateInstance::Request&& input) noexcept
        {
            auto result = co_await resource.create(std::move(input.data));
            co_return result ? InstanceCommands::CreateInstance::Response{{}, *result}
                             : InstanceCommands::CreateInstance::Response{result.error()};
        }

        exec::task<InstanceCommands::CreateInstances::Response>
        createInstances(
            RasterMeshInstanceStorage& resource,
            InstanceCommands::CreateInstances::Request&& input
        ) noexcept
        {
            auto result = co_await resource.create(input.data);
            co_return result ? InstanceCommands::CreateInstances::Response{{}, std::move(*result)}
                             : InstanceCommands::CreateInstances::Response{result.error()};
        }

        exec::task<InstanceCommands::ReleaseInstance::Response>
        releaseInstance(
            RasterMeshInstanceStorage& resource,
            InstanceCommands::ReleaseInstance::Request&& input
        ) noexcept
        {
            auto result = co_await resource.release(input.data);
            co_return InstanceCommands::ReleaseInstance::Response{result ? RenderError{} : result.error()};
        }

        RenderResult<void>
        applyInputs(RasterMeshInstanceStorage& resource, std::span<const MeshInstanceUpdate> instances) noexcept
        {
            return resource.update(instances);
        }

        void updateInstances(
            RasterMeshInstanceStorage& resource,
            const InstanceCommands::Update::Request& input,
            AckResponse& response
        ) noexcept
        {
            const auto result = applyInputs(resource, input.instances);
            response.error = result ? RenderError{} : result.error();
        }

    } // namespace

    static_assert(offsetof(MeshInstanceGpuProperty, material_buffer) == 4);
    static_assert(offsetof(MeshInstanceGpuProperty, material_record) == 8);
    static_assert(offsetof(MeshInstanceGpuProperty, section_index) == 12);
    static_assert(offsetof(MeshInstanceGpuProperty, render_flags) == 16);
    static_assert(offsetof(MeshInstanceGpuProperty, transform_record) == 20);

    struct RasterMeshInstanceStorage::GpuPage final
    {
        nri::Buffer *properties = nullptr, *alive = nullptr;

        nri::Buffer*& field(std::size_t index) noexcept
        {
            return index == 0 ? properties : alive;
        }
    };

    struct RasterMeshInstanceStorage::Page final
    {
        std::array<MeshInstanceGpuProperty, kMeshInstancePageSlots> properties{};
        // Sole CPU authority for resource identities; GPU property addresses are projected per FIF.
        std::array<InstanceRecord, kMeshInstancePageSlots> records{};
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
            std::array<std::vector<nri::Buffer*>, 2> addressed_buffers;
            std::uint64_t addressed_revision = 0;
            std::vector<nri::Buffer*> root_dependencies;
            std::uint64_t root_revision = 0;
            bool root_published = false;
            std::array<DirtyRanges, 2> dirty;
            std::array<std::vector<DirtyWordMask>, 2> accepted;
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
        constexpr std::array<std::size_t, 2> kStride{
            sizeof(MeshInstanceGpuProperty),
            sizeof(std::uint32_t)
        };

        std::span<const std::byte> emptyField(std::size_t field) noexcept
        {
            static constexpr std::array<MeshInstanceGpuProperty, 2> properties{};
            static constexpr std::array<std::uint32_t, 2> alive{};
            if (field == 0)
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

    RasterMeshInstanceStorage::RasterMeshInstanceStorage(
        ResourceCreateContext& dependencies, ResourceInstanceId id, TransformStorage& transforms
    ) noexcept
        : session_(dependencies.device), uploads_(*dependencies.uploads),
          replica_progress_(*dependencies.replica_progress), frame_count_(dependencies.frame_count),
          resources_(dependencies.resources), transforms_(transforms), id_(id),
          paged_(dependencies.device.usesDeviceAddresses()),
          frames_(std::make_unique<FrameData>())
    {
    }

    void RasterMeshInstanceStorage::release(GpuPage& page) noexcept
    {
        if (!page.properties)
        {
            return;
        }
        session_.deferRelease(
            [page = std::exchange(page, {}), destroy = session_.nriCore().DestroyBuffer]() mutable noexcept
            {
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
        StorageSubscription::disconnect(paged_bindings_);
        for (auto& head : bindings_)
        {
            StorageSubscription::disconnect(head);
        }
        for (const auto& entry : entries_)
        {
            if (!entry.key.isNull()) transforms_.releaseInstance(instanceRecord(entry).transform);
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
        const auto usage = (nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE);
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

    nri::Buffer*
    RasterMeshInstanceStorage::fieldBuffer(std::uint32_t frame, std::uint32_t page, std::size_t field) noexcept
    {
        auto& slice = frames_->slices[frame];
        return paged_ ? pages_[page]->gpu[frame].field(field)
                      : (slice.pending_flat.properties ? slice.pending_flat : slice.flat).field(field);
    }

    std::uint64_t RasterMeshInstanceStorage::fieldOffset(std::uint32_t record, std::size_t stride) const noexcept
    {
        return std::uint64_t(paged_ ? record % kMeshInstancePageSlots : record) * stride;
    }

    void RasterMeshInstanceStorage::publishBuffers(std::uint32_t frame) noexcept
    {
        auto& slice = frames_->slices[frame];
        if (paged_ && slice.addressed_buffers[0].size() != slice.table->leaves.size() + slice.known_pages)
        {
            for (std::size_t field = 0; field < bindings_.size(); ++field)
            {
                auto& buffers = slice.addressed_buffers[field];
                buffers.clear();
                for (const auto& leaf : slice.table->leaves)
                {
                    buffers.push_back(leaf.buffer);
                }
                for (std::uint32_t page = 0; page < slice.known_pages; ++page)
                {
                    buffers.push_back(pages_[page]->gpu[frame].field(field));
                }
            }
            ++slice.addressed_revision;
        }
        for (std::size_t field = 0; field < bindings_.size(); ++field)
        {
            auto& buffer = paged_ ? slice.table->root : slice.flat.field(field);
            StorageSubscription::publish(
                bindings_[field],
                frame,
                {buffer,
                 0,
                 session_.nriCore().GetBufferDesc(*buffer).size,
                 nullptr,
                 slice.addressed_buffers[field],
                 slice.addressed_revision}
            );
        }
        if (paged_bindings_) StorageSubscription::publish(paged_bindings_, frame, pagedRoot(frame));
    }

    ResourceInput RasterMeshInstanceStorage::pagedRoot(std::uint32_t frame) noexcept
    {
        auto& slice = frames_->slices[frame];
        if (!slice.root_published)
            return {nullptr, 0, meshInstanceProjectionBytes(EMeshInstanceBinding::PAGED_ROOT)};
        if (slice.root_revision != slice.addressed_revision)
        {
            auto& dependencies = slice.root_dependencies;
            dependencies.assign(slice.addressed_buffers[0].begin(), slice.addressed_buffers[0].end());
            const auto alive = std::span{slice.addressed_buffers[1]}.last(slice.known_pages);
            dependencies.insert(dependencies.end(), alive.begin(), alive.end());
            slice.root_revision = slice.addressed_revision;
        }
        auto* root = slice.table->root;
        return {
            root, 0, session_.nriCore().GetBufferDesc(*root).size, nullptr,
            slice.root_dependencies, slice.addressed_revision
        };
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
            paged_ ? slice.table && slice.table->root : slice.pending_flat.properties || slice.flat.properties;
        if (allocated && slice.known_pages == pages_.size())
        {
            return {};
        }
        const auto first_page = slice.known_pages;
        if (!paged_)
        {
            const auto required = std::max<std::uint64_t>(capacity(), 2);
            const auto* buffer = slice.pending_flat.properties ? slice.pending_flat.properties : slice.flat.properties;
            const bool growing =
                !buffer || session_.nriCore().GetBufferDesc(*buffer).size < required * sizeof(MeshInstanceGpuProperty);
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
            const auto usage = (nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE);
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
                if (!gpu.properties)
                {
                    if (auto result = allocate(gpu, kMeshInstancePageSlots); !result)
                    {
                        return result;
                    }
                }
                auto& leaf = table.leaves[page / kMeshInstanceTableAxis];
                leaf.addresses[page % kMeshInstanceTableAxis] = {
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

    void RasterMeshInstanceStorage::invalidateSelection() noexcept
    {
        if (selection_revision_ == UINT64_MAX) std::terminate();
        ++selection_revision_;
    }

    const RasterMeshInstanceStorage::InstanceRecord&
    RasterMeshInstanceStorage::instanceRecord(const Entry& entry) const noexcept
    {
        const auto index = entry.key.index;
        return pages_[index / kMeshInstancePageSlots]->records[index % kMeshInstancePageSlots];
    }

    RenderResult<std::uint64_t>
    RasterMeshInstanceStorage::captureMeshSelections(MeshInstanceSnapshot& output) const noexcept
    {
        if (output.revision == selection_revision_) return output.revision;
        output.instances.clear();
        output.instances.reserve(live_records_);
        output.sections.clear();
        std::set<std::pair<MeshId, std::uint32_t>> sections;
        for (const auto& entry : entries_)
        {
            if (entry.key.isNull()) continue;
            const auto item = instanceRecord(entry);
            const auto id = extension_detail::ResourceIdentity::make<MeshInstanceId>(id_, entry.key);
            const auto owner = extension_detail::ResourceIdentity::parent(item.material);
            const auto storage = resources_.borrow<RasterMaterialStorage>(owner);
            if (!storage) return cxx::unexpected(storage.error());
            const auto program = (*storage)->programId(item.material);
            if (program.isNull()) return cxx::unexpected(RenderError{kStorageIdentity});
            const auto index = entry.key.index;
            const auto& property = pages_[index / kMeshInstancePageSlots]->properties[index % kMeshInstancePageSlots];
            const bool render_enabled = (property.render_flags & kRenderEnabledBit) != 0;
            output.instances.push_back({id, item.mesh, item.material, item.submesh, program, render_enabled});
            sections.emplace(item.mesh, item.submesh);
        }
        output.sections.reserve(sections.size());
        for (const auto& [mesh, submesh] : sections)
        {
            const auto owner = extension_detail::ResourceIdentity::parent(mesh);
            const auto storage = resources_.borrow<IndexedTriangleMeshStorage>(owner);
            if (!storage) return cxx::unexpected(storage.error());
            const auto geometry = (*storage)->arenaSection(mesh, submesh);
            if (!geometry) return cxx::unexpected(geometry.error());
            output.sections.push_back({mesh, submesh, *geometry});
        }
        output.revision = selection_revision_;
        return output.revision;
    }

    RenderResult<MeshInstanceGpuProperty>
    RasterMeshInstanceStorage::resolveReferences(const MeshInstanceData& input, std::uint32_t frame) const noexcept
    {
        auto& registry = resources_;
        for (auto source :
             {extension_detail::ResourceIdentity::parent(input.mesh),
              extension_detail::ResourceIdentity::parent(input.material)})
        {
            if (!registry.canReference(id_, source))
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
        }
        auto mesh =
            registry.borrow<detail::IndexedTriangleMeshStorage>(extension_detail::ResourceIdentity::parent(input.mesh)
            );
        auto material = registry.borrow<detail::RasterMaterialStorage>(
            extension_detail::ResourceIdentity::parent(input.material)
        );
        if (!mesh || !material)
        {
            return cxx::unexpected(!mesh ? mesh.error() : material.error());
        }
        const auto* description = (*mesh)->find(input.mesh);
        const auto material_slot = (*material)->gpuReference(input.material, frame);
        const auto transform_slot = transforms_.gpuSlot(input.transform_id);
        const bool invalid_mesh = !description || input.submesh >= description->submeshes.size();
        const bool invalid_reference = invalid_mesh || !material_slot || !transform_slot;
        if (invalid_reference)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        return MeshInstanceGpuProperty{
            extension_detail::ResourceIdentity::local(input.mesh).index,
            material_slot->buffer_index,
            material_slot->record_index,
            input.submesh,
            (input.render_enabled ? kRenderEnabledBit : 0u) | (input.cast_shadow ? 2u : 0u),
            *transform_slot
        };
    }

    exec::task<RenderResult<MeshInstanceId>> RasterMeshInstanceStorage::create(MeshInstanceData input) noexcept
    {
        return createBatch(std::move(input));
    }

    exec::task<RenderResult<std::vector<MeshInstanceId>>>
    RasterMeshInstanceStorage::create(std::span<const MeshInstanceData> input) noexcept
    {
        return createBatch(std::move(input));
    }

    template <class Input>
    exec::task<RenderResult<RasterMeshInstanceStorage::CreateValue<Input>>>
    RasterMeshInstanceStorage::createBatch(Input input) noexcept
    {
        const auto stop = co_await stdexec::read_env(stdexec::get_stop_token);
        if (stop.stop_requested()) co_return cxx::unexpected(RenderError{kUploadStopped});
        if (updating_)
        {
            co_return cxx::unexpected(RenderError{kDeviceBusy});
        }
        const auto descriptions = [&]() noexcept -> std::span<const MeshInstanceData>
        {
            if constexpr (std::same_as<Input, MeshInstanceData>)
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
        using Ids = std::conditional_t<
            std::same_as<Input, MeshInstanceData>,
            std::array<MeshInstanceId, 1>,
            std::vector<MeshInstanceId>>;
        Ids ids;
        if constexpr (!std::same_as<Input, MeshInstanceData>)
        {
            ids.resize(descriptions.size());
        }
        if (descriptions.empty())
        {
            co_return CreateValue<Input>{};
        }
        // The batch command borrows its input. Select owned records before releasing the owner.
        std::vector<MeshInstanceData> selected_descriptions;
        auto selected = descriptions;
        if constexpr (!std::same_as<Input, MeshInstanceData>)
        {
            selected_descriptions.assign(descriptions.begin(), descriptions.end());
            selected = selected_descriptions;
        }
        for (const auto& description : selected)
        {
            if (!transforms_.gpuSlot(description.transform_id))
                co_return cxx::unexpected(RenderError{kStorageIdentity});
        }

        auto& data = *frames_;
        auto properties = std::move(data.properties);
        auto writes = std::move(data.writes);
        cxx::scope_exit recycle_upload([&]() noexcept
        {
            properties.clear();
            writes.clear();
            data.properties = std::move(properties);
            data.writes = std::move(writes);
        });
        properties.clear();
        properties.reserve(selected.size());
        updating_ = true;
        ++preparation_revision_;
        std::size_t reserved = 0;
        const auto old_pages = pages_.size();
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                for (std::size_t i = 0; i < reserved; ++i)
                {
                    const auto key = extension_detail::ResourceIdentity::local(ids[i]);
                    transforms_.releaseInstance(selected[i].transform_id);
                    entries_.erase(key);
                }
                for (std::uint32_t frame = 0; frame < frame_count_; ++frame)
                {
                    auto& slice = data.slices[frame];
                    // Unpublished slots remain dead in the CPU authority. Any accepted partial writes
                    // are restored before a later Draw; no business state is rolled forward on failure.
                    for (std::size_t i = 0; i < reserved; ++i)
                    {
                        if (extension_detail::ResourceIdentity::local(ids[i]).index <
                            old_pages * kMeshInstancePageSlots)
                        {
                            mark(frame, extension_detail::ResourceIdentity::local(ids[i]).index, 3);
                        }
                    }
                    if (pages_.size() != old_pages)
                    {
                        if (slice.pending_flat.properties)
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
                        for (auto& buffers : slice.addressed_buffers) buffers.clear();
                        slice.root_dependencies.clear();
                        slice.root_published = false;
                        ++slice.addressed_revision;
                        StorageSubscription::publish(paged_bindings_, frame, {});
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
            const auto key = entries_.emplace();
            ids[i] = extension_detail::ResourceIdentity::make<MeshInstanceId>(id_, key);
            ++reserved;
            while (pages_.size() <= key.index / kMeshInstancePageSlots)
            {
                pages_.push_back(std::make_unique<Page>());
            }
            transforms_.retainInstance(selected[i].transform_id);
        }
        // Allocate logical-bit storage before any fallible native preparation so rollback can
        // acknowledge a partially prepared tail without inspecting a stale GPU object.
        for (std::uint32_t frame = 0; frame < frame_count_; ++frame)
        {
            for (auto& dirty : data.slices[frame].dirty)
            {
                dirty.grow(capacity());
            }
        }
        auto replica = co_await replica_progress_.acquireReplica(session_);
        if (!replica) co_return cxx::unexpected(replica.error());
        cxx::scope_exit end_replica([&]() noexcept { replica_progress_.endReplica(*replica); });
        const auto frame = replica->frame.slot_index;
        for (const auto& description : selected)
        {
            auto property = resolveReferences(description, frame);
            if (!property)
            {
                co_return cxx::unexpected(property.error());
            }
            properties.push_back(*property);
        }
        if (auto prepared = ensureFrame(frame); !prepared)
        {
            co_return cxx::unexpected(prepared.error());
        }
        data.order(
            descriptions.size(),
            capacity(),
            [&](std::size_t i) noexcept { return extension_detail::ResourceIdentity::local(ids[i]).index; }
        );
        writes.reserve(selected.size() * kStride.size());
        const std::uint32_t alive = 1;
        for (std::size_t field = 0; field < kStride.size(); ++field)
        {
            data.forEachOrdered(
                [&](std::size_t i) noexcept { return extension_detail::ResourceIdentity::local(ids[i]).index; },
                [&](std::uint32_t record, std::size_t i) noexcept
                {
                    const auto bytes = field == 0 ? std::as_bytes(std::span(&properties[i], 1))
                                                  : std::as_bytes(std::span(&alive, 1));
                    writes.push_back(
                        {fieldBuffer(frame, record / kMeshInstancePageSlots, field),
                         fieldOffset(record, kStride[field]),
                         bytes}
                    );
                }
            );
        }
        auto& accepted_uses = replica_progress_.replicaFrontiers(*replica);
        if (auto accepted = co_await uploads_.writeBuffers(writes, {}, &accepted_uses); !accepted)
        {
            co_return cxx::unexpected(accepted.error());
        }
        if (stop.stop_requested()) co_return cxx::unexpected(RenderError{kUploadStopped});
        for (std::size_t i = 0; i < descriptions.size(); ++i)
        {
            const auto record = extension_detail::ResourceIdentity::local(ids[i]).index;
            auto& page = *pages_[record / kMeshInstancePageSlots];
            const auto local = record % kMeshInstancePageSlots;
            const auto& description = selected[i];
            page.properties[local] = properties[i];
            page.records[local] = {
                description.mesh, description.material, description.transform_id, description.submesh
            };
            page.alive[local] = 1;
            entries_.find(extension_detail::ResourceIdentity::local(ids[i]))->key =
                extension_detail::ResourceIdentity::local(ids[i]);
            for (std::uint32_t other = 0; other < frame_count_; ++other)
            {
                if (other != frame)
                {
                    mark(other, record, 3);
                }
            }
            for (auto& dirty : data.slices[frame].dirty)
            {
                dirty.acknowledge({record / 64, std::uint64_t{1} << (record % 64)});
            }
        }
        live_records_ += static_cast<std::uint32_t>(descriptions.size());
        invalidateSelection();
        data.clearOrder();
        updating_ = false;
        rollback.release();
        if constexpr (std::same_as<Input, MeshInstanceData>)
        {
            co_return ids[0];
        }
        else
        {
            co_return std::move(ids);
        }
    }

    RenderResult<void> RasterMeshInstanceStorage::update(std::span<const MeshInstanceUpdate> instances) noexcept
    {
        if (updating_) return cxx::unexpected(RenderError{kDeviceBusy});
        auto& data = *frames_;
        data.properties.clear();
        data.properties.reserve(instances.size());
        // Validate the batch before publishing any references. Preparation resolves each FIF
        // replica's physical material address from the qualified identity.
        for (const auto& value : instances)
        {
            const auto record = extension_detail::ResourceIdentity::local(value.instance);
            const auto* entry = extension_detail::ResourceIdentity::parent(value.instance) == id_
                ? entries_.find(record) : nullptr;
            if (!entry || entry->key.isNull())
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            auto property = resolveReferences(value.description, 0);
            if (!property)
            {
                return cxx::unexpected(property.error());
            }
            data.properties.push_back(*property);
        }
        bool instances_changed = false;
        for (std::size_t i = 0; i < instances.size(); ++i)
        {
            const auto& value = instances[i];
            const auto& description = value.description;
            auto& entry = *entries_.find(extension_detail::ResourceIdentity::local(value.instance));
            const auto previous = instanceRecord(entry);
            const bool group_changed = previous.mesh != description.mesh ||
                previous.material != description.material || previous.submesh != description.submesh;
            const auto record = extension_detail::ResourceIdentity::local(value.instance).index;
            auto& page = *pages_[record / kMeshInstancePageSlots];
            const auto slot = record % kMeshInstancePageSlots;
            const auto transform = description.transform_id;
            if (previous.transform != transform)
            {
                transforms_.retainInstance(transform);
                transforms_.releaseInstance(previous.transform);
            }
            const auto& new_property = data.properties[i];
            const auto& old_property = page.properties[slot];
            const bool enabled_changed = ((old_property.render_flags ^ new_property.render_flags) & kRenderEnabledBit) != 0;
            const bool property_changed = group_changed || old_property.render_flags != new_property.render_flags ||
                old_property.transform_record != new_property.transform_record;
            const auto fields = static_cast<std::uint8_t>(property_changed ? 1 : 0);
            page.records[slot] = {description.mesh, description.material, transform, description.submesh};
            page.properties[slot] = new_property;
            for (std::uint32_t frame = 0; frame < frame_count_; ++frame)
            {
                mark(frame, record, fields);
            }
            instances_changed |= fields != 0;
            if (group_changed || enabled_changed) invalidateSelection();
        }
        preparation_revision_ += instances_changed;
        return {};
    }

    exec::task<RenderResult<Ack>> RasterMeshInstanceStorage::release(MeshInstanceId input) noexcept
    {
        if (updating_)
        {
            co_return cxx::unexpected(RenderError{kDeviceBusy});
        }
        const auto* entry = extension_detail::ResourceIdentity::parent(input) == id_
                                ? entries_.find(extension_detail::ResourceIdentity::local(input))
                                : nullptr;
        if (!entry || entry->key.isNull())
        {
            co_return cxx::unexpected(RenderError{kStorageIdentity});
        }
        updating_ = true;
        ++preparation_revision_;
        cxx::scope_exit cleanup([&]() noexcept { updating_ = false; });
        auto replica = co_await replica_progress_.acquireReplica(session_);
        if (!replica) co_return cxx::unexpected(replica.error());
        cxx::scope_exit end_replica([&]() noexcept { replica_progress_.endReplica(*replica); });
        const auto frame = replica->frame.slot_index;
        if (auto prepared = ensureFrame(frame); !prepared)
        {
            co_return cxx::unexpected(prepared.error());
        }
        const auto record = extension_detail::ResourceIdentity::local(input).index;
        const std::uint32_t dead = 0;
        const BufferWrite write{
            fieldBuffer(frame, record / kMeshInstancePageSlots, 1),
            fieldOffset(record, sizeof(dead)),
            std::as_bytes(std::span(&dead, 1))
        };
        auto& accepted_uses = replica_progress_.replicaFrontiers(*replica);
        if (auto accepted = co_await uploads_.writeBuffers(std::span(&write, 1), {}, &accepted_uses); !accepted)
        {
            mark(frame, record, 2);
            co_return cxx::unexpected(accepted.error());
        }
        invalidateSelection();
        auto& page = *pages_[record / kMeshInstancePageSlots];
        const auto local = record % kMeshInstancePageSlots;
        page.alive[local] = 0;
        transforms_.releaseInstance(page.records[local].transform);
        page.records[local] = {};
        for (std::uint32_t other = 0; other < frame_count_; ++other)
        {
            if (other != frame)
            {
                mark(other, record, 2);
            }
        }
        frames_->slices[frame].dirty[1].acknowledge({record / 64, std::uint64_t{1} << (record % 64)});
        entries_.erase(extension_detail::ResourceIdentity::local(input));
        --live_records_;
        co_return Ack{};
    }

    RenderResult<bool>
    RasterMeshInstanceStorage::prepareFrame(
        std::uint32_t frame,
        ResourceUploader& uploads,
        Preparation& preparation
    ) noexcept
    {
        if (frame >= frame_count_) return cxx::unexpected(RenderError{kDeviceArgument});
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
            RenderResult<void> result;
            const bool complete = DirtyRanges::visitRanges(
                slice.selected,
                page_records,
                [&](std::uint32_t first, std::uint32_t count) noexcept
                {
                    while (count)
                    {
                        auto prepared = source(first, count);
                        if (!prepared)
                        {
                            result = cxx::unexpected(prepared.error());
                            return false;
                        }
                        auto [write, unit] = *prepared;
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
                        dirty.detachRange(accepted, first, records);
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
            const bool is_incomplete_snapshot = !complete || !dirty.empty();
            if (is_incomplete_snapshot)
            {
                return cxx::unexpected(RenderError{kDeviceCapacity});
            }
            return true;
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
                preparation.onSubmitted(
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
                    -> RenderResult<std::pair<BufferWrite, std::size_t>>
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
                    -> RenderResult<std::pair<BufferWrite, std::size_t>>
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
        std::array<MeshInstanceGpuProperty, kMeshInstanceDirtySlots> property_values;
        for (std::size_t field = 0; field < kStride.size(); ++field)
        {
            auto fields = stage(
                slice.dirty[field],
                slice.accepted[field],
                kStride[field],
                field == 1 ? kMeshInstancePageSlots : kMeshInstanceDirtySlots,
                [&](std::uint32_t first,
                    std::uint32_t count) noexcept -> RenderResult<std::pair<BufferWrite, std::size_t>>
                {
                    const auto local = first % kMeshInstancePageSlots;
                    if (field == 0 && !pages_.empty())
                    {
                        const auto& page = *pages_[first / kMeshInstancePageSlots];
                        std::copy_n(page.properties.begin() + local, count, property_values.begin());
                        for (std::uint32_t i = 0; i < count; ++i)
                        {
                            if (page.alive[local + i] == 0)
                                continue;
                            const auto material = page.records[local + i].material;
                            auto storage = resources_.borrow<RasterMaterialStorage>(
                                extension_detail::ResourceIdentity::parent(material)
                            );
                            if (!storage)
                                return cxx::unexpected(storage.error());
                            auto reference = (*storage)->gpuReference(material, frame);
                            if (!reference)
                                return cxx::unexpected(reference.error());
                            property_values[i].material_buffer = reference->buffer_index;
                            property_values[i].material_record = reference->record_index;
                        }
                        return std::pair{
                            BufferWrite{
                                fieldBuffer(frame, first / kMeshInstancePageSlots, field),
                                fieldOffset(first, kStride[field]),
                                std::as_bytes(std::span(property_values).first(count))
                            },
                            kStride[field]
                        };
                    }
                    const auto source = [&]() noexcept -> std::span<const std::byte>
                    {
                        if (pages_.empty())
                        {
                            return emptyField(field);
                        }
                        const auto& page = *pages_[first / kMeshInstancePageSlots];
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
        return ready && !slice.pending_flat.properties;
    }

    void RasterMeshInstanceStorage::adoptFrameBuffers(std::uint32_t frame) noexcept
    {
        auto& slice = frames_->slices[frame];
        if (paged_)
        {
            const auto dependency_count = slice.table->leaves.size() + slice.known_pages;
            if (!slice.root_published || slice.addressed_buffers[0].size() != dependency_count)
            {
                slice.root_published = true;
                publishBuffers(frame);
            }
            return;
        }
        if (!slice.pending_flat.properties)
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
        for (std::size_t field = 0; field < kStride.size(); ++field)
        {
            slice.dirty[field].finishSelection(slice.accepted[field], bool(accepted));
        }
        if (slice.table)
        {
            slice.table->roots_dirty.finishSelection(slice.table->roots_accepted, bool(accepted));
            slice.table->pages_dirty.finishSelection(slice.table->pages_accepted, bool(accepted));
        }
        const bool fields_clean = std::ranges::all_of(slice.dirty, [](const auto& dirty) { return dirty.empty(); });
        const bool table_clean = !slice.table || (slice.table->roots_dirty.empty() && slice.table->pages_dirty.empty());
        if (accepted && fields_clean && table_clean)
        {
            adoptFrameBuffers(frame);
        }
    }

    RenderResult<void> RasterMeshInstanceStorage::resolve(
        const VGraphResourceDesc& plan,
        const MeshInstanceBinding& input,
        StorageSubscription& subscription,
        std::span<ResourceInput> output
    ) noexcept
    {
        const auto* description = std::get_if<GraphBufferDesc>(&plan);
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
        if (output.size() > frame_count_) return cxx::unexpected(RenderError{kDeviceArgument});
        subscription.attach(bindings_[field]);
        for (std::uint32_t frame = 0; frame < output.size(); ++frame)
        {
            auto& slice = frames_->slices[frame];
            const auto handle = paged_ ? (slice.table ? slice.table->root : nullptr) : slice.flat.field(field);
            output[frame] = {
                handle,
                0,
                handle ? session_.nriCore().GetBufferDesc(*handle).size : bytes,
                nullptr,
                slice.addressed_buffers[field],
                slice.addressed_revision
            };
        }
        return {};
    }

    RenderResult<StorageGraphImport>
    RasterMeshInstanceStorage::describeGraphImport(std::uint32_t projection) const noexcept
    {
        constexpr auto root = static_cast<std::uint32_t>(EMeshInstanceBinding::PAGED_ROOT);
        const bool invalid_projection = paged_ ? projection != root : projection >= root;
        if (invalid_projection) return cxx::unexpected(RenderError{kStorageMismatch});
        const auto bytes = meshInstanceProjectionBytes(static_cast<EMeshInstanceBinding>(projection));
        return StorageGraphImport{
            GraphBufferDesc{bytes}, {GraphBufferRange{0, bytes}}, EGraphUse::STORAGE_READ, EGraphUse::STORAGE_READ
        };
    }

    RenderResult<void> RasterMeshInstanceStorage::resolveGraphImport(
        std::uint32_t projection, const VGraphResourceDesc& plan, StorageSubscription& subscription,
        std::span<ResourceInput> output
    ) noexcept
    {
        auto contract = describeGraphImport(projection);
        if (!contract || contract->description != plan)
            return cxx::unexpected(RenderError{kStorageMismatch});
        if (!paged_)
            return resolve(plan, {static_cast<EMeshInstanceBinding>(projection)}, subscription, output);
        if (output.size() > frame_count_) return cxx::unexpected(RenderError{kDeviceArgument});
        subscription.attach(paged_bindings_);
        for (std::uint32_t frame = 0; frame < output.size(); ++frame) output[frame] = pagedRoot(frame);
        return {};
    }

    ResourceRegistration RasterMeshInstanceStorage::factory() noexcept
    {
        static const auto frame_call =
            FrameDataCall::declare<InstanceResource, RasterMeshInstanceStorage, &applyInputs>();

        static const std::array commands{
            CommandBinding::declare<InstanceCommands::CreateInstance, &createInstance>(),
            CommandBinding::declare<InstanceCommands::CreateInstances, &createInstances>(),
            CommandBinding::declare<InstanceCommands::ReleaseInstance, &releaseInstance>(),
            CommandBinding::declare<InstanceCommands::Update, &updateInstances>()
        };
        static constexpr auto snapshot = StorageSnapshotOps::make<
            RasterMeshInstanceStorage, MeshInstanceSnapshot, &RasterMeshInstanceStorage::captureMeshSelections,
            &RasterMeshInstanceStorage::selection_revision_
        >();
        static const StorageBindingOps binding{
            [](void* value,
               const VGraphResourceDesc& plan,
               const void* argument,
               StorageSubscription& subscription,
               std::span<ResourceInput> output) noexcept
            {
                return static_cast<RasterMeshInstanceStorage*>(value)
                    ->resolve(plan, *static_cast<const MeshInstanceBinding*>(argument), subscription, output);
            },
            [](void* value, std::uint32_t frame, ResourceUploader& uploads, Preparation& preparation) noexcept
            { return static_cast<RasterMeshInstanceStorage*>(value)->prepareFrame(frame, uploads, preparation); },
            [](const void* value) noexcept
            { return &static_cast<const RasterMeshInstanceStorage*>(value)->preparation_revision_; },
            [](const void* value, std::uint32_t projection) noexcept
            { return static_cast<const RasterMeshInstanceStorage*>(value)->describeGraphImport(projection); },
            [](void* value, std::uint32_t projection, const VGraphResourceDesc& plan,
               StorageSubscription& node, std::span<ResourceInput> output) noexcept
            { return static_cast<RasterMeshInstanceStorage*>(value)->resolveGraphImport(projection, plan, node, output); },
            &snapshot
        };
        static const auto create = +[](ResourceCreateContext& dependencies, ResourceInstanceId id
                                    ) noexcept -> RenderResult<std::unique_ptr<RasterMeshInstanceStorage>>
        {
            if (!dependencies.uploads || !dependencies.replica_progress || dependencies.frame_count == 0 || dependencies.frame_count > 3)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            auto& registry = dependencies.resources;
            auto owner = registry.requireDuringConstruction(dependencies, id, registry.typeId(kTransformResourceName));
            if (!owner) return cxx::unexpected(owner.error());
            auto storage = registry.borrow<TransformStorage>(*owner);
            if (!storage) return cxx::unexpected(storage.error());
            return std::make_unique<RasterMeshInstanceStorage>(dependencies, id, **storage);
        };
        return makeResourceRegistration<render::InstanceResource, RasterMeshInstanceStorage, create>(
            nullptr, &binding, commands, frame_call
        );
    }
} // namespace lux::render::detail
