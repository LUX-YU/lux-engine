#pragma once

#include <lux/engine/render/effect/FrameAlgorithm.hpp>
#include <lux/engine/render/effect/implementations/mesh_common/IndexedDraws.hpp>
#include <lux/engine/render/resource/instance/Data.hpp>
#include <lux/engine/render/resource/material/Resource.hpp>

namespace lux::render
{
    struct RasterState;
}

namespace lux::render::detail
{
    struct CpuDrawStorage;
    class IndexedTriangleMeshStorage;
    class SampledTexture2DStorage;
    class MeshSurfacePrograms;

    struct MeshProgramBinding final
    {
        ResourceInstanceId instances;
        const IndexedTriangleMeshStorage* geometry;
        rdesc::EMaterialPurpose purpose;
        bool front_counter_clockwise;
        bool operator==(const MeshProgramBinding&) const noexcept = default;
    };

    class MeshSurfaceStream final
    {
    public:
        struct Configuration final
        {
            std::array<GraphResourceId, 6> buffers;
            bool shared_members, front_counter_clockwise, texture_descriptors;
            rdesc::EMaterialPurpose purpose;
        };

        struct Inputs final
        {
            DrawStreamSlot stream;
            RasterResourceSlot geometry;
            RasterResourceSlot materials;
            std::optional<RasterResourceSlot> textures;
            std::array<RasterBufferSlot, 6> buffers;
            std::array<std::uint32_t, 5> arguments;
            bool shared_members, front_counter_clockwise;
            rdesc::EMaterialPurpose purpose;
        };

        [[nodiscard]] static RenderResult<Inputs> resolve(const Configuration&, const RasterOperands&) noexcept;
        [[nodiscard]] static RenderResult<MeshSurfaceStream> bind(const Inputs&, const InstalledDrawSources&) noexcept;
        [[nodiscard]] RenderResult<void>
        prepare(const MeshDrawTopology&, std::span<const MeshDrawGroupRecord>, std::span<const RasterState>) noexcept;

    private:
        MeshSurfaceStream(
            const Inputs& inputs,
            const SampledTexture2DStorage* textures,
            const MeshSurfacePrograms& programs,
            CpuDrawStorage& output
        ) noexcept : programs_(programs), inputs_(inputs), textures_(textures), output_(output) {}

        struct LayoutVersion final
        {
            std::uint64_t groups, mesh_programs, programs, mapping;
            std::uint32_t views;
            bool operator==(const LayoutVersion&) const noexcept = default;
        };

        [[nodiscard]] RenderResult<void>
        buildLayout(const MeshDrawTopology&, std::span<const MeshDrawGroupRecord>, LayoutVersion) noexcept;
        [[nodiscard]] RenderResult<void> bindGeometry(std::span<const MeshId>) noexcept;

        const MeshSurfacePrograms& programs_;
        std::optional<LayoutVersion> layout_version_;
        const Inputs& inputs_;
        const SampledTexture2DStorage* textures_;
        CpuDrawStorage& output_;
    };
}
