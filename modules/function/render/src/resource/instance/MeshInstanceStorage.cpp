#include <lux/engine/render/resource/transform/Resource.hpp>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/device/ResourceUploader.hpp>
#include <lux/engine/render/graph/GraphResource.hpp>
#include <lux/engine/render/memory/Cleanup.hpp>
#include <lux/engine/render/memory/DirtyRanges.hpp>
#include <lux/engine/render/resource/ResourceSet.hpp>
#include <lux/engine/render/resource/mesh/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/resource/material/MaterialStorage.hpp>
#include <lux/engine/render/resource/instance/MeshInstanceStorage.hpp>
#include <lux/engine/render/resource/transform/TransformStorage.hpp>

namespace lux::render::detail
{
    namespace
    {
        constexpr auto first_group_projection = static_cast<std::uint32_t>(EMeshInstanceBinding::CANDIDATES);
        constexpr auto group_projection_count =
            static_cast<std::uint32_t>(EMeshInstanceBinding::CAMERA_DISPATCH) - first_group_projection + 1;

        constexpr std::uint32_t kRenderEnabledBit = 1;

    } // namespace

    static_assert(offsetof(MeshInstanceGpuProperty, material_buffer) == 4);
    static_assert(offsetof(MeshInstanceGpuProperty, material_record) == 8);
    static_assert(offsetof(MeshInstanceGpuProperty, section_index) == 12);
    static_assert(offsetof(MeshInstanceGpuProperty, render_flags) == 16);
    static_assert(offsetof(MeshInstanceGpuProperty, transform_id) == 20);

    struct MeshInstanceStorage::GpuPage final
    {
        nri::Buffer *properties = nullptr, *alive = nullptr;

        nri::Buffer*& field(std::size_t index) noexcept
        {
            return index == 0 ? properties : alive;
        }
    };

    struct MeshInstanceStorage::Page final
    {
        std::array<MeshInstanceGpuProperty, kMeshInstancePageSlots> properties{};
        // Dirty property ranges resolve this material column for the selected physical frame.
        std::array<MaterialId, kMeshInstancePageSlots> materials{};
        std::array<std::uint32_t, kMeshInstancePageSlots> alive{};
        std::array<GpuPage, 3> gpu;
    };

    namespace
    {
        // Each replica field owns its pending records and the prefix staged for this submission.
        struct ReplicaFieldUpdates final
        {
            DirtyRanges pending;
            std::vector<DirtyWordMask> staged;

            void settle(bool accepted) noexcept { pending.finishSelection(staged, accepted); }
        };
    } // namespace

    struct MeshInstanceStorage::PageTable final
    {
        struct Leaf final
        {
            nri::Buffer* buffer = nullptr;
            std::array<MeshInstancePageGpu, kMeshInstanceTableAxis> addresses{};
        };

        nri::Buffer* root = nullptr;
        std::array<std::uint64_t, kMeshInstanceTableAxis> addresses{};
        std::vector<Leaf> leaves;
        ReplicaFieldUpdates roots, pages;
    };

    // Each FIF replica retains its published buffers and the upload progress for their replacements.
    struct MeshInstanceStorage::FrameData final
    {
        struct GroupBuffer final
        {
            nri::Buffer* buffer = nullptr;
            nri::Buffer* replacement = nullptr;
            std::uint64_t published_size = 0;
            GraphBufferRange pending{}, staged{};

            RenderResult<nri::Buffer*>
            reserve(DeviceSession& session, std::size_t bytes, EMeshInstanceBinding role) noexcept
            {
                auto* target = replacement ? replacement : buffer;
                const auto& core = session.nriCore();
                if (target && core.GetBufferDesc(*target).size >= bytes) return target;

                const auto limit = session.description().memory.storageBufferMaxRange;
                if (bytes > limit) return cxx::unexpected(RenderError{kStorageCapacity});
                const auto capacity = std::min<std::uint64_t>(std::bit_ceil(bytes), limit);
                const bool indirect = role == EMeshInstanceBinding::CAMERA_DISPATCH;
                const auto stride = indirect ? 0u : static_cast<std::uint32_t>(meshInstanceProjectionBytes(role));
                const auto usage = indirect ? nri::BufferUsageBits::ARGUMENT : nri::BufferUsageBits::SHADER_RESOURCE;
                const nri::BufferDesc desc{capacity, stride, usage};
                nri::Buffer* candidate = nullptr;
                const auto result = core.CreatePlacedBuffer(session.device(), NriDeviceHeap, desc, candidate);
                if (result != nri::Result::SUCCESS) return cxx::unexpected(nativeError(result));

                releaseAfterGpu(session, replacement);
                replacement = candidate;
                pending = {0, bytes};
                return replacement;
            }
        };

        struct Slice final
        {
            std::array<GroupBuffer, group_projection_count> groups;
            GpuPage flat;
            GpuPage pending_flat;
            std::unique_ptr<PageTable> table;
            bool needs_upload = true;
            std::uint32_t known_pages = 0;
            std::vector<nri::Buffer*> root_dependencies;
            std::uint64_t root_revision = 0;
            [[nodiscard]] bool needsRootPublication() const noexcept
            {
                return table && (!root_revision || root_dependencies.size() != table->leaves.size() + 2ull * known_pages);
            }
            std::array<ReplicaFieldUpdates, 2> fields;
            std::vector<DirtyWordMask> selected;
        };

        std::array<Slice, 3> slices;
        // Rows are addressed by EMeshVisibilityExtent. Jobs borrow them through their installation lifetime.
        std::array<std::array<std::uint64_t, 3>, 2> camera_extents{};
    };

    // Both borrows end with the synchronous create/update batch.
    struct MeshInstanceStorage::InputSources final
    {
        const IndexedTriangleMeshStorage* meshes = nullptr;
        const MaterialStorage* materials = nullptr;
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

    MeshInstanceStorage::MeshInstanceStorage(
        ResourceCreateContext& dependencies, ResourceInstanceId id, TransformStorage& transforms
    ) noexcept
        : session_(dependencies.device), frame_count_(dependencies.frame_count),
          resources_(dependencies.resources), transforms_(transforms), id_(id),
          paged_(dependencies.device.usesDeviceAddresses()),
          frames_(std::make_unique<FrameData>())
    {
    }

    void MeshInstanceStorage::release(GpuPage& page) noexcept
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

    void MeshInstanceStorage::release(PageTable& table) noexcept
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

    MeshInstanceStorage::~MeshInstanceStorage() noexcept
    {
        for (const auto& entry : entries_)
        {
            transforms_.releaseInstance(entry.transform);
        }
        for (auto& slice : frames_->slices)
        {
            for (auto& group : slice.groups)
            {
                releaseAfterGpu(session_, group.buffer);
                releaseAfterGpu(session_, group.replacement);
            }
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
    MeshInstanceStorage::fieldBuffer(std::uint32_t frame, std::uint32_t page, std::size_t field) noexcept
    {
        auto& slice = frames_->slices[frame];
        return paged_ ? pages_[page]->gpu[frame].field(field)
                      : (slice.pending_flat.properties ? slice.pending_flat : slice.flat).field(field);
    }

    std::uint64_t MeshInstanceStorage::fieldOffset(std::uint32_t record, std::size_t stride) const noexcept
    {
        return std::uint64_t(paged_ ? record % kMeshInstancePageSlots : record) * stride;
    }

    ResourceInput MeshInstanceStorage::pagedRoot(std::uint32_t frame) const noexcept
    {
        const auto& slice = frames_->slices[frame];
        if (!slice.root_revision) return {nullptr, 0, meshInstanceProjectionBytes(EMeshInstanceBinding::PAGED_ROOT)};
        auto* root = slice.table->root;
        return {
            root, 0, session_.nriCore().GetBufferDesc(*root).size, nullptr,
            slice.root_dependencies, slice.root_revision
        };
    }

    void MeshInstanceStorage::mark(std::uint32_t frame, std::uint32_t record, std::uint8_t fields) noexcept
    {
        auto& slice = frames_->slices[frame];
        slice.needs_upload = true;
        for (std::size_t field = 0; field < kStride.size(); ++field)
        {
            if (!(fields & (1u << field)))
            {
                continue;
            }
            slice.fields[field].pending.grow(capacity());
            slice.fields[field].pending.mark(record);
        }
    }

    RenderResult<void> MeshInstanceStorage::ensureFrame(std::uint32_t frame) noexcept
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
            for (auto& field : slice.fields)
            {
                field.pending.grow(required);
                const auto first =
                    growing ? 0 : std::min<std::uint64_t>(required, std::uint64_t(first_page) * kMeshInstancePageSlots);
                field.pending.markRange(first, required - first);
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
                table.roots.pending.grow(kMeshInstanceTableAxis);
                table.roots.pending.markRange(0, kMeshInstanceTableAxis);
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
                table.roots.pending.mark(static_cast<std::uint32_t>(index));
                table.leaves.push_back(std::move(leaf));
                table.pages.pending.grow(table.leaves.size() * kMeshInstanceTableAxis);
                table.pages.pending.markRange(index * kMeshInstanceTableAxis, kMeshInstanceTableAxis);
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
                table.pages.pending.mark(page);
            }
        }
        if (paged_)
        {
            for (auto& field : slice.fields)
            {
                field.pending.grow(capacity());
                field.pending.markRange(
                    std::uint64_t(first_page) * kMeshInstancePageSlots,
                    (pages_.size() - first_page) * kMeshInstancePageSlots
                );
            }
        }
        slice.known_pages = static_cast<std::uint32_t>(pages_.size());
        return {};
    }

    RenderResult<void> MeshInstanceStorage::refreshDrawGroups() noexcept
    {
        if (!grouping_.needsPublication()) return {};
        auto changes = grouping_.publish(resources_, selection_revision_);
        if (!changes) return cxx::unexpected(changes.error());
        for (auto& slice : std::span{frames_->slices}.first(frame_count_))
            for (std::size_t i = 0; i < slice.groups.size(); ++i)
                markDirtyBytes(slice.groups[i].pending, (*changes)[i]);
        return {};
    }

    RenderResult<ResourceViewValue<MeshDrawTopology>> MeshInstanceStorage::selectDrawTopology() noexcept
    {
        if (auto grouped = refreshDrawGroups(); !grouped) return cxx::unexpected(grouped.error());
        // A material reassignment within the same program changes draw inputs while keeping the GPU groups resident.
        const auto& selected = grouping_.selectSurfaces(selection_revision_);
        return ResourceViewValue<MeshDrawTopology>{selected, selection_revision_};
    }

    RenderResult<MeshInstanceStorage::ResolvedInstance>
    MeshInstanceStorage::resolveReferences(const MeshInstanceData& input, InputSources& sources) const noexcept
    {
        const auto bind = [this]<class T>(const T*& storage, auto id) noexcept -> RenderResult<void>
        {
            const auto owner = extension_detail::ResourceIdentity::parent(id);
            if (storage && storage->id() == owner) return {};
            if (!resources_.canReference(id_, owner)) return cxx::unexpected(RenderError{kStorageIdentity});
            const auto resolved = resources_.borrow<T>(owner);
            if (!resolved) return cxx::unexpected(resolved.error());
            storage = *resolved;
            return {};
        };
        if (auto bound = bind(sources.meshes, input.mesh); !bound) return cxx::unexpected(bound.error());
        if (auto bound = bind(sources.materials, input.material); !bound) return cxx::unexpected(bound.error());
        const auto* description = sources.meshes->find(input.mesh);
        const auto program = sources.materials->programOf(input.material);
        const bool valid_transform = transforms_.contains(input.transform);
        const bool invalid_mesh = !description || input.submesh >= description->submeshes.size();
        const bool invalid_reference = invalid_mesh || program.isNull() || !valid_transform;
        if (invalid_reference)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        const MeshInstanceGpuProperty property{
            .mesh_index = extension_detail::ResourceIdentity::local(input.mesh).index,
            .section_index = input.submesh,
            .render_flags = (input.render_enabled ? kRenderEnabledBit : 0u) | (input.cast_shadow ? 2u : 0u),
            .transform_id = TransformId{extension_detail::ResourceIdentity::local(input.transform).index}
        };
        return ResolvedInstance{property, program};
    }

    RenderResult<void> MeshInstanceStorage::create(
        std::span<const MeshInstanceData> descriptions, std::span<MeshInstanceId> ids
    ) noexcept
    {
        if (descriptions.size() != ids.size()) std::terminate();
        if (descriptions.empty()) return {};
        if (descriptions.size() > UINT32_MAX - entries_.size()) return cxx::unexpected(RenderError{kStorageCapacity});

        InputSources sources;
        resolved_instances_.clear();
        resolved_instances_.reserve(descriptions.size());
        for (const auto& description : descriptions)
        {
            auto property = resolveReferences(description, sources);
            if (!property) return cxx::unexpected(property.error());
            resolved_instances_.push_back(*property);
        }

        // Reserve the actual sparse slots before committing any reference or dirty bit. Capacity depends
        // on the highest physical slot, including holes left by retired generations.
        std::uint64_t pages = pages_.size();
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            const auto& description = descriptions[i];
            const auto key = entries_.emplace(description.mesh, description.transform);
            ids[i] = extension_detail::ResourceIdentity::make<MeshInstanceId>(id_, key);
            pages = std::max(pages, std::uint64_t{key.index / kMeshInstancePageSlots} + 1);
        }
        const auto slots = paged_ ? kMeshInstancePageSlots : std::bit_ceil(pages * kMeshInstancePageSlots);
        if (slots > session_.description().memory.storageBufferMaxRange / sizeof(MeshInstanceGpuProperty))
        {
            for (const auto id : ids) entries_.erase(extension_detail::ResourceIdentity::local(id));
            return cxx::unexpected(RenderError{kStorageCapacity});
        }
        while (pages_.size() < pages) pages_.push_back(std::make_unique<Page>());

        bool enabled = false;
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            const auto& description = descriptions[i];
            transforms_.retainInstance(description.transform);
            const auto record = extension_detail::ResourceIdentity::local(ids[i]).index;
            const auto slot = record % kMeshInstancePageSlots;
            auto& page = *pages_[record / kMeshInstancePageSlots];
            page.properties[slot] = resolved_instances_[i].property;
            page.materials[slot] = description.material;
            page.alive[slot] = 1;
            grouping_.update(record, description, resolved_instances_[i].program, {});
            enabled |= description.render_enabled;
            for (std::uint32_t frame = 0; frame < frame_count_; ++frame) mark(frame, record, 3);
        }
        if (enabled && ++selection_revision_ == 0) std::terminate();
        return {};
    }

    RenderResult<void> MeshInstanceStorage::update(std::span<const MeshInstanceUpdate> instances) noexcept
    {
        InputSources sources;
        resolved_instances_.clear();
        resolved_instances_.reserve(instances.size());
        // Validate the batch before publishing any references. Preparation resolves each FIF
        // replica's physical material address from the qualified identity.
        for (const auto& value : instances)
        {
            const auto record = extension_detail::ResourceIdentity::local(value.instance);
            if (extension_detail::ResourceIdentity::parent(value.instance) != id_ || !entries_.isValid(record))
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            auto property = resolveReferences(value.description, sources);
            if (!property)
            {
                return cxx::unexpected(property.error());
            }
            resolved_instances_.push_back(*property);
        }
        bool selection_changed = false;
        for (std::size_t i = 0; i < instances.size(); ++i)
        {
            const auto& value = instances[i];
            const auto& description = value.description;
            const auto key = extension_detail::ResourceIdentity::local(value.instance);
            auto& entry = entries_[key];
            const auto record = key.index;
            auto& page = *pages_[record / kMeshInstancePageSlots];
            const auto slot = record % kMeshInstancePageSlots;
            const auto previous_material = page.materials[slot];
            const auto& old_property = page.properties[slot];
            const auto& new_property = resolved_instances_[i].property;
            const bool group_changed = entry.mesh != description.mesh || previous_material != description.material ||
                old_property.section_index != description.submesh;
            const bool flags_changed = old_property.render_flags != new_property.render_flags;
            const bool transform_changed = entry.transform != description.transform;
            if (!group_changed && !flags_changed && !transform_changed) continue;
            if (transform_changed)
            {
                transforms_.retainInstance(description.transform);
                transforms_.releaseInstance(entry.transform);
            }
            const bool enabled_changed = ((old_property.render_flags ^ new_property.render_flags) & kRenderEnabledBit) != 0;
            entry = {description.mesh, description.transform};
            page.materials[slot] = description.material;
            page.properties[slot] = new_property;
            for (std::uint32_t frame = 0; frame < frame_count_; ++frame) mark(frame, record, 1);
            if (enabled_changed || (group_changed && description.render_enabled))
            {
                grouping_.update(record, description, resolved_instances_[i].program, previous_material);
                selection_changed = true;
            }
        }
        if (selection_changed && ++selection_revision_ == 0) std::terminate();
        return {};
    }

    RenderResult<void> MeshInstanceStorage::release(MeshInstanceId id) noexcept
    {
        const auto key = extension_detail::ResourceIdentity::local(id);
        const auto* entry = extension_detail::ResourceIdentity::parent(id) == id_ ? entries_.find(key) : nullptr;
        if (!entry) return cxx::unexpected(RenderError{kStorageIdentity});
        auto& page = *pages_[key.index / kMeshInstancePageSlots];
        const auto slot = key.index % kMeshInstancePageSlots;
        const bool enabled = (page.properties[slot].render_flags & kRenderEnabledBit) != 0;
        grouping_.remove(key.index, page.materials[slot]);
        transforms_.releaseInstance(entry->transform);
        page.materials[slot] = {};
        page.alive[slot] = 0;
        for (std::uint32_t frame = 0; frame < frame_count_; ++frame) mark(frame, key.index, 2);
        entries_.erase(key);
        if (enabled && ++selection_revision_ == 0) std::terminate();
        return {};
    }

    RenderResult<void> MeshInstanceStorage::copyProperties(
        std::uint32_t first, std::uint32_t frame, std::span<MeshInstanceGpuProperty> output
    ) const noexcept
    {
        const auto& page = *pages_[first / kMeshInstancePageSlots];
        const auto local = first % kMeshInstancePageSlots;
        std::copy_n(page.properties.begin() + local, output.size(), output.begin());
        const MaterialStorage* storage = nullptr;
        MaterialId previous;
        rdesc::MaterialGpuReference address;
        for (std::size_t i = 0; i < output.size(); ++i)
        {
            const auto material = page.materials[local + i];
            if (material.isNull()) continue;
            if (material != previous)
            {
                const auto owner = extension_detail::ResourceIdentity::parent(material);
                if (!storage || storage->id() != owner)
                {
                    const auto found = resources_.borrow<MaterialStorage>(owner);
                    if (!found) return cxx::unexpected(found.error());
                    storage = *found;
                }
                const auto reference = storage->gpuReference(material, frame);
                if (!reference) return cxx::unexpected(reference.error());
                address = *reference;
                previous = material;
            }
            output[i].material_buffer = address.buffer_index;
            output[i].material_record = address.record_index;
        }
        return {};
    }

    RenderResult<bool>
    MeshInstanceStorage::prepareFrame(
        std::uint32_t frame,
        ResourceUploader& uploads
    ) noexcept
    {
        if (frame >= frame_count_) return cxx::unexpected(RenderError{kDeviceArgument});
        if (!frames_->slices[frame].needs_upload) return true;
        if (auto prepared = ensureFrame(frame); !prepared)
        {
            return cxx::unexpected(prepared.error());
        }
        auto& slice = frames_->slices[frame];
        const bool fields_clean = std::ranges::all_of(slice.fields, [](const auto& field) { return field.pending.empty(); });
        const bool table_clean = !slice.table || (slice.table->roots.pending.empty() && slice.table->pages.pending.empty());
        if (fields_clean && table_clean) return prepareGroupInputs(frame, uploads);
        bool ready = true;
        // Every source is authoritative CPU data or an address table for this physical slice.
        // The selected masks are bounded by the current staging window, never all live records.
        auto stage = [&](ReplicaFieldUpdates& field, std::size_t stride, std::uint32_t page_records, auto source)
            noexcept -> RenderResult<bool>
        {
            field.staged.clear();
            const auto word_limit = (uploads.windowByteLimit() / stride + 63) / 64 + uploads.windowRegionLimit();
            field.pending.select(word_limit, slice.selected);
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
                        auto copied = uploads.stageBufferPrefix(*prepared, stride);
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
                        field.pending.detachRange(field.staged, first, records);
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
            return complete && field.pending.empty();
        };
        if (slice.table)
        {
            auto& table = *slice.table;
            auto roots = stage(
                table.roots,
                sizeof(std::uint64_t),
                static_cast<std::uint32_t>(table.addresses.size()),
                [&](std::uint32_t first, std::uint32_t count) noexcept
                    -> RenderResult<BufferWrite>
                {
                    return BufferWrite{
                        table.root,
                        std::uint64_t(first) * sizeof(std::uint64_t),
                        std::as_bytes(std::span(table.addresses).subspan(first, count))
                    };
                }
            );
            if (!roots)
            {
                return cxx::unexpected(roots.error());
            }
            ready &= *roots;
            auto pages = stage(
                table.pages,
                sizeof(MeshInstancePageGpu),
                kMeshInstanceTableAxis,
                [&](std::uint32_t first, std::uint32_t count) noexcept
                    -> RenderResult<BufferWrite>
                {
                    auto& leaf = table.leaves[first / kMeshInstanceTableAxis];
                    const auto local = first % kMeshInstanceTableAxis;
                    return BufferWrite{
                        leaf.buffer,
                        std::uint64_t(local) * sizeof(MeshInstancePageGpu),
                        std::as_bytes(std::span(leaf.addresses).subspan(local, count))
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
                slice.fields[field],
                kStride[field],
                field == 1 ? kMeshInstancePageSlots : kMeshInstanceDirtySlots,
                [&](std::uint32_t first,
                    std::uint32_t count) noexcept -> RenderResult<BufferWrite>
                {
                    const auto local = first % kMeshInstancePageSlots;
                    if (field == 0 && !pages_.empty())
                    {
                        const auto values = std::span{property_values}.first(count);
                        if (auto copied = copyProperties(first, frame, values); !copied)
                            return cxx::unexpected(copied.error());
                        return BufferWrite{
                            fieldBuffer(frame, first / kMeshInstancePageSlots, field),
                            fieldOffset(first, kStride[field]),
                            std::as_bytes(values)
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
                    return BufferWrite{
                        fieldBuffer(frame, first / kMeshInstancePageSlots, field),
                        fieldOffset(first, kStride[field]),
                        source.subspan(local * kStride[field], count * kStride[field])
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
        if (!ready || slice.pending_flat.properties || slice.needsRootPublication()) return false;
        return prepareGroupInputs(frame, uploads);
    }

    RenderResult<bool>
    MeshInstanceStorage::prepareGroupInputs(std::uint32_t frame, ResourceUploader& uploads) noexcept
    {
        std::array<bool, group_projection_count> needed;
        for (std::uint32_t i = 0; i < needed.size(); ++i)
            needed[i] = resources_.hasGraphInputUsers(id_, first_group_projection + i);
        if (std::ranges::find(needed, true) == needed.end()) return publishFrameBindings(frame, {});
        if (auto grouped = refreshDrawGroups(); !grouped) return cxx::unexpected(grouped.error());
        const auto dispatch_limit = needed.back() ? session_.description().shaderStage.compute.dispatchMaxDim[0] : UINT32_MAX;
        const auto payloads = grouping_.uploadSources(dispatch_limit);
        if (!payloads) return cxx::unexpected(payloads.error());
        auto& buffers = frames_->slices[frame].groups;
        std::array<GraphBufferPublication, group_projection_count> publications;
        std::size_t count = 0;
        bool ready = true;
        for (std::uint32_t i = 0; i < buffers.size(); ++i)
        {
            const auto projection = first_group_projection + i;
            if (!needed[i]) continue;
            const auto bytes = (*payloads)[i];
            auto& slice = buffers[i];
            const auto target = slice.reserve(session_, bytes.size(), static_cast<EMeshInstanceBinding>(projection));
            if (!target) return cxx::unexpected(target.error());
            // A shorter logical projection drops its tail. Regrowth dirties the newly exposed bytes.
            auto& pending = slice.pending;
            pending.offset = std::min<std::uint64_t>(pending.offset, bytes.size());
            pending.size = std::min<std::uint64_t>(pending.size, bytes.size() - pending.offset);
            if (!pending.size)
            {
                if (!slice.replacement && slice.published_size == bytes.size()) continue;
                publications[count++] = {projection, frame, {*target, 0, bytes.size()}};
                continue;
            }
            auto copied = uploads.stageBufferPrefix({*target, pending.offset, bytes.subspan(pending.offset, pending.size)});
            if (!copied) return cxx::unexpected(copied.error());
            slice.staged = {pending.offset, *copied};
            pending.offset += *copied;
            pending.size -= *copied;
            // A resized or first-use descriptor is published after its complete contents are accepted.
            // An unchanged binding may be submitted behind this round's final upload.
            ready &= !slice.replacement && slice.published_size == bytes.size() &&
                !pending.size;
        }
        if (!ready) return false;
        return publishFrameBindings(frame, std::span{publications}.first(count));
    }

    RenderResult<bool>
    MeshInstanceStorage::publishFrameBindings(std::uint32_t frame, std::span<const GraphBufferPublication> groups) noexcept
    {
        auto& slice = frames_->slices[frame];
        const bool replaces_root = slice.needsRootPublication();
        const bool replaces_fields = slice.pending_flat.properties != nullptr;
        if (replaces_root || replaces_fields || !groups.empty())
        {
            std::array<GraphBufferPublication, group_projection_count + kStride.size()> updates;
            std::ranges::copy(groups, updates.begin());
            auto count = groups.size();
            std::vector<nri::Buffer*> dependencies;
            if (replaces_root)
            {
                if (slice.root_revision == UINT64_MAX) return cxx::unexpected(RenderError{kDeviceCapacity});
                dependencies.reserve(slice.table->leaves.size() + 2ull * slice.known_pages);
                for (const auto& leaf : slice.table->leaves) dependencies.push_back(leaf.buffer);
                for (std::size_t field = 0; field < kStride.size(); ++field)
                    for (std::uint32_t page = 0; page < slice.known_pages; ++page)
                        dependencies.push_back(pages_[page]->gpu[frame].field(field));
                auto* root = slice.table->root;
                constexpr auto projection = static_cast<std::uint32_t>(EMeshInstanceBinding::PAGED_ROOT);
                updates[count++] = {projection, frame, {
                    root, 0, session_.nriCore().GetBufferDesc(*root).size, nullptr, dependencies, slice.root_revision + 1
                }};
            }
            else if (replaces_fields)
            {
                for (std::uint32_t field = 0; field < kStride.size(); ++field)
                {
                    auto* buffer = slice.pending_flat.field(field);
                    updates[count++] = {field, frame, {buffer, 0, session_.nriCore().GetBufferDesc(*buffer).size}};
                }
            }
            auto published = resources_.publishGraphBuffers(id_, std::span{updates}.first(count));
            if (!published) return cxx::unexpected(published.error());
            if (replaces_root)
            {
                // Installed spans retain the candidate allocation.
                slice.root_dependencies.swap(dependencies);
                ++slice.root_revision;
            }
            else if (replaces_fields)
            {
                auto previous = std::exchange(slice.flat, std::exchange(slice.pending_flat, {}));
                release(previous);
            }
            for (const auto& update : groups)
            {
                auto& group = slice.groups[update.projection - first_group_projection];
                if (group.replacement)
                {
                    auto* previous = std::exchange(group.buffer, std::exchange(group.replacement, nullptr));
                    releaseAfterGpu(session_, previous);
                }
                group.published_size = update.value.size;
            }
        }
        slice.needs_upload = false;
        auto& extents = frames_->camera_extents;
        extents[static_cast<std::size_t>(EMeshVisibilityExtent::VISIBLE_MEMBERS)][frame] = grouping_.visibleBytes();
        extents[static_cast<std::size_t>(EMeshVisibilityExtent::INDEXED_COMMANDS)][frame] =
            std::uint64_t{grouping_.drawGroupCount()} * kMeshWindingCount * sizeof(nri::DrawIndexedDesc);
        return true;
    }

    void MeshInstanceStorage::finishFramePreparation(const ReplicaSettlement& settlement) noexcept
    {
        const auto frame = settlement.frame.slot;
        if (settlement.error.type) frames_->slices[frame].needs_upload = true;
        if (!settlement.copies.count) return;
        const bool accepted = !settlement.error.type;
        auto& slice = frames_->slices[frame];
        for (auto& group : slice.groups)
        {
            const auto staged = std::exchange(group.staged, {});
            if (!accepted) markDirtyBytes(group.pending, staged);
        }
        for (auto& field : slice.fields) field.settle(accepted);
        if (slice.table)
        {
            slice.table->roots.settle(accepted);
            slice.table->pages.settle(accepted);
        }
    }

    RenderResult<ResourceProjection>
    MeshInstanceStorage::describeGraphImport(std::uint32_t projection) const noexcept
    {
        constexpr auto root = static_cast<std::uint32_t>(EMeshInstanceBinding::PAGED_ROOT);
        const bool group_input = projection >= first_group_projection && projection < first_group_projection + group_projection_count;
        const bool invalid_projection = !group_input && (paged_ ? projection != root : projection >= root);
        if (invalid_projection) return cxx::unexpected(RenderError{kStorageMismatch});
        return meshInstanceProjection(static_cast<EMeshInstanceBinding>(projection));
    }

    RenderResult<void> MeshInstanceStorage::resolveGraphImport(
        std::uint32_t projection, const VGraphResourceDesc& plan, std::span<ResourceInput> output
    ) noexcept
    {
        auto contract = describeGraphImport(projection);
        if (!contract || contract->description != plan)
            return cxx::unexpected(RenderError{kStorageMismatch});
        if (projection >= first_group_projection)
        {
            if (output.size() > frame_count_) return cxx::unexpected(RenderError{kDeviceArgument});
            // Installation can introduce the first reader of this GPU table after another Job prepared the replica.
            // The owner inspects committed readers before allocating or uploading anything.
            if (!resources_.hasGraphInputUsers(id_, projection))
                for (auto& slice : std::span{frames_->slices}.first(frame_count_)) slice.needs_upload = true;
            const auto stride = meshInstanceProjectionBytes(static_cast<EMeshInstanceBinding>(projection));
            for (std::uint32_t frame = 0; frame < output.size(); ++frame)
            {
                const auto& slice = frames_->slices[frame].groups[projection - first_group_projection];
                output[frame] = {slice.buffer, 0, slice.buffer ? slice.published_size : stride};
            }
            return {};
        }
        if (output.size() > frame_count_) return cxx::unexpected(RenderError{kDeviceArgument});
        for (std::uint32_t frame = 0; frame < output.size(); ++frame)
        {
            if (paged_) output[frame] = pagedRoot(frame);
            else
            {
                auto* buffer = frames_->slices[frame].flat.field(projection);
                const auto bytes = buffer ? session_.nriCore().GetBufferDesc(*buffer).size :
                    std::max<std::uint64_t>(2, capacity()) * kStride[projection];
                output[frame] = {buffer, 0, bytes};
            }
        }
        return {};
    }


    ResourceRegistration MeshInstanceStorage::factory() noexcept
    {

        static constexpr auto cpu_view = ResourceViewOps::make<
            MeshInstanceStorage, MeshDrawTopology, &MeshInstanceStorage::selectDrawTopology
        >();
        static const StorageBindingOps binding{
            .prepareFrame = [](void* value, ReplicaFrame frame, ResourceUploader& uploads) noexcept
            { return static_cast<MeshInstanceStorage*>(value)->prepareFrame(frame.slot, uploads); },
            .describeGraphImport = [](const void* value, std::uint32_t projection) noexcept
            { return static_cast<const MeshInstanceStorage*>(value)->describeGraphImport(projection); },
            .resolveGraphImport = [](void* value, std::uint32_t projection, const VGraphResourceDesc& plan,
               std::span<ResourceInput> output) noexcept
            { return static_cast<MeshInstanceStorage*>(value)->resolveGraphImport(projection, plan, output); },
            .cpu_view = cpu_view,
            .settleFrame = [](void* value, const ReplicaSettlement& settlement) noexcept
            { static_cast<MeshInstanceStorage*>(value)->finishFramePreparation(settlement); },
            .bindBufferExtent = [](const void* value, std::uint32_t field) noexcept -> std::span<const std::uint64_t>
            {
                const auto& owner = *static_cast<const MeshInstanceStorage*>(value);
                if (field >= owner.frames_->camera_extents.size()) return {};
                return std::span{owner.frames_->camera_extents[field]}.first(owner.frame_count_);
            }
        };
        static constexpr auto create = +[](ResourceCreateContext& dependencies, ResourceInstanceId id
                                    ) noexcept -> RenderResult<std::unique_ptr<MeshInstanceStorage>>
        {
            if (!dependencies.uploads || dependencies.frame_count == 0 || dependencies.frame_count > 3)
            {
                return cxx::unexpected(RenderError{kDeviceArgument});
            }
            auto& registry = dependencies.resources;
            auto owner = dependencies.require(kTransformResourceName);
            if (!owner) return cxx::unexpected(owner.error());
            auto storage = registry.borrow<TransformStorage>(*owner);
            if (!storage) return cxx::unexpected(storage.error());
            return std::make_unique<MeshInstanceStorage>(dependencies, id, **storage);
        };
        return makeResourceRegistration<render::InstanceResource, MeshInstanceStorage, create, &MeshInstanceStorage::update>(&binding);
    }
} // namespace lux::render::detail
