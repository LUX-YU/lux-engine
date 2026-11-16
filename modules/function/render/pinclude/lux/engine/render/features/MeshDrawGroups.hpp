#pragma once

#include <lux/engine/render/extensions/features/FeatureBinding.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>
#include <lux/engine/render/storage/RasterMeshInstanceStorage.hpp>
#include <map>

namespace lux::render::detail
{
    // GPU input shared by the cull and compact stages. Offsets are uint words, not bytes.
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
        MeshInstanceDrawGroup selection;
        MeshDrawGroupRecord record;
        std::uint64_t generation = 0;
        std::uint32_t gpu_index = UINT32_MAX; // Dense live-record address; never the logical slot identity.
    };

    // CPU view of the same prepared group ordering. It carries only professional selection and
    // offsets; native programs belong to the Device cache and resources remain in their Storage.
    struct MeshDrawGroupSource final
    {
        std::span<const MeshDrawGroup> groups;
        std::uint64_t revision = 1;
        std::uint32_t view_count = 1;
    };

    struct MeshDrawGroupsConfiguration final
    {
        GraphResourceId instances;
        // groups, candidates, chunks, visible, indirect commands, dispatch arguments.
        std::array<GraphResourceId, 6> outputs;
        GraphResourceId views;
    };

    // One Attachment's derived grouping. Legacy MdcTable's live-key lookup, dead-slot recycling
    // and sticky power-of-two capacity bands are retained without its graph-recompile coupling.
    class MeshDrawGroups final
    {
    public:
        [[nodiscard]] static RenderResult<FeatureBinding>
        create(
            DeviceSession&,
            FrameExecution&,
            ResourceRegistry&,
            RenderGraphInstance&,
            CompiledPrograms&,
            std::span<const ResourceInstanceId>,
            const void*
        ) noexcept;
        ~MeshDrawGroups() noexcept;

    private:
        using GroupKey = std::tuple<MeshId, const rdesc::MaterialProgramSet*, std::uint32_t>;

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

        struct Slice final
        {
            std::array<nri::Buffer*, 6> buffers{};
            std::array<ResourceUpdate, 6> updates;
            std::uint64_t revision = 0, pending_revision = 0;
            std::size_t uploaded = 0, pending = 0;
        };

        MeshDrawGroups(DeviceSession&, FrameExecution&, ResourceRegistry&, MeshDrawGroupsConfiguration) noexcept;
        void update(MeshInstanceId, const RasterMeshInstanceStorage::Selection*) noexcept;
        [[nodiscard]] bool remove(std::uint32_t instance) noexcept;
        [[nodiscard]] RenderResult<void> pack() noexcept;
        [[nodiscard]] RenderResult<FeatureBinding::Preparation>
        prepare(const FeatureFrameInput&, ResourceUploader&) noexcept;

        DeviceSession& device_;
        FrameExecution& frames_;
        ResourceRegistry& resources_;
        MeshDrawGroupsConfiguration configuration_;
        RasterMeshInstanceStorage::Observer observer_;
        std::map<GroupKey, std::uint32_t> indices_;
        std::vector<Group> groups_;
        std::vector<std::uint32_t> free_groups_;
        std::vector<Membership> memberships_;
        std::vector<MeshDrawGroup> descriptions_;
        std::vector<MeshDrawGroupRecord> records_;
        std::vector<std::uint32_t> candidates_;
        std::vector<std::array<std::uint32_t, 2>> chunks_;
        std::array<std::uint32_t, 6> dispatch_{};
        std::array<Slice, 3> slices_;
        MeshDrawGroupSource source_;
        std::uint64_t data_revision_ = 1, packed_revision_ = 0;
        std::uint64_t visible_bytes_ = 4;
        RenderError error_;
    };
} // namespace lux::render::detail
