#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/capabilities/CapabilityBuild.hpp>
#include <lux/engine/render/capabilities/FeatureRecipes.hpp>
#include <lux/engine/render/capabilities/Mesh3dCapability.hpp>
#include <lux/engine/render/capabilities/RasterSurface.hpp>

namespace lux::render
{
    namespace
    {
        auto fields(auto& value) noexcept
        {
            return std::tie(value.visibility);
        }

    } // namespace

    RenderResult<SerializedRenderConfiguration>
    encodeRasterSurfaceConfiguration(const RasterSurfaceConfiguration& value) noexcept
    {
        return detail::encodeConfiguration(
            kRasterSurfaceConfigurationSchema,
            value,
            [](const auto& v) { return fields(v); }
        );
    }

    RenderResult<RasterSurfaceConfiguration> decodeRasterSurfaceConfiguration(std::span<const std::byte> bytes) noexcept
    {
        return detail::decodeConfiguration<RasterSurfaceConfiguration>(bytes, [](auto& v) { return fields(v); });
    }

    namespace detail
    {
        RenderResult<SurfacePorts>
        appendSurfaceFeatures(
            EffectDefinition& destination,
            std::span<const FeatureId> features,
            const EffectOutputDesc& output,
            const RasterSurfaceConfiguration& options,
            std::string_view key
        ) noexcept
        {
            if (!validRecipeFeatures(features, 2)) return cxx::unexpected(RenderError{kFeatureContract});
            GBufferFeatureConfig surface{output};
            MeshVisibilityFeatureConfig visibility;
            visibility.frustum_culling = options.visibility;
            visibility.camera_view = true;
            auto visible = ConfigValue::make(std::move(visibility));
            if (!visible) return cxx::unexpected(visible.error());
            auto value = ConfigValue::make(std::move(surface));
            if (!value) return cxx::unexpected(value.error());
            EffectDefinition recipe;
            recipe.features.push_back({features[1], "visibility", std::move(*visible)});
            recipe.features.push_back({features[0], "surface", std::move(*value)});
            for (const auto port : kMeshDrawInputPorts)
            {
                recipe.connections.push_back({{0, port}, {1, port}});
            }
            auto offset = appendFeatureRecipe(destination, std::move(recipe), key);
            if (!offset) return cxx::unexpected(offset.error());
            const auto raster = *offset + 1;
            return SurfacePorts{
                FeaturePortRef{raster, "albedo_metallic"}, FeaturePortRef{raster, "normal_roughness"},
                FeaturePortRef{raster, "emissive_ao"}, FeaturePortRef{raster, "depth"}
            };
        }

        CapabilityImplementation RasterSurface::implementation() noexcept
        {
            auto result = describeImplementation(name.name());
            return result;
        }
    } // namespace detail
} // namespace lux::render
