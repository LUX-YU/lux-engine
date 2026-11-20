#pragma once

#include <lux/engine/render/extensions/execution/InstanceLink.hpp>
#include <lux/engine/render/resources/instance/Data.hpp>
#include <map>

namespace lux::render { struct MeshVisibilityView; struct RasterState; }

namespace lux::render::detail
{
    struct MeshDrawGroupSource;

    struct alignas(16) VisibilityViewConstants final
    {
        TransformMatrix view, projection;
    };
}

template <> struct lux::render::ConstantBlockTraits<lux::render::detail::VisibilityViewConstants> final
{
    static lux::render::ConstantBlockLayout layout(lux::rdesc::EShaderTarget) noexcept
    {
        using namespace lux::render;
        using Block = detail::VisibilityViewConstants;
        constexpr auto scalar = lux::rdesc::EDataScalar::FLOAT;
        constexpr auto order = lux::rdesc::EShaderMatrixOrder::COLUMN_MAJOR;
        static constexpr std::array members{
            ConstantMemberLayout{"view", offsetof(Block, view), scalar, 4, 4, 16, order},
            ConstantMemberLayout{"projection", offsetof(Block, projection), scalar, 4, 4, 16, order}
        };
        return {sizeof(Block), alignof(Block), members};
    }
};

namespace lux::render::detail
{
    class MeshSurfaceStream final
    {
    public:
        struct Configuration final
        {
            GraphPassId pass;
            std::array<GraphResourceId, 5> buffers;
            bool paged, shared_members, front_counter_clockwise;
            rdesc::EMaterialPurpose purpose;
        };

        struct Inputs final
        {
            DrawStreamSlot stream;
            std::array<RasterBufferSlot, 5> buffers;
            std::array<std::uint32_t, 4> arguments;
            bool paged, shared_members, front_counter_clockwise;
            rdesc::EMaterialPurpose purpose;
        };

        [[nodiscard]] static RenderResult<Inputs> resolve(const Configuration&, InstanceLinkContext&) noexcept;
        explicit MeshSurfaceStream(const Inputs& inputs) noexcept : inputs_(inputs) {}
        [[nodiscard]] RenderResult<void> selectPrograms(RasterProgramSelection&, const MeshInstanceSnapshot&) noexcept;
        void settlePrograms(bool adopted) noexcept;
        [[nodiscard]] RenderResult<void> prepare(
            FramePreparation&, const MeshDrawGroupSource&, const ViewGpuParameters&
        ) noexcept;
        [[nodiscard]] RenderResult<void> prepareShadow(
            FramePreparation&, const MeshDrawGroupSource&, std::span<const MeshVisibilityView>,
            std::span<const RasterState>
        ) noexcept;

    private:
        struct ProgramBinding final
        {
            std::uint32_t first;
            bool reads_material;
        };

        struct SelectedGroup final
        {
            RasterMeshGeometry geometry;
            std::uint32_t first_program, gpu_index, visible_offset, visible_stride;
        };

        [[nodiscard]] RenderResult<void> prepareSections(
            FramePreparation&, const MeshDrawGroupSource&, std::span<const VisibilityViewConstants>,
            std::span<const RasterState>, bool depth_bias
        ) noexcept;

        using GroupKey = std::pair<MeshId, MaterialProgramId>;
        struct ProgramSelectionState final
        {
            std::map<GroupKey, ProgramBinding> groups;
            std::vector<std::pair<std::uint32_t, MaterialId>> materials;
            std::size_t program_count = 0;
        };

        Inputs inputs_;
        std::optional<ProgramSelectionState> selected_programs_, pending_programs_;
        std::vector<RasterStreamDraw> draws_;
        std::vector<RasterStreamBinding> bindings_;
        std::vector<RasterStreamVertex> vertices_;
        std::vector<SelectedGroup> groups_;
        std::vector<RasterStreamSection> sections_;
        std::vector<VisibilityViewConstants> shadow_constants_;
    };
}
