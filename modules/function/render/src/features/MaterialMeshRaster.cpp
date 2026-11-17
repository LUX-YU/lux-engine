#include <lux/engine/render/resources/mesh/Binding.hpp>
#include <lux/engine/render/resources/material/Binding.hpp>
#include <lux/engine/render/resources/instance/Binding.hpp>
#include <lux/engine/render/resources/texture/Binding.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/RasterMesh.hpp>
#include <lux/engine/render/features/RasterMeshParameters.hpp>

namespace lux::render
{

    RenderResult<void>
    detail::MaterialMeshRaster::Ports::describe(
        const Config* config,
        std::string_view key,
        FeaturePorts& ports
    ) noexcept
    {
        return detail::describe<false>(config, key, ports);
    }

    template <bool Paged>
    RenderResult<void>
    detail::MaterialMeshRaster::build(const Config* config, std::string_view key, FeatureBuildContext& context) noexcept
    {
        return detail::build<false, Paged>(config, key, context);
    }

    template RenderResult<void>
    detail::MaterialMeshRaster::build<false>(const Config*, std::string_view, FeatureBuildContext&) noexcept;

    template RenderResult<void>
    detail::MaterialMeshRaster::build<true>(const Config*, std::string_view, FeatureBuildContext&) noexcept;

} // namespace lux::render
