#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/graph/GraphResource.hpp>
#include <lux/engine/render/resource/instance/Data.hpp>
#include <lux/engine/render/resource/mesh/Data.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <span>
#include <tuple>
#include <vector>

namespace lux::render { class ResourceRegistry; }

namespace lux::render::detail
{
    // The instance storage updates membership only when an instance mutation commits.
    class MeshGrouping final
    {
    public:
        void update(std::uint32_t slot, const MeshInstanceData&, MaterialProgramId, MaterialId previous) noexcept;
        void remove(std::uint32_t slot, MaterialId) noexcept;
        // These bytes are borrowed only while staging the selected scene projections.
        using UploadSources = std::array<std::span<const std::byte>, 4>;
        [[nodiscard]] RenderResult<UploadSources> uploadSources(std::uint32_t dispatch_limit) const & noexcept;
        RenderResult<UploadSources> uploadSources(std::uint32_t) const && = delete;
        [[nodiscard]] const MeshDrawTopology& selectSurfaces(std::uint64_t revision) & noexcept;
        [[nodiscard]] std::uint64_t visibleBytes() const noexcept { return std::max<std::uint64_t>(visible_bytes_, 256); }
        [[nodiscard]] std::size_t drawGroupCount() const noexcept { return std::max<std::size_t>(indices_.size(), 1); }
        [[nodiscard]] bool needsPublication() const noexcept { return groups_dirty_; }
        // Byte intervals follow the CANDIDATES, CHUNKS, CAMERA_GROUPS and CAMERA_DISPATCH projection order.
        using ProjectionChanges = std::array<GraphBufferRange, 4>;
        [[nodiscard]] RenderResult<ProjectionChanges> publish(const ResourceRegistry&, std::uint64_t revision) noexcept;

    private:
        using GroupKey = std::tuple<MeshId, MaterialProgramId, std::uint32_t>;

        struct Membership final
        {
            std::uint32_t group = UINT32_MAX, position = 0;
        };

        struct Group final
        {
            GroupKey key;
            std::vector<std::uint32_t> members;
            std::size_t capacity = 0;
            // A membership edit invalidates the slice previously packed at this offset.
            std::uint32_t published_offset = UINT32_MAX;
        };

        using ProgramUse = std::pair<MeshId, MaterialProgramId>;
        struct SurfaceMaterials final
        {
            std::map<MaterialId, std::size_t> counts;
            std::uint32_t source = UINT32_MAX;
        };
        void releaseMaterial(const GroupKey&, MaterialId) noexcept;
        // Submeshes of the same geometry and Program share a draw source and its distinct material reads.
        std::map<ProgramUse, SurfaceMaterials> materials_;
        bool group_sources_dirty_ = true;
        bool material_reads_dirty_ = true;
        std::map<GroupKey, std::uint32_t> indices_;
        std::vector<Group> groups_;
        MeshDrawTopology topology_;
        std::vector<std::uint32_t> candidates_;
        std::vector<std::array<std::uint32_t, 2>> chunks_;
        std::vector<MeshDrawGroupRecord> draft_records_;
        std::vector<std::uint32_t> free_groups_;
        std::vector<Membership> memberships_;
        bool groups_dirty_ = true;
        std::uint64_t visible_bytes_ = 0;
    };

    // A View determines the count and offsets of visible streams. Candidate slots and chunks stay scene-owned.
    class MeshVisibilityLayout final
    {
    public:
        // A single View borrows the Scene rows for this synchronous call. Other layer counts use scaled rows.
        [[nodiscard]] RenderResult<std::span<const MeshDrawGroupRecord>>
        select(const MeshDrawTopology&, std::uint32_t views, std::array<std::uint32_t, 2> dispatch_limit) noexcept;

        [[nodiscard]] const std::array<std::uint32_t, 6>& dispatch() const & noexcept { return dispatch_; }
        [[nodiscard]] std::uint64_t visibleBytes() const noexcept { return visible_bytes_; }
        [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
        [[nodiscard]] std::uint64_t dispatchRevision() const noexcept { return dispatch_revision_; }

    private:
        std::vector<MeshDrawGroupRecord> records_;
        std::array<std::uint32_t, 6> dispatch_{};
        std::uint64_t visibility_revision_ = 0, revision_ = 0, dispatch_revision_ = 0, visible_bytes_ = 0;
        std::uint32_t view_count_ = 0;
    };
} // namespace lux::render::detail
