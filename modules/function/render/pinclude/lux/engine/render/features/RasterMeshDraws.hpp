#pragma once

#include <lux/engine/render/extensions/features/FeatureBinding.hpp>
#include <lux/engine/render/features/MeshDrawGroups.hpp>
#include <lux/engine/render/resource/mesh/IndexedTriangleMeshStorage.hpp>
#include <lux/engine/render/graph/compile/ShaderBindingPlan.hpp>

namespace lux::render::detail
{
    struct RasterDrawList;

    struct MeshDrawConfiguration final
    {
        GraphPassId pass;
        // Source ports: groups, transforms, properties, alive, visible, commands.
        std::array<GraphResourceId, 6> inputs;
        rdesc::EMaterialPurpose purpose = rdesc::EMaterialPurpose::GBUFFER;
        bool paged = false;
        bool front_counter_clockwise = true;
        GraphResourceId views, raster_states, depth;
    };

    // Actual per-Attachment draw preparation. Immutable native programs are borrowed from the
    // Device cache; Instance owns private bindings and packets for the selected physical FIF slice.
    class RasterMeshDraws final
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
        ~RasterMeshDraws() noexcept;

    private:
        struct Program final
        {
            PassBindings bindings;
            nri::Pipeline* pipeline = nullptr;
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
            MeshDrawConfiguration
        ) noexcept;
        [[nodiscard]] const MeshDrawGroupSource& source() const noexcept;
        [[nodiscard]] bool requiresPrograms() const noexcept;
        void rebuildDrawOrder() noexcept;
        [[nodiscard]] exec::task<RenderResult<void>> preparePrograms(execution::CpuScheduler) noexcept;
        [[nodiscard]] RenderResult<FeatureBinding::Preparation>
        prepare(const FeatureFrameInput&, ResourceUploader&, detail::Preparation&) noexcept;
        // Refreshes current FIF bindings; true means the packet structure was rebuilt and needs merging.
        [[nodiscard]] RenderResult<bool>
        refreshDrawPackets(const FeatureFrameInput&, Slice&, RasterDrawList&) noexcept;
        [[nodiscard]] RenderResult<void>
        resolveFrameResources(const FeatureFrameInput&, RasterDrawList&) noexcept;
        void mergeDrawPackets(RasterDrawList&) noexcept;
        void updateViewParameters(const FeatureFrameInput&, RasterDrawList&) noexcept;
        DeviceSession& device_;
        ResourceRegistry& resources_;
        RenderGraphInstance& instance_;
        CompiledPrograms& programs_;
        MeshDrawConfiguration configuration_;
        std::vector<Group> groups_;
        std::vector<std::unique_ptr<Program>> program_bindings_;
        std::vector<std::uint32_t> draw_order_;
        std::vector<MaterialId> material_sources_;
        std::vector<ResourceInput> read_buffers_;
        std::vector<const nri::Texture*> read_images_;
        std::array<std::uint32_t, 2> camera_offsets_{};
        std::array<Slice, 3> slices_;
        std::uint64_t prepared_revision_ = 0, draw_layout_revision_ = 1;
    };
} // namespace lux::render::detail
