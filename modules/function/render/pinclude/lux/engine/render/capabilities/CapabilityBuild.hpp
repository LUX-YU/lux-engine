#pragma once

#include <lux/engine/render/commands/Lighting3dCommands.hpp>

#include <lux/engine/render/commands/Mesh3dCommands.hpp>

#include <lux/engine/render/capabilities/BuiltinImplementations.hpp>
#include <lux/engine/render/extensions/features/CapabilityLowering.hpp>
#include <lux/engine/render/features/CameraParameters.hpp>
#include <lux/engine/render/inputs/DirectLightInputs.hpp>
#include <lux/engine/render/inputs/IndexedTriangleMeshInputs.hpp>
#include <lux/engine/render/inputs/RasterInstanceInputs.hpp>
#include <lux/engine/render/inputs/RasterMaterialInputs.hpp>
#include <lux/engine/render/inputs/SampledTextureInputs.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>

#include <algorithm>

namespace lux::render::detail
{
    template <class Value, class Fields>
    RenderResult<SerializedRenderConfiguration>
    encodeConfiguration(std::string_view schema, const Value& value, Fields fields) noexcept
    {
        SerializedRenderConfiguration result{std::string(schema)};
        serialization::BinaryWriter writer(result.bytes);
        if (!serialization::write(writer, fields(value), serialization::kPortableMetadataBudget))
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        return result;
    }

    template <class Value, class Fields>
    RenderResult<Value> decodeConfiguration(std::span<const std::byte> bytes, Fields fields) noexcept
    {
        Value result;
        auto members = fields(result);
        serialization::BinaryReader reader(bytes);
        if (!serialization::read(reader, members, serialization::kPortableMetadataBudget) || reader.remaining() != 0)
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        return result;
    }

    template <class Value, auto Decode>
    RenderResult<ConfigValue> decodeConfigurationValue(std::span<const std::byte> bytes) noexcept
    {
        auto result = Decode(bytes);
        if (!result)
        {
            return cxx::unexpected(result.error());
        }
        return ConfigValue::make(std::move(*result));
    }

    inline CapabilityImplementation describeImplementation(
        std::string_view implementation,
        std::string_view configuration_schema,
        std::vector<CapabilityPort> ports
    ) noexcept
    {
        CapabilityImplementation result;
        result.implementation = implementation;
        result.configuration = {std::string(configuration_schema), 1};
        CapabilityGraphDescription graph;
        graph.ports = std::move(ports);
        graph.target_formats = {
            rdesc::ETextureFormat::RGBA8_UNORM,
            rdesc::ETextureFormat::RGBA8_SRGB,
            rdesc::ETextureFormat::BGRA8_UNORM,
            rdesc::ETextureFormat::BGRA8_SRGB
        };
        graph.target_samples = {1};
        graph.target_kinds = {ETargetKind::OFFSCREEN, ETargetKind::SURFACE};
        result.graph = std::move(graph);
        return result;
    }

    inline std::uint32_t portIndex(const CapabilityBuildInput& input, std::string_view key) noexcept
    {
        return static_cast<std::uint32_t>(
            std::ranges::find(input.ports, key, &CapabilityPort::key) - input.ports.begin()
        );
    }

    inline void mapPort(
        CapabilityGraphContribution& result,
        const CapabilityBuildInput& input,
        std::string_view key,
        std::vector<FeaturePortRef> references
    ) noexcept
    {
        result.ports[portIndex(input, key)] = std::move(references);
    }

    // Professional resource requirements shared by the corresponding raster algorithms.
    // These are static dependency declarations, not a second registry or owner.
    inline constexpr std::array rasterStorageDependencies{
        CapabilityStorageDependency{EStorageScope::GLOBAL, kMeshStorageType, "assets"},
        CapabilityStorageDependency{EStorageScope::GLOBAL, kTextureStorageType, "assets"},
        CapabilityStorageDependency{EStorageScope::GLOBAL, kMaterialStorageType, "assets"},
        CapabilityStorageDependency{EStorageScope::SCENE, kMeshInstanceStorageType, "instances"}
    };

    inline constexpr std::array lightStorageDependencies{
        CapabilityStorageDependency{EStorageScope::SCENE, kDirectLightingStorageType, "direct_lights"}
    };

    [[nodiscard]] RenderResult<EffectDefinition> buildShadowFeatures(const CapabilityBuildInput&) noexcept;

} // namespace lux::render::detail
