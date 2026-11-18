#include <lux/editor/ui/render/UiFeature.hpp>
#include <lux/editor/ui/render/UiState.hpp>
#include <lux/engine/render/extensions/Registration.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <lux/engine/serialization/PortableValueCodec.hpp>
#include <cmath>

namespace lux::editor::ui
{
    bool UiConfiguration::valid() const noexcept
    {
        return index_type == EIndexType::UINT16 &&
            std::ranges::all_of(clear, [](float value) noexcept { return std::isfinite(value); });
    }

    RenderResult<SerializedRenderConfiguration> encodeUiConfiguration(const UiConfiguration& value) noexcept
    {
        if (!value.valid())
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        SerializedRenderConfiguration result{std::string(kUiConfigurationSchema)};
        serialization::BinaryWriter writer(result.bytes);
        if (!serialization::write(
                writer, std::tie(value.index_type, value.clear), serialization::kPortableMetadataBudget
            ))
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        return result;
    }

    RenderResult<UiConfiguration> decodeUiConfiguration(std::span<const std::byte> bytes) noexcept
    {
        UiConfiguration result;
        auto fields = std::tie(result.index_type, result.clear);
        serialization::BinaryReader reader(bytes);
        if (!serialization::read(reader, fields, serialization::kPortableMetadataBudget) || reader.remaining() ||
            !result.valid())
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        return result;
    }

    RenderResult<CapabilityImplementationId> registerUi(RenderServer& server) noexcept
    {
        auto resource = server.registerResource<UiState>();
        if (!resource)
        {
            return cxx::unexpected(resource.error());
        }
        auto feature = server.registerFeature<UiRasterFeature>();
        if (!feature)
        {
            return cxx::unexpected(feature.error());
        }
        auto capability = server.registerCapability<UiCapability>();
        if (!capability)
        {
            return cxx::unexpected(capability.error());
        }
        CapabilityImplementation implementation;
        implementation.implementation = "lux.editor.ui.imgui";
        implementation.description = "Owned ImGui drawing snapshots with ordinary indexed passes.";
        implementation.features = {*feature};
        using enum rdesc::ETextureFormat;
        using enum ETargetKind;
        implementation.outputs = {
            {RGBA8_UNORM, 1, OFFSCREEN}, {RGBA8_UNORM, 1, SURFACE},
            {RGBA8_SRGB, 1, OFFSCREEN}, {RGBA8_SRGB, 1, SURFACE},
            {BGRA8_UNORM, 1, OFFSCREEN}, {BGRA8_UNORM, 1, SURFACE},
            {BGRA8_SRGB, 1, OFFSCREEN}, {BGRA8_SRGB, 1, SURFACE}
        };
        return server.addCapabilityImplementation(*capability, implementation);
    }

    RenderResult<CreateEffectRequest> makeUiRenderEffect(
        EffectOutputDesc output, const CapabilityImplementationInfo& implementation, const UiConfiguration& config
    ) noexcept
    {
        const bool is_wrong_implementation =
            implementation.capability.type != cxx::make_basic_type_info<UiCapability>() ||
            implementation.implementation != "lux.editor.ui.imgui" || implementation.features.size() != 1;
        if (is_wrong_implementation)
        {
            return cxx::unexpected(RenderError{kCapabilityDeclaration});
        }
        if (!config.valid()) return cxx::unexpected(RenderError{kInvalidConfig});
        auto value = ConfigValue::make(UiRasterFeatureConfig{output, config});
        if (!value)
        {
            return cxx::unexpected(value.error());
        }
        CreateEffectRequest result{output};
        result.definition.features.push_back({implementation.features.front(), "ui/draw", std::move(*value)});
        result.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{output.format, output.samples}});
        result.implementations.push_back({implementation.implementation_id, "ui"});
        return result;
    }
} // namespace lux::editor::ui
