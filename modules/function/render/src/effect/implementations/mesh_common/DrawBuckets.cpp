#include <lux/engine/render/resource/mesh/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/effect/implementations/mesh_common/DrawBuckets.hpp>
#include <lux/engine/render/effect/implementations/mesh_visibility/VisibilityInputs.hpp>
#include <lux/engine/render/memory/DirtyRanges.hpp>
#include <algorithm>
#include <bit>

namespace lux::render::detail
{
    namespace
    {
        std::uint32_t chunkCount(std::uint32_t members) noexcept
        {
            return members / 64 + (members % 64 != 0);
        }

        template <class T>
        GraphBufferRange changedBytes(std::span<const T> old, std::span<const T> next) noexcept
        {
            const auto mismatch = std::ranges::mismatch(old, next);
            const auto first = static_cast<std::size_t>(mismatch.in2 - next.begin());
            auto end = next.size();
            if (old.size() == end)
                while (end > first && old[end - 1] == next[end - 1]) --end;
            return {first * sizeof(T), (end - first) * sizeof(T)};
        }

        // The group records have already passed validation. Patch the published rows in place.
        template <class T> struct PackedRows final
        {
            std::vector<T>& rows;
            std::size_t written = 0;
            GraphBufferRange dirty{};

            void append(std::span<const T> values) noexcept
            {
                const auto remaining = std::span<const T>{rows}.subspan(written);
                auto change = changedBytes(remaining.first(std::min(remaining.size(), values.size())), values);
                if (change.size)
                {
                    const auto end = written + values.size();
                    if (end > rows.size()) rows.resize(end);
                    const auto source = std::as_bytes(values).subspan(change.offset, change.size);
                    change.offset += written * sizeof(T);
                    const auto destination = std::as_writable_bytes(std::span{rows}).subspan(change.offset, change.size);
                    std::ranges::copy(source, destination.begin());
                    markDirtyBytes(dirty, change);
                }
                written += values.size();
            }

            GraphBufferRange finish() noexcept
            {
                if (!written && !rows.empty())
                {
                    // Empty structured buffers expose one zero record to the GPU.
                    const T sentinel{};
                    append({&sentinel, 1});
                    written = 0;
                }
                rows.resize(written);
                return dirty;
            }
        };
    }

    void MeshGrouping::releaseMaterial(const GroupKey& key, MaterialId id) noexcept
    {
        const auto source = materials_.find({std::get<0>(key), std::get<1>(key)});
        if (source == materials_.end()) std::terminate();
        auto& counts = source->second.counts;
        const auto entry = counts.find(id);
        if (entry == counts.end()) std::terminate();
        if (--entry->second) return;
        counts.erase(entry);
        if (counts.empty()) materials_.erase(source);
        material_reads_dirty_ = true;
    }

    void MeshGrouping::remove(std::uint32_t slot, MaterialId material) noexcept
    {
        if (slot >= memberships_.size()) return;
        auto& member = memberships_[slot];
        if (member.group == UINT32_MAX) return;
        auto& group = groups_[member.group];
        releaseMaterial(group.key, material);
        const auto last = group.members.back();
        group.members[member.position] = last;
        memberships_[last].position = member.position;
        group.members.pop_back();
        group.published_offset = UINT32_MAX;
        if (group.members.empty())
        {
            indices_.erase(group.key);
            group_sources_dirty_ = true;
            visible_bytes_ -= meshVisibleStride(group.capacity) * kMeshWindingCount;
            group.capacity = 0;
            free_groups_.push_back(member.group);
        }
        member = {};
        groups_dirty_ = true;
    }

    void MeshGrouping::update(
        std::uint32_t slot,
        const MeshInstanceData& input,
        MaterialProgramId program,
        MaterialId previous
    ) noexcept
    {
        if (!input.render_enabled)
        {
            remove(slot, previous);
            return;
        }
        if (slot >= memberships_.size()) memberships_.resize(std::bit_ceil(std::size_t{slot} + 1));
        auto& member = memberships_[slot];
        const GroupKey key{input.mesh, program, input.submesh};
        const bool same_group = member.group != UINT32_MAX && groups_[member.group].key == key;
        if (same_group && previous == input.material) return;

        // Retain first so replacing the only material or moving its only submesh keeps the same program use.
        auto& instances = materials_[{input.mesh, program}].counts[input.material];
        if (instances++ == 0) material_reads_dirty_ = true;
        if (same_group)
        {
            releaseMaterial(key, previous);
            return;
        }

        remove(slot, previous);
        const auto [entry, inserted] = indices_.try_emplace(key);
        if (inserted)
        {
            group_sources_dirty_ = true;
            if (free_groups_.empty())
            {
                entry->second = static_cast<std::uint32_t>(groups_.size());
                groups_.emplace_back();
            }
            else
            {
                entry->second = free_groups_.back();
                free_groups_.pop_back();
            }
            groups_[entry->second].key = key;
        }
        auto& group = groups_[entry->second];
        member = {entry->second, static_cast<std::uint32_t>(group.members.size())};
        group.members.push_back(slot);
        group.published_offset = UINT32_MAX;
        if (group.members.size() > group.capacity)
        {
            const auto old_stride = group.capacity ? meshVisibleStride(group.capacity) : 0;
            group.capacity = std::bit_ceil(group.members.size());
            visible_bytes_ += (meshVisibleStride(group.capacity) - old_stride) * kMeshWindingCount;
        }
        groups_dirty_ = true;
    }

    RenderResult<MeshGrouping::ProjectionChanges>
    MeshGrouping::publish(const ResourceRegistry& resources, std::uint64_t revision) noexcept
    {
        auto& topology = topology_;
        const IndexedTriangleMeshStorage* meshes = nullptr;
        std::uint32_t visible_offset = 0, candidate_count = 0, chunk_count = 0;
        draft_records_.clear();
        for (const auto& group : groups_)
        {
            if (group.members.empty()) continue;
            const auto& [mesh, program, submesh] = group.key;
            if (group.capacity > UINT32_MAX || group.members.size() > UINT32_MAX - candidate_count)
                return cxx::unexpected(RenderError{kStorageCapacity});
            const auto stride = meshVisibleStride(static_cast<std::uint32_t>(group.capacity)) / sizeof(std::uint32_t);
            if (stride > (UINT32_MAX - visible_offset) / kMeshWindingCount)
                return cxx::unexpected(RenderError{kStorageCapacity});
            const auto owner = extension_detail::ResourceIdentity::parent(mesh);
            if (!meshes || meshes->id() != owner)
            {
                const auto found = resources.borrow<IndexedTriangleMeshStorage>(owner);
                if (!found) return cxx::unexpected(found.error());
                meshes = *found;
            }
            const auto geometry = meshes->arenaSection(mesh, submesh);
            if (!geometry) return cxx::unexpected(geometry.error());
            const auto count = static_cast<std::uint32_t>(group.members.size());
            draft_records_.push_back({
                geometry->section.index_count, count, geometry->section.first_index, geometry->section.vertex_offset,
                candidate_count, visible_offset, static_cast<std::uint32_t>(stride), 1, geometry->bounds
            });
            visible_offset += static_cast<std::uint32_t>(stride * kMeshWindingCount);
            candidate_count += count;
            chunk_count += chunkCount(count);
        }
        if (draft_records_.empty()) draft_records_.emplace_back();
        const auto streams = std::uint64_t{draft_records_.size()} * kMeshWindingCount;
        if (streams > UINT32_MAX) return cxx::unexpected(RenderError{kStorageCapacity});
        const std::array<std::uint32_t, 6> dispatch{
            chunk_count, 1, 1, static_cast<std::uint32_t>((streams + 63) / 64), 1, 1
        };

        PackedRows candidates{candidates_};
        PackedRows chunks{chunks_};
        const bool count_changed = topology.draw_groups.size() != draft_records_.size();
        bool raster_layout_changed = count_changed;
        std::uint32_t index = 0, old_chunk_offset = 0;
        for (auto& group : groups_)
        {
            if (group.members.empty()) continue;
            const auto& record = draft_records_[index];
            // Each visible offset is the sum of preceding group strides. Candidate offsets only address culling inputs.
            if (!raster_layout_changed)
                raster_layout_changed = record.visible_stride != topology.draw_groups[index].visible_stride;
            if (group.published_offset == record.candidate_offset)
            {
                candidates.written += record.candidate_count;
            }
            else
            {
                candidates.append(group.members);
                group.published_offset = record.candidate_offset;
            }
            const auto old_count = index < topology.draw_groups.size() ?
                chunkCount(topology.draw_groups[index].candidate_count) : 0u;
            const auto count = chunkCount(record.candidate_count);
            if (old_chunk_offset == chunks.written && old_count == count)
            {
                chunks.written += count;
            }
            else
            {
                for (std::size_t first = 0; first < group.members.size(); first += 64)
                {
                    const std::array chunk{index, static_cast<std::uint32_t>(first)};
                    chunks.append({&chunk, 1});
                }
            }
            old_chunk_offset += old_count;
            ++index;
        }
        const ProjectionChanges changes{
            candidates.finish(), chunks.finish(),
            changedBytes<MeshDrawGroupRecord>(topology.draw_groups, draft_records_),
            changedBytes<std::uint32_t>(topology.camera_dispatch, dispatch)
        };
        const bool records_changed = count_changed || changes[2].size != 0;
        if (records_changed) topology.draw_groups.swap(draft_records_);
        draft_records_.clear();
        topology.camera_dispatch = dispatch;
        if (records_changed || changes[3].size || !topology.visibility_revision) topology.visibility_revision = revision;
        if (raster_layout_changed || !topology.raster_layout_revision) topology.raster_layout_revision = revision;
        groups_dirty_ = false;
        return changes;
    }

    RenderResult<MeshGrouping::UploadSources> MeshGrouping::uploadSources(std::uint32_t dispatch_limit) const & noexcept
    {
        const auto& dispatch = topology_.camera_dispatch;
        const auto x = std::max(dispatch[0], dispatch[3]);
        if (x > dispatch_limit) return cxx::unexpected(deviceBudget(EDeviceBudget::DISPATCH_X, x, dispatch_limit));
        const auto bytes = []<class T>(const std::vector<T>& rows) noexcept
        {
            // Even an empty dispatch binds a valid structured buffer descriptor.
            static constexpr T sentinel{};
            return std::as_bytes(rows.empty() ? std::span{&sentinel, 1} : std::span{rows});
        };
        return UploadSources{
            bytes(candidates_), bytes(chunks_), bytes(topology_.draw_groups), std::as_bytes(std::span{dispatch})
        };
    }

    const MeshDrawTopology& MeshGrouping::selectSurfaces(std::uint64_t revision) & noexcept
    {
        auto& selected = topology_;
        if (group_sources_dirty_)
        {
            // First use in GPU group order also determines the draw order for blended materials.
            std::map<MeshId, std::uint32_t> vertices;
            for (auto& entry : materials_) entry.second.source = UINT32_MAX;
            std::vector<MeshId> meshes;
            std::vector<MeshDrawTopology::MeshProgram> programs;
            bool remapped = selected.group_sources.size() != indices_.size();
            selected.group_sources.resize(indices_.size());
            std::size_t index = 0;
            for (const auto& group : groups_)
            {
                if (group.members.empty()) continue;
                const auto& [mesh, program, submesh] = group.key;
                const auto use = materials_.find({mesh, program});
                if (use == materials_.end()) std::terminate();
                auto& source = use->second.source;
                if (source == UINT32_MAX)
                {
                    source = static_cast<std::uint32_t>(programs.size());
                    const auto next_vertex = static_cast<std::uint32_t>(meshes.size());
                    const auto [vertex, new_mesh] = vertices.try_emplace(mesh, next_vertex);
                    if (new_mesh) meshes.push_back(mesh);
                    programs.push_back({vertex->second, program});
                }
                auto& previous = selected.group_sources[index++];
                remapped |= previous != source;
                previous = source;
            }
            if (remapped) selected.raster_layout_revision = revision;
            const bool changed = meshes != selected.meshes || programs != selected.mesh_programs;
            if (changed || !selected.program_use_revision)
            {
                selected.meshes = std::move(meshes);
                selected.mesh_programs = std::move(programs);
                selected.program_use_revision = revision;
                material_reads_dirty_ = true;
            }
            group_sources_dirty_ = false;
        }
        if (!material_reads_dirty_) return selected;
        selected.surfaces.clear();
        for (const auto& use : selected.mesh_programs)
        {
            const auto found = materials_.find({selected.meshes[use.mesh], use.program});
            if (found == materials_.end()) std::terminate();
            const auto& materials = found->second;
            for (const auto& entry : materials.counts) selected.surfaces.push_back({materials.source, entry.first});
        }
        selected.surface_revision = revision;
        material_reads_dirty_ = false;
        return selected;
    }

    RenderResult<std::span<const MeshDrawGroupRecord>> MeshVisibilityLayout::select(
        const MeshDrawTopology& topology, std::uint32_t views, std::array<std::uint32_t, 2> limit
    ) noexcept
    {
        const auto selected = [&]() noexcept -> std::span<const MeshDrawGroupRecord>
        {
            return views == 1 ? topology.draw_groups : records_;
        };
        if (topology.visibility_revision && topology.visibility_revision == visibility_revision_ && views == view_count_)
            return selected();
        const auto groups = std::uint64_t{topology.draw_groups.size()};
        const auto streams_per_view = groups * kMeshWindingCount;
        if (views && streams_per_view > UINT32_MAX / views) return cxx::unexpected(RenderError{kStorageCapacity});
        const auto compaction = (streams_per_view * views + 63) / 64;
        const auto work_groups = std::max<std::uint64_t>(topology.camera_dispatch[0], compaction);
        if (work_groups > limit[0]) return cxx::unexpected(deviceBudget(EDeviceBudget::DISPATCH_X, work_groups, limit[0]));
        if (views > limit[1]) return cxx::unexpected(deviceBudget(EDeviceBudget::DISPATCH_Y, views, limit[1]));

        const auto& last = topology.draw_groups.back();
        const auto words_per_view = last.visible_offset + std::uint64_t{last.visible_stride} * kMeshWindingCount;
        const auto words = words_per_view * views;
        if (words > UINT32_MAX) return cxx::unexpected(RenderError{kStorageCapacity});
        if (views != 1)
        {
            records_ = topology.draw_groups;
            for (auto& record : records_)
            {
                record.visible_offset *= views;
                record.view_count = views;
            }
        }
        else records_.clear();
        if (++revision_ == 0) std::terminate();
        const std::array<std::uint32_t, 6> dispatch{
            topology.camera_dispatch[0], views, 1, static_cast<std::uint32_t>(compaction), 1, 1
        };
        if (dispatch_ != dispatch)
        {
            dispatch_ = dispatch;
            dispatch_revision_ = revision_;
        }
        visible_bytes_ = std::max<std::uint64_t>(words * sizeof(std::uint32_t), 256);
        visibility_revision_ = topology.visibility_revision;
        view_count_ = views;
        return selected();
    }
} // namespace lux::render::detail
