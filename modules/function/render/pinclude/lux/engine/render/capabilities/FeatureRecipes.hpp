#pragma once

#include <lux/engine/render/capabilities/DeferredLighting.hpp>
#include <lux/engine/render/capabilities/RasterSurface.hpp>
#include <lux/engine/render/extensions/features/EffectDefinition.hpp>
#include <lux/engine/render/features/shadow/ShadowInputs.hpp>
#include <array>

namespace lux::render::detail
{
    // Author-side aliases of actual Feature ports. No duplicated shapes or semantic resolver.
    using SurfacePorts = std::array<FeaturePortRef, 4>;

    struct LightingPorts final
    {
        SurfacePorts surface;
        FeaturePortRef color;
    };

    [[nodiscard]] RenderResult<SurfacePorts>
    appendSurfaceFeatures(
        EffectDefinition&, std::span<const FeatureId>, const EffectOutputDesc&,
        const RasterSurfaceConfiguration&, std::string_view key
    ) noexcept;

    [[nodiscard]] RenderResult<LightingPorts>
    appendLightingFeatures(
        EffectDefinition&, std::span<const FeatureId>, const EffectOutputDesc&,
        const DeferredLightingConfiguration&, std::string_view key
    ) noexcept;


    struct ShadowPorts final
    {
        ShadowMapShape shape;
        std::uint32_t projection;
        FeaturePortRef depth;
    };

    [[nodiscard]] RenderResult<ShadowPorts>
    appendShadowFeatures(EffectDefinition&, std::span<const FeatureId>, const MeshShadowsConfiguration&) noexcept;

    // Append to the one author definition. Callers connect returned FeaturePortRefs explicitly.
    [[nodiscard]] inline RenderResult<std::uint32_t>
    appendFeatureRecipe(EffectDefinition& destination, EffectDefinition source, std::string_view key) noexcept
    {
        const bool is_invalid_key = key.empty();
        const bool is_too_large = source.features.size() > UINT32_MAX ||
            destination.features.size() > UINT32_MAX - source.features.size();
        if (is_invalid_key || is_too_large) return cxx::unexpected(RenderError{kInvalidConfig});
        const auto offset = static_cast<std::uint32_t>(destination.features.size());
        for (auto& feature : source.features)
        {
            feature.contribution_key = std::string(key) + "/" + feature.contribution_key;
            destination.features.push_back(std::move(feature));
        }
        for (auto& connection : source.connections)
        {
            connection.source.contribution += offset;
            connection.destination.contribution += offset;
            destination.connections.push_back(std::move(connection));
        }
        for (auto& input : source.inputs)
        {
            input.destination.contribution += offset;
            destination.inputs.push_back(std::move(input));
        }
        for (auto& output : source.outputs)
        {
            output.source.contribution += offset;
            destination.outputs.push_back(std::move(output));
        }
        return offset;
    }

    [[nodiscard]] inline bool
    validRecipeFeatures(std::span<const FeatureId> features, std::size_t count) noexcept
    {
        return features.size() == count && std::ranges::none_of(features, &FeatureId::isNull);
    }
} // namespace lux::render::detail
