#include <lux/engine/render/device/Error.hpp>
#include <lux/engine/render/features/mesh/MeshGrouping.hpp>
#include <lux/engine/render/features/mesh/MeshVisibilityInputs.hpp>
#include <algorithm>
#include <bit>

namespace lux::render::detail
{
    bool MeshGrouping::remove(std::uint32_t instance) noexcept
    {
        auto& membership = memberships_[instance];
        auto& group = groups_[membership.group];
        const auto last = group.members.back();
        group.members[membership.position] = last;
        memberships_[last].position = membership.position;
        group.members.pop_back();
        const bool source_changed = group.members.empty();
        if (source_changed)
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
        }
        membership.group = UINT32_MAX;
        return source_changed;
    }

    void MeshGrouping::update(std::uint32_t local, const Selection* selection) noexcept
    {
        const GroupKey key = selection ? GroupKey{selection->mesh, selection->program, selection->section}
                                       : GroupKey{};
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
                material_ids_dirty_ |= membership.material != selection->material;
                membership.material = selection->material;
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
            description.mesh = selection->mesh;
            description.program = selection->program;
            description.record.candidate_count = static_cast<std::uint32_t>(group.members.size());
        }
        source_.revision += source_changed;
        material_ids_dirty_ = true;
        ++data_revision_;
    }

    void MeshGrouping::setViewCount(std::uint32_t count) noexcept
    {
        if (source_.view_count == count) return;
        source_.view_count = count;
        ++data_revision_;
    }

    RenderResult<void>
    MeshGrouping::pack(const void* owner, ResolveGeometry resolve, std::array<std::uint32_t, 2> dispatch_limit) noexcept
    {
        if (material_ids_dirty_)
        {
            material_ids_.clear();
            for (const auto& group : groups_)
            {
                for (auto slot : group.members)
                {
                    material_ids_.push_back(memberships_[slot].material);
                }
            }
            std::sort(material_ids_.begin(), material_ids_.end());
            const auto duplicates = std::ranges::unique(material_ids_);
            material_ids_.erase(duplicates.begin(), duplicates.end());
            source_.materials = material_ids_;
            material_ids_dirty_ = false;
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
            const auto& [mesh, program, section] = group.key;
            const auto geometry = resolve(owner, mesh, section);
            if (!geometry) return cxx::unexpected(geometry.error());
            const auto stride = meshVisibleStride(group.capacity) / 4;
            const auto extent = stride * source_.view_count * kMeshWindingCount;
            const bool exceeds_offsets =
                visible_words + extent > UINT32_MAX || candidates_.size() + group.members.size() > UINT32_MAX;
            if (exceeds_offsets)
            {
                return cxx::unexpected(RenderError{kStorageCapacity});
            }
            const auto gpu_index = static_cast<std::uint32_t>(records_.size());
            auto& record = records_.emplace_back();
            record = {
                geometry->section.index_count,
                static_cast<std::uint32_t>(group.members.size()),
                geometry->section.first_index,
                geometry->section.vertex_offset,
                static_cast<std::uint32_t>(candidates_.size()),
                static_cast<std::uint32_t>(visible_words),
                static_cast<std::uint32_t>(stride),
                source_.view_count,
                geometry->bounds
            };
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
        const auto stream_count = std::max<std::uint64_t>(records_.size(), 1) * source_.view_count * kMeshWindingCount;
        const bool exceeds_dispatch = chunks_.size() > UINT32_MAX || stream_count > UINT32_MAX;
        if (exceeds_dispatch)
        {
            return cxx::unexpected(RenderError{kStorageCapacity});
        }
        const auto compaction_groups = (stream_count + 63) / 64;
        const auto work_groups = std::max<std::uint64_t>(chunks_.size(), compaction_groups);
        const bool exceeds_x = work_groups > dispatch_limit[0];
        const bool exceeds_y = source_.view_count > dispatch_limit[1];
        if (exceeds_x || exceeds_y)
        {
            const auto axis = exceeds_x ? 0u : 1u;
            const auto requested = exceeds_x ? work_groups : source_.view_count;
            constexpr std::array budgets{
                EDeviceBudget::DISPATCH_X, EDeviceBudget::DISPATCH_Y, EDeviceBudget::DISPATCH_Z
            };
            return cxx::unexpected(deviceBudget(budgets[axis], requested, dispatch_limit[axis]));
        }
        dispatch_ = {
            static_cast<std::uint32_t>(chunks_.size()),
            source_.view_count,
            1,
            static_cast<std::uint32_t>(compaction_groups),
            1,
            1
        };
        // Compaction writes zero-instance commands from the sentinel when no groups are live.
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

    std::uint64_t MeshGrouping::indirectStreamCount() const noexcept
    {
        // The empty selection reserves one group per view for the shader's descriptor contract.
        return std::max<std::uint64_t>(indices_.size(), 1) * source_.view_count * kMeshWindingCount;
    }

    std::array<std::span<const std::byte>, 4> MeshGrouping::uploads() const noexcept
    {
        return {
            std::as_bytes(std::span(records_)),
            std::as_bytes(std::span(candidates_)),
            std::as_bytes(std::span(chunks_)),
            std::as_bytes(std::span(dispatch_))
        };
    }
} // namespace lux::render::detail
