#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/capabilities/BuiltinImplementations.hpp>
#include <lux/engine/render/capabilities/CapabilityBuild.hpp>
#include <lux/engine/render/capabilities/ImguiCapability.hpp>
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/extensions/features/EffectDefinition.hpp>
#include <lux/engine/render/inputs/SampledTextureInputs.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>

namespace lux::render::detail
{
    namespace
    {
        RenderResult<ConfigValue> decodeConfiguration(std::span<const std::byte> bytes) noexcept
        {
            UiConfiguration value;
            serialization::BinaryReader reader(bytes);
            auto fields = std::tie(value.index_type, value.clear);
            const auto decoded = serialization::read(reader, fields, serialization::kPortableMetadataBudget);
            const bool valid_index = value.index_type == EIndexType::UINT16 || value.index_type == EIndexType::UINT32;
            if (!decoded || reader.remaining() != 0 || !valid_index)
            {
                return cxx::unexpected(RenderError{kInvalidConfig});
            }
            return ConfigValue::make(value);
        }

        RenderResult<CapabilityGraphContribution> lowerUi(const CapabilityBuildInput& build) noexcept
        {
            const auto& target = build.output;
            const auto& configuration = *static_cast<const UiConfiguration*>(build.configuration.data());
            EffectDefinition result;
            UiRasterFeatureConfig ui{target};
            ui.index_type = configuration.index_type;
            ui.clear = configuration.clear;
            auto value = ConfigValue::make(std::move(ui));
            if (!value)
            {
                return cxx::unexpected(value.error());
            }
            result.features.push_back({build.features[0], "ui", std::move(*value)});
            result.outputs.push_back({{0, "color"}, GraphOutputRequirements{target.format, 1}});
            return CapabilityGraphContribution{std::move(result), {{{0, "color"}}}};
        }
    } // namespace

    CapabilityImplementation UiScene::implementation() noexcept
    {
        static const std::array dependencies{
            CapabilityResourceDependency{EStorageScope::GLOBAL, kTextureResourceName}
        };
        static const CapabilityImplementation support = []
        {
            CapabilityImplementation result;
            result.implementation = name.name();
            result.description = "Indexed UI geometry with sampled images and imported Target outputs.";
            result.configuration.schema = "lux.render.config.ui";
            CapabilityGraphDescription graph;
            graph.ports = {
                {"color",
                 ECapabilityPortDirection::OUTPUT,
                 "lux.color.display.v1",
                 CapabilityImageShape{std::nullopt, 1, 1},
                 "lux.coordinates.viewport"}
            };
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
        }();
        auto implementation = support;
        implementation.resource_dependencies = dependencies;
        implementation.decode_configuration = decodeConfiguration;
        implementation.build_graph = lowerUi;
        return implementation;
    }
} // namespace lux::render::detail
