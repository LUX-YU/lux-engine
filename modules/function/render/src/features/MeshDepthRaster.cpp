#include <lux/engine/render/resources/mesh/Binding.hpp>
#include <lux/engine/render/resources/material/Binding.hpp>
#include <lux/engine/render/resources/instance/Binding.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/RasterMesh.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>

namespace lux::render
{

    RenderResult<void>
    detail::MeshDepthRaster::Ports::describe(const Config* config, std::string_view key, FeaturePorts& ports) noexcept
    {
        return detail::describe<true>(config, key, ports);
    }

    template <bool Paged>
    RenderResult<void>
    detail::MeshDepthRaster::build(const Config* config, std::string_view key, FeatureBuildContext& context) noexcept
    {
        return detail::build<true, Paged>(config, key, context);
    }

    template RenderResult<void>
    detail::MeshDepthRaster::build<false>(const Config*, std::string_view, FeatureBuildContext&) noexcept;

    template RenderResult<void>
    detail::MeshDepthRaster::build<true>(const Config*, std::string_view, FeatureBuildContext&) noexcept;

} // namespace lux::render
