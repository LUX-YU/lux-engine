#pragma once

#include <lux/engine/render/extensions/execution/InstanceLink.hpp>
#include <lux/engine/render/resources/instance/Data.hpp>
#include <lux/engine/render/resources/material/Resource.hpp>
#include <map>

namespace lux::render
{
    class RasterProgramBuildContext;
    struct ViewGpuParameters;
    struct MeshVisibilityView;
    struct RasterState;
}

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
    class IndexedTriangleMeshStorage;

    class MeshSurfaceStream final
    {
    public:
        struct Configuration final
        {
            GraphPassId pass;
            std::array<GraphResourceId, 5> buffers;
            bool paged, shared_members, front_counter_clockwise, texture_descriptors;
            rdesc::EMaterialPurpose purpose;
        };

        struct Inputs final
        {
            DrawStreamSlot stream;
            DrawResourceSlot<IndexedTriangleMeshStorage> geometry;
            MaterialDrawSources material;
            std::array<RasterBufferSlot, 5> buffers;
            std::array<std::uint32_t, 4> arguments;
            bool paged, shared_members, front_counter_clockwise;
            rdesc::EMaterialPurpose purpose;
        };

        struct ProgramBinding final
        {
            std::uint32_t first;
            bool reads_material;
        };

        using GroupKey = std::pair<MeshId, MaterialProgramId>;
        struct DrawMapping final
        {
            std::map<GroupKey, ProgramBinding> groups;
            std::vector<std::pair<std::uint32_t, MaterialId>> materials;
            std::size_t program_count = 0;
        };

        struct DrawInputs final
        {
            FramePreparation& frame;
            const MeshDrawGroupSource& groups;
            const DrawMapping& mapping;
        };

        [[nodiscard]] static RenderResult<Inputs> resolve(const Configuration&, InstanceLinkContext&) noexcept;
        explicit MeshSurfaceStream(const Inputs& inputs) noexcept : inputs_(inputs) {}
        MeshSurfaceStream(const Inputs&&) = delete;
        [[nodiscard]] static RenderResult<std::unique_ptr<DrawMapping>>
        buildPrograms(
            const Inputs&,
            RasterProgramBuildContext&,
            const MeshInstanceSnapshot&,
            const DrawMapping*
        ) noexcept;

        [[nodiscard]] RenderResult<void> prepare(DrawInputs, const ViewGpuParameters&) noexcept;
        [[nodiscard]] RenderResult<void>
        prepareShadow(DrawInputs, std::span<const MeshVisibilityView>, std::span<const RasterState>) noexcept;

    private:
        struct GroupLayout final
        {
            MeshId mesh;
            MaterialProgramId program;
            std::uint64_t generation;
            std::uint32_t first_program, gpu_index, visible_offset, visible_stride;
            bool operator==(const GroupLayout&) const noexcept = default;
        };

        struct SelectedGroup final
        {
            GroupLayout layout;
            RasterIndexedGeometry geometry;
        };

        struct SectionInputs final
        {
            std::span<const VisibilityViewConstants> constants;
            std::span<const RasterState> states;
            bool depth_bias;
        };

        struct LayoutVersion final
        {
            std::uint64_t groups, programs;
            std::uint32_t views;
            bool operator==(const LayoutVersion&) const noexcept = default;
        };

        [[nodiscard]] RenderResult<void>
        prepareSections(DrawInputs, SectionInputs) noexcept;
        [[nodiscard]] RenderResult<void> buildLayout(FramePreparation&, std::uint32_t view_count) noexcept;

        MaterialRecordReader material_records_;
        std::optional<LayoutVersion> layout_version_;
        std::vector<GroupLayout> layout_groups_;

        const Inputs&                           inputs_;
        std::vector<RasterStreamDraw>           draws_;
        std::vector<RasterStreamBinding>        bindings_;
        std::vector<RasterStreamVertex>         vertices_;
        std::vector<SelectedGroup>              groups_;
        std::vector<RasterStreamSection>        sections_;
        std::vector<VisibilityViewConstants>    shadow_constants_;
    };
}
