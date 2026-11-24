#pragma once

#include <lux/engine/render/device/PreparedRasterTable.hpp>
#include <lux/engine/render/resource/material/MeshPipelines.hpp>

namespace lux::render
{
    class ResourceRegistry;
    struct MeshVertexLayout;
}

namespace lux::render::detail
{
    struct MeshMaterialShader;
    class DeviceEntry;

    // One explicitly requested set for a material purpose. The material publishes it only after both windings
    // of every use have compiled. Jobs hold independent native references through their accepted frontiers.
    class PreparedMeshPipelines final
    {
    public:
        using Result = RenderResult<std::unique_ptr<const PreparedMeshPipelines>>;
        [[nodiscard]] static exec::task<Result>
        create(DeviceEntry&, const MaterialMeshPipelines&, const MeshMaterialShader&, const ResourceRegistry&) noexcept;
        ~PreparedMeshPipelines() noexcept = default;
        PreparedMeshPipelines(const PreparedMeshPipelines&) = delete;
        PreparedMeshPipelines& operator=(const PreparedMeshPipelines&) = delete;

        [[nodiscard]] RenderResult<bool> matches(const MaterialMeshPipelines&, const ResourceRegistry&) const noexcept;
        [[nodiscard]] std::span<const PublishedRasterProgram>
        find(const MeshVertexLayout&, bool front_counter_clockwise, const RasterStreamTarget&) const & noexcept;

    private:
        explicit PreparedMeshPipelines(DeviceEntry& device) noexcept : device_(device), table_(device) {}
        [[nodiscard]] RenderResult<std::vector<RasterProgramLink>>
        link(const MaterialMeshPipelines&, const MeshMaterialShader&, const ResourceRegistry&) noexcept;
        template <class Program> [[nodiscard]] std::size_t
        findUse(std::span<const Program>, const MeshVertexLayout&, bool front_ccw, const RasterStreamTarget&) const noexcept;

        struct Use final
        {
            bool front_counter_clockwise;
            RasterStreamTarget target;
        };
        DeviceEntry& device_;
        std::vector<Use> uses_;
        PreparedRasterTable table_;
    };
} // namespace lux::render::detail
