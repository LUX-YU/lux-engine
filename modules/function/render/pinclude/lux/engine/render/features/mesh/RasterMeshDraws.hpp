#pragma once

#include <lux/engine/render/device/CompiledPrograms.hpp>

#include <lux/engine/render/extensions/graph/InstanceData.hpp>
#include <lux/engine/render/features/mesh/MeshDrawGroups.hpp>
#include <lux/engine/render/resource/mesh/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>

namespace lux::render::detail
{
    class RenderGraphInstance;

    struct CompiledPass;
    struct RasterDrawList;

    struct MeshDrawConfiguration final
    {
        GraphPassId pass;
        // Source ports: groups, transforms, properties, alive, visible, commands.
        std::array<GraphResourceId, 6> inputs;
        rdesc::EMaterialPurpose purpose = rdesc::EMaterialPurpose::GBUFFER;
        bool front_counter_clockwise = true;
        GraphResourceId views, raster_states, depth;
    };

    struct MeshDrawSettings final
    {
        rdesc::EMaterialPurpose purpose;
        bool front_counter_clockwise;
        bool layered;
        std::uint32_t depth_slot;
    };

    // Actual per-Attachment draw preparation. Immutable native programs are borrowed from the
    // Device cache; Instance owns private bindings and packets for the selected physical FIF slice.
    class RasterMeshDraws final : public InstanceData
    {
    public:
        [[nodiscard]] static RenderResult<std::unique_ptr<InstanceData>>
        create(
            DeviceSession&,
            ResourceRegistry&,
            InstanceAccess&,
            std::span<const ResourceInstanceId>,
            const void*
        ) noexcept;
        ~RasterMeshDraws() noexcept;

    private:
        struct Program final
        {
            PassBindings bindings;
            CompiledPrograms::Program* program = nullptr;
        };

        struct Group final
        {
            std::uint64_t generation = 0;
            std::array<std::uint32_t, 2> programs{};
            IndexedTriangleMeshStorage::BufferViews buffers;
        };

        struct Slice final
        {
            std::uint32_t view_count = 0;
            std::uint64_t draw_revision = 0;
        };

        RasterMeshDraws(
            DeviceSession&,
            ResourceRegistry&,
            RenderGraphInstance&,
            CompiledPrograms&,
            const MeshDrawGroupSource&,
            MeshDrawSettings,
            const CompiledPass&
        ) noexcept;
        [[nodiscard]] bool requiresPrograms() const noexcept override;
        [[nodiscard]] std::uint64_t programRevision() const noexcept override;
        void rebuildDrawOrder() noexcept;
        [[nodiscard]] exec::task<RenderResult<bool>>
        preparePrograms(execution::CpuScheduler, std::uint64_t, stdexec::inplace_stop_token) noexcept override;
        [[nodiscard]] RenderResult<bool>
        prepare(const InstanceInputs&, ResourceUploader&, detail::Preparation&) noexcept override;
        // Refreshes current FIF bindings; true means the packet structure was rebuilt and needs merging.
        [[nodiscard]] RenderResult<bool>
        refreshDrawPackets(const InstanceInputs&, Slice&, RasterDrawList&) noexcept;
        [[nodiscard]] RenderResult<void>
        resolveFrameResources(const InstanceInputs&, RasterDrawList&) noexcept;
        void mergeDrawPackets(RasterDrawList&) noexcept;
        void updateViewParameters(const InstanceInputs&, RasterDrawList&) noexcept;
        DeviceSession& device_;
        ResourceRegistry& resources_;
        RenderGraphInstance& instance_;
        CompiledPrograms& programs_;
        const MeshDrawGroupSource& source_;
        MeshDrawSettings settings_;
        const CompiledPass& pass_;
        std::vector<Group> groups_;
        std::vector<std::unique_ptr<Program>> program_bindings_;
        std::vector<std::uint32_t> draw_order_;
        std::vector<ResourceInput> read_buffers_;
        std::vector<const nri::Texture*> read_images_;
        std::array<Slice, 3> slices_;
        std::uint64_t prepared_revision_ = 0, draw_layout_revision_ = 1;
    };
} // namespace lux::render::detail
