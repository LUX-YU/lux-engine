#include <bit>
#include <limits>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/features/MeshDrawGroups.hpp>
#include <lux/engine/render/features/MeshVisibilityInputs.hpp>
#include <lux/engine/render/memory/Cleanup.hpp>
#include <lux/engine/render/resource/mesh/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/resource/material/RasterMaterialStorage.hpp>

namespace lux::render::detail
{
    MeshDrawGroups::MeshDrawGroups(
        DeviceSession& device,
        FrameExecution& frames,
        ResourceRegistry& resources,
        MeshDrawGroupsConfiguration configuration
    ) noexcept
        : device_(device), frames_(frames), resources_(resources), configuration_(configuration)
    {
    }

    MeshDrawGroups::~MeshDrawGroups() noexcept
    {
        observer_.reset();
        for (auto& slice : slices_)
        {
            for (auto* buffer : slice.buffers)
            {
                releaseAfterGpu(device_, buffer);
            }
        }
    }

    bool MeshDrawGroups::remove(std::uint32_t instance) noexcept
    {
        auto& membership = memberships_[instance];
        auto& group = groups_[membership.group];
        const auto last = group.members.back();
        group.members[membership.position] = last;
        memberships_[last].position = membership.position;
        group.members.pop_back();
        bool source_changed = group.members.empty();
        if (group.members.empty())
        {
            indices_.erase(group.key);
            group.capacity = 0;
            free_groups_.push_back(membership.group);
            descriptions_[membership.group] = {};
        }
        else
        {
            auto& description = descriptions_[membership.group];
            description.record.candidate_count = static_cast<std::uint32_t>(group.members.size());
            const auto material = memberships_[group.members.front()].material;
            source_changed = description.selection.material_source != material;
            description.selection.material_source = material;
        }
        membership.group = UINT32_MAX;
        return source_changed;
    }

    void MeshDrawGroups::update(MeshInstanceId id, const RasterMeshInstanceStorage::Selection* selection) noexcept
    {
        if (id.isNull())
        {
            error_ = RenderError{kStorageIdentity};
            ++source_.revision;
            return;
        }
        const auto local = extension_detail::ResourceIdentity::local(id).index;
        GroupKey key;
        if (selection)
        {
            auto storage = resources_.borrow<RasterMaterialStorage>(
                extension_detail::ResourceIdentity::parent(selection->material)
            );
            const auto* program = storage ? (*storage)->program(selection->material) : nullptr;
            if (!program)
            {
                error_ = RenderError{kStorageIdentity};
                ++source_.revision;
                return;
            }
            key = {selection->mesh, program, selection->submesh};
        }
        if (local >= memberships_.size())
        {
            memberships_.resize(std::bit_ceil(std::size_t(local) + 1));
        }
        auto& membership = memberships_[local];
        bool source_changed = false;
        if (membership.group != UINT32_MAX)
        {
            if (selection && groups_[membership.group].key == key)
            {
                membership.material = selection->material;
                if (membership.position == 0)
                {
                    auto& source = descriptions_[membership.group].selection.material_source;
                    source_.revision += source != selection->material;
                    source = selection->material;
                }
                return; // Parameter-only material changes and Transform updates do not regroup.
            }
            source_changed = remove(local);
        }
        if (selection)
        {
            auto found = indices_.find(key);
            if (found == indices_.end())
            {
                const auto index =
                    free_groups_.empty() ? static_cast<std::uint32_t>(groups_.size()) : free_groups_.back();
                if (free_groups_.empty())
                {
                    groups_.emplace_back();
                    descriptions_.emplace_back();
                    source_.groups = descriptions_;
                }
                else
                {
                    free_groups_.pop_back();
                }
                auto& group = groups_[index];
                group.key = key;
                found = indices_.emplace(key, index).first;
                descriptions_[index].generation = source_.revision + 1;
                source_changed = true;
            }
            auto& group = groups_[found->second];
            membership = {found->second, static_cast<std::uint32_t>(group.members.size()), selection->material};
            group.members.push_back(local);
            if (group.members.size() > group.capacity)
            {
                group.capacity = std::bit_ceil(static_cast<std::uint32_t>(group.members.size()));
            }
            auto& description = descriptions_[found->second];
            description.selection = {selection->mesh, memberships_[group.members.front()].material, selection->submesh};
            description.record.candidate_count = static_cast<std::uint32_t>(group.members.size());
        }
        source_.revision += source_changed;
        ++data_revision_;
    }

    RenderResult<void> MeshDrawGroups::pack() noexcept
    {
        if (error_.type != 0)
        {
            return cxx::unexpected(error_);
        }
        if (packed_revision_ == data_revision_)
        {
            return {};
        }
        descriptions_.resize(groups_.size());
        records_.clear();
        candidates_.clear();
        chunks_.clear();
        std::uint64_t visible_words = 0;
        for (std::uint32_t index = 0; index < groups_.size(); ++index)
        {
            const auto& group = groups_[index];
            if (group.members.empty())
            {
                descriptions_[index].gpu_index = UINT32_MAX;
                continue;
            }
            const auto [mesh, program, section] = group.key;
            auto storage =
                resources_.borrow<IndexedTriangleMeshStorage>(extension_detail::ResourceIdentity::parent(mesh));
            const auto* geometry = storage ? (*storage)->find(mesh) : nullptr;
            if (!geometry || section >= geometry->submeshes.size())
            {
                return cxx::unexpected(RenderError{kStorageIdentity});
            }
            const auto stride = meshVisibleStride(group.capacity) / 4;
            const auto extent = stride * source_.view_count * kMeshWindingCount;
            const bool exceeds_offsets =
                visible_words + extent > UINT32_MAX || candidates_.size() + group.members.size() > UINT32_MAX;
            if (exceeds_offsets)
            {
                return cxx::unexpected(RenderError{kStorageCapacity});
            }
            const auto& submesh = geometry->submeshes[section];
            auto buffers = (*storage)->bufferViews(mesh);
            if (!buffers) return cxx::unexpected(buffers.error());
            const auto index_base = buffers->indices.offset / buffers->index_stride;
            const auto vertex_base = buffers->vertices.offset / buffers->vertex_stride;
            const bool invalid_index = index_base > UINT32_MAX || submesh.first_index > UINT32_MAX - index_base;
            if (invalid_index || vertex_base > std::uint64_t(UINT32_MAX))
                return cxx::unexpected(RenderError{kStorageCapacity});
            const auto vertex_offset = std::int64_t(vertex_base) + submesh.vertex_offset;
            if (vertex_offset < INT32_MIN || vertex_offset > INT32_MAX)
                return cxx::unexpected(RenderError{kStorageCapacity});
            const auto gpu_index = static_cast<std::uint32_t>(records_.size());
            auto& record = records_.emplace_back();
            record = {
                submesh.index_count,
                static_cast<std::uint32_t>(group.members.size()),
                static_cast<std::uint32_t>(index_base + submesh.first_index),
                static_cast<std::int32_t>(vertex_offset),
                static_cast<std::uint32_t>(candidates_.size()),
                static_cast<std::uint32_t>(visible_words),
                static_cast<std::uint32_t>(stride),
                source_.view_count,
                geometry->bounds
            };
            const auto material = memberships_[group.members.front()].material;
            descriptions_[index].selection = {mesh, material, section};
            descriptions_[index].record = record;
            descriptions_[index].gpu_index = gpu_index;
            candidates_.insert(candidates_.end(), group.members.begin(), group.members.end());
            for (std::uint32_t first = 0; first < group.members.size(); first += 64)
            {
                chunks_.push_back({gpu_index, first});
            }
            visible_words += extent;
        }
        visible_bytes_ = std::max<std::uint64_t>(visible_words * 4, 256);
        const auto stream_count = std::uint64_t(records_.size()) * source_.view_count * kMeshWindingCount;
        const bool exceeds_dispatch = chunks_.size() > UINT32_MAX || stream_count > UINT32_MAX;
        if (exceeds_dispatch)
        {
            return cxx::unexpected(RenderError{kStorageCapacity});
        }
        dispatch_ = {
            static_cast<std::uint32_t>(chunks_.size()),
            source_.view_count,
            1,
            static_cast<std::uint32_t>((stream_count + 63) / 64),
            1,
            1
        };
        // Empty storage still needs a legal descriptor range; zero dispatches never read this sentinel.
        if (records_.empty())
        {
            records_.emplace_back();
            records_.front().view_count = source_.view_count;
        }
        if (candidates_.empty())
        {
            candidates_.push_back(0);
        }
        if (chunks_.empty())
        {
            chunks_.push_back({});
        }
        source_.groups = descriptions_;
        packed_revision_ = data_revision_;
        return {};
    }

    RenderResult<FeatureBinding::Preparation>
    MeshDrawGroups::prepare(const FeatureFrameInput& input, ResourceUploader& uploader) noexcept
    {
        const auto view_count = static_cast<std::uint32_t>(input.resources.front().size / sizeof(MeshVisibilityView));
        if (source_.view_count != view_count)
        {
            source_.view_count = view_count;
            ++data_revision_;
        }
        if (auto packed = pack(); !packed)
        {
            return cxx::unexpected(packed.error());
        }
        auto& slice = slices_[input.slot];
        const std::array<std::span<const std::byte>, 4> inputs{
            std::as_bytes(std::span(records_)),
            std::as_bytes(std::span(candidates_)),
            std::as_bytes(std::span(chunks_)),
            std::as_bytes(std::span(dispatch_))
        };
        // A declared View always exposes both winding records, even while no instances are alive.
        const auto streams = std::max<std::uint64_t>(indices_.size(), 1) * source_.view_count * kMeshWindingCount;
        const std::array<std::uint64_t, 6> sizes{
            inputs[0].size(),
            inputs[1].size(),
            inputs[2].size(),
            visible_bytes_,
            std::max<std::uint64_t>(streams * sizeof(nri::DrawIndexedDesc), 20),
            sizeof(dispatch_)
        };
        for (std::size_t i = 0; i < sizes.size(); ++i)
        {
            const auto bytes = std::max<std::uint64_t>(sizes[i], 4);
            auto*& buffer = slice.buffers[i];
            if (!buffer || device_.nriCore().GetBufferDesc(*buffer).size < bytes)
            {
                const auto capacity = std::bit_ceil(bytes);
                if (capacity > device_.description().memory.storageBufferMaxRange)
                {
                    return cxx::unexpected(RenderError{kStorageCapacity});
                }
                nri::Buffer* candidate = nullptr;
                const nri::BufferDesc description{
                    capacity,
                    4,
                    (nri::BufferUsageBits::SHADER_RESOURCE | nri::BufferUsageBits::SHADER_RESOURCE_STORAGE) | nri::BufferUsageBits::ARGUMENT
                };
                const auto created =
                    device_.nriCore().CreatePlacedBuffer(device_.device(), NriDeviceHeap, description, candidate);
                if (created != nri::Result::SUCCESS)
                {
                    return cxx::unexpected(nativeError(created));
                }
                releaseAfterGpu(device_, buffer);
                buffer = candidate;

                slice.revision = 0;
            }
            slice.updates[i] = {configuration_.outputs[i], {buffer, 0, bytes}};
        }
        slice.updates[0].input.parameters = std::as_bytes(std::span(&source_, 1));
        if (slice.revision == data_revision_)
        {
            return FeatureBinding::Preparation{FeatureBinding::EPreparation::READY, slice.updates};
        }
        slice.pending_revision = 0;
        bool staged_any = false;
        cxx::scope_exit register_copy(
            [&]() noexcept
            {
                if (!staged_any)
                {
                    return;
                }
                uploader.onPreparationSubmitted(
                    &slice,
                    [](void* data, std::uint32_t, RenderResult<void> accepted) noexcept
                    {
                        auto& state = *static_cast<Slice*>(data);
                        if (accepted)
                        {
                            if (state.pending_revision)
                            {
                                state.revision = state.pending_revision;
                            }
                        }
                        state.pending_revision = 0;
                    }
                );
            }
        );
        constexpr std::array<std::uint32_t, 4> targets{0, 1, 2, 5};
        for (std::size_t i = 0; i < inputs.size(); ++i)
        {
            if (inputs[i].empty())
            {
                continue;
            }
            auto copied = uploader.stageBuffer({slice.buffers[targets[i]], 0, inputs[i]});
            if (!copied)
            {
                return cxx::unexpected(copied.error());
            }
            staged_any = true;
        }
        slice.pending_revision = data_revision_;
        return FeatureBinding::Preparation{FeatureBinding::EPreparation::RECORD, slice.updates};
    }

    RenderResult<FeatureBinding> MeshDrawGroups::create(
        DeviceSession& device,
        FrameExecution& frames,
        ResourceRegistry& registry,
        RenderGraphInstance&,
        CompiledPrograms&,
        std::span<const ResourceInstanceId> storages,
        const void* input
    ) noexcept
    {
        const auto& configuration = *static_cast<const MeshDrawGroupsConfiguration*>(input);
        auto instances = registry.borrow<RasterMeshInstanceStorage>(storages[configuration.instances.index]);
        if (!instances)
        {
            return cxx::unexpected(RenderError{kStorageIdentity});
        }
        auto value = std::unique_ptr<MeshDrawGroups>(new MeshDrawGroups(device, frames, registry, configuration));
        value->observer_.receiver = value.get();
        value->observer_.changed = [](void* data, MeshInstanceId id, const RasterMeshInstanceStorage::Selection* item
                                   ) noexcept { static_cast<MeshDrawGroups*>(data)->update(id, item); };
        (*instances)->observe(value->observer_);
        (*instances)->forEach([&](MeshInstanceId id, const auto& item) noexcept { value->update(id, &item); });
        if (auto packed = value->pack(); !packed)
        {
            return cxx::unexpected(packed.error());
        }
        for (auto& slice : value->slices_)
        {
            slice.updates[0] = {configuration.outputs[0], {}};
            slice.updates[0].input.parameters = std::as_bytes(std::span(&value->source_, 1));
        }
        auto* source = value.get();
        FeatureBinding binding{
            {value.release(), [](void* data) noexcept { delete static_cast<MeshDrawGroups*>(data); }},
            FeatureBinding::PrepareResources{[](void* data, const FeatureFrameInput& frame, ResourceUploader& uploader
                                             ) noexcept
                                             { return static_cast<MeshDrawGroups*>(data)->prepare(frame, uploader); }}
        };
        binding.initial_resources = std::span(&source->slices_[0].updates[0], 1);
        // View count may change without an instance membership mutation (dynamic shadow slices).
        // This short check runs per frame; packing and upload still use their actual dirty revision.
        return binding;
    }
} // namespace lux::render::detail
