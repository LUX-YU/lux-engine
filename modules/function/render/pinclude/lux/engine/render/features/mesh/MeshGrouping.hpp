#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/resources/material/Data.hpp>
#include <lux/engine/render/resources/mesh/Data.hpp>
#include <array>
#include <cstddef>
#include <map>
#include <span>
#include <tuple>
#include <vector>

namespace lux::render::detail
{
    // The visibility kernels address these offsets in uint words.
    struct MeshDrawGroupRecord final
    {
        std::uint32_t index_count = 0, candidate_count = 0, first_index = 0;
        std::int32_t vertex_offset = 0;
        std::uint32_t candidate_offset = 0, visible_offset = 0, visible_stride = 0, view_count = 1;
        MeshGpuBounds bounds;
    };

    static_assert(sizeof(MeshDrawGroupRecord) == 64);

    struct MeshDrawGroup final
    {
        MeshId mesh;
        MaterialProgramId program;
        MeshDrawGroupRecord record;
        std::uint64_t generation = 0;
        std::uint32_t gpu_index = UINT32_MAX; // Dense live-record address.
    };

    struct MeshDrawGroupSource final
    {
        std::span<const MeshDrawGroup> groups;
        std::span<const MaterialId> materials;
        std::uint64_t revision = 1;
        std::uint32_t view_count = 1;
    };

    class MeshGrouping final
    {
    public:
        struct Selection final
        {
            MeshId mesh;
            MaterialId material;
            MaterialProgramId program;
            std::uint32_t section;
        };

        using ResolveGeometry = RenderResult<MeshArenaSection> (*)(const void*, MeshId, std::uint32_t) noexcept;

        MeshGrouping() noexcept = default;
        MeshGrouping(const MeshGrouping&) = delete;
        MeshGrouping& operator=(const MeshGrouping&) = delete;

        void update(std::uint32_t instance, const Selection*) noexcept;
        void setViewCount(std::uint32_t) noexcept;
        void invalidateSource() noexcept { ++source_.revision; }
        [[nodiscard]] RenderResult<void>
        pack(const void* owner, ResolveGeometry, std::array<std::uint32_t, 2> dispatch_limit) noexcept;
        [[nodiscard]] const MeshDrawGroupSource& source() const noexcept { return source_; }
        [[nodiscard]] std::uint64_t revision() const noexcept { return data_revision_; }
        [[nodiscard]] std::uint64_t visibleBytes() const noexcept { return visible_bytes_; }
        [[nodiscard]] std::uint64_t indirectStreamCount() const noexcept;
        [[nodiscard]] std::array<std::span<const std::byte>, 4> uploads() const noexcept;

    private:
        using GroupKey = std::tuple<MeshId, MaterialProgramId, std::uint32_t>;

        struct Membership final
        {
            std::uint32_t group = UINT32_MAX, position = 0;
            MaterialId material;
        };

        struct Group final
        {
            GroupKey key;
            std::vector<std::uint32_t> members;
            std::uint32_t capacity = 0;
        };

        [[nodiscard]] bool remove(std::uint32_t instance) noexcept;

        std::map<GroupKey, std::uint32_t> indices_;
        std::vector<Group> groups_;
        std::vector<std::uint32_t> free_groups_;
        std::vector<Membership> memberships_;
        std::vector<MeshDrawGroup> descriptions_;
        std::vector<MaterialId> material_ids_;
        std::vector<MeshDrawGroupRecord> records_;
        std::vector<std::uint32_t> candidates_;
        std::vector<std::array<std::uint32_t, 2>> chunks_;
        std::array<std::uint32_t, 6> dispatch_{};
        MeshDrawGroupSource source_;
        std::uint64_t data_revision_ = 1, packed_revision_ = 0;
        bool material_ids_dirty_ = true;
        std::uint64_t visible_bytes_ = 4;
    };
} // namespace lux::render::detail
