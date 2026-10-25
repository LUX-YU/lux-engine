#include <lux/cxx/core/CheckedArithmetic.hpp>
#include <lux/engine/render/capabilities/RasterAssets.hpp>
#include <lux/engine/render/extensions/features/EffectDefinition.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>

namespace lux::render::detail
{
    namespace
    {
        RenderResult<void> expandUi(
            const TargetDesc& target,
            const void* settings,
            const void* data,
            std::span<const FeatureId> dependencies,
            EffectDefinition& result
        ) noexcept
        {
            const auto& configuration = *static_cast<const UiConfiguration*>(settings);
            const auto& input = *static_cast<const UiSceneInputs*>(data);
            UiRasterFeatureConfig ui{target};
            ui.index_type = configuration.index_type;
            ui.clear = configuration.clear;
            for (std::uint32_t i = 0; i < input.images.size(); ++i)
            {
                std::visit(
                    [&](const auto& image)
                    {
                        using Image = std::decay_t<decltype(image)>;
                        const auto& shape = [&]() -> const ImageShape&
                        {
                            if constexpr (std::same_as<Image, UiSampledImage>)
                            {
                                return image.description;
                            }
                            else
                            {
                                return image;
                            }
                        }();
                        GraphImageDesc description{shape.width, shape.height, shape.layers, shape.mips, shape.format};
                        if constexpr (std::same_as<Image, UiSampledImage>)
                        {
                            ui.images.push_back(UiTextureImage{image.texture, description, "ui"});
                        }
                        else
                        {
                            description.extent.source = EImageExtentSource::IMPORTED;
                            ui.images.push_back(UiTargetImage{description});
                            result.inputs.push_back(
                                {{0, uiImagePort(i)},
                                 {uiImageInput(i),
                                  description,
                                  EGraphOrigin::IMPORTED,
                                  {GraphImageRange{}},
                                  EGraphUse::SAMPLED}}
                            );
                        }
                    },
                    input.images[i]
                );
            }
            auto value = ConfigValue::make(std::move(ui));
            if (!value)
            {
                return cxx::unexpected(value.error());
            }
            result.features.push_back({dependencies[1], "ui", std::move(*value)});
            result.outputs.push_back({{0, "color"}, GraphOutputRequirements{target.format, 1}});
            return {};
        }
    } // namespace

    const FeatureGraphOps& UiScene::graph(bool) noexcept
    {
        static constexpr std::array dependencies{UiRasterFeature::name};
        static const std::array inputs{
            CapabilityStorageInput{kSampledTextureInput, 1, ECapabilityInputScope::GLOBAL, kTextureStorageType, "ui"}
        };
        static const FeatureGraphOps graph{
            .expand = expandUi,
            .dependencies = dependencies,
            .effect_input_type = cxx::typeToken<UiSceneInputs>(),
            .storage_inputs = inputs
        };
        return graph;
    }

    const CapabilitySupport& UiScene::capabilities() noexcept
    {
        static const CapabilitySupport support = []
        {
            CapabilitySupport result;
            result.implementation = name.name();
            result.description = "Indexed UI geometry with sampled images and imported Target outputs.";
            result.configuration.schema = "lux.render.config.ui";
            result.configuration.use = ERenderPayloadUse::CONFIGURATION;
            result.configuration.lifetime = ERenderPayloadLifetime::MOVED_VALUE;
            result.configuration.changes_graph = true;
            result.configuration.setCppType(cxx::typeToken<UiConfiguration>());
            RenderCapability capability;
            capability.name = kUiSceneCapability;
            capability.description = result.description;
            capability.inputs.push_back(RasterAssets::capabilities().capabilities.front().inputs[1]);
            RenderPayloadContract images;
            images.schema = "lux.render.input.ui_images";
            images.use = ERenderPayloadUse::EFFECT_INPUT;
            images.lifetime = ERenderPayloadLifetime::MOVED_VALUE;
            images.changes_graph = true;
            images.setCppType(cxx::typeToken<UiSceneInputs>());
            capability.inputs.push_back(std::move(images));
            RenderPayloadContract geometry;
            geometry.schema = "lux.render.input.ui_geometry";
            geometry.use = ERenderPayloadUse::FRAME_INPUT;
            geometry.setCppType(cxx::typeToken<DrawListData>());
            capability.inputs.push_back(std::move(geometry));
            capability.outputs = {"color"};
            capability.target_formats = {
                rdesc::ETextureFormat::RGBA8_UNORM,
                rdesc::ETextureFormat::RGBA8_SRGB,
                rdesc::ETextureFormat::BGRA8_UNORM,
                rdesc::ETextureFormat::BGRA8_SRGB
            };
            capability.target_samples = {1};
            capability.target_kinds = {ETargetKind::OFFSCREEN, ETargetKind::SURFACE};
            result.capabilities.push_back(std::move(capability));
            return result;
        }();
        return support;
    }
} // namespace lux::render::detail

namespace lux::render
{
    RenderResult<std::size_t> ConfigValueTraits<UiSceneInputs>::retainedBytes(const UiSceneInputs& value) noexcept
    {
        const auto images = cxx::checkedMul(value.images.capacity(), sizeof(VUiSceneImage));
        if (!images)
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        const auto total = cxx::checkedAdd(sizeof(value), *images);
        return total ? RenderResult<std::size_t>{*total} : cxx::unexpected(RenderError{kInvalidConfig});
    }
} // namespace lux::render
