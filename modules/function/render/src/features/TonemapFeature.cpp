#include "TonemapLinear.hpp"
#include "TonemapSrgb.hpp"
#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/FullscreenRaster.hpp>
#include <lux/engine/render/features/ParameterLayout.hpp>
#include <lux/engine/render/features/TonemapFeature.hpp>

namespace lux::render
{
    namespace
    {
        RenderResult<void> describeTonemap(const void* value, std::string_view, FeaturePorts& ports) noexcept
        {
            const auto& config = *static_cast<const TonemapFeatureConfig*>(value);
            if (auto valid = validateEffectOutput(config.output); !valid)
            {
                return valid;
            }
            const bool valid_format = config.output.format == rdesc::ETextureFormat::RGBA8_UNORM ||
                                      config.output.format == rdesc::ETextureFormat::RGBA8_SRGB ||
                                      config.output.format == rdesc::ETextureFormat::BGRA8_UNORM ||
                                      config.output.format == rdesc::ETextureFormat::BGRA8_SRGB;
            const bool invalid_encoding =
                config.encoding > EColorEncoding::SRGB ||
                (rdesc::isSrgbTextureFormat(config.output.format) && config.encoding != EColorEncoding::SRGB);
            if (!valid_format || invalid_encoding || config.output.samples != 1)
            {
                return cxx::unexpected(RenderError{kFeatureContract});
            }
            GraphImageDesc hdr{0, 0, 1, 1, rdesc::ETextureFormat::RGBA16_SFLOAT};
            hdr.extent.source = EImageExtentSource::TARGET;
            FeaturePort input{"hdr_color", EFeaturePort::INPUT, hdr};
            input.semantic = "lux.color.linear-hdr.v1";
            ports.resources.push_back(std::move(input));
            auto color = hdr;
            color.format = config.output.format;
            ports.resources.push_back({"color", EFeaturePort::OUTPUT, color});
            return {};
        }

        RenderResult<void>
        buildTonemap(
            const void* value,
            std::string_view key,
            FeatureBuildContext& context,
            const lux::render::detail::EnabledDeviceFeatures&
        ) noexcept
        {
            const auto& config = *static_cast<const TonemapFeatureConfig*>(value);
            const bool encode =
                config.encoding == EColorEncoding::SRGB && !rdesc::isSrgbTextureFormat(config.output.format);
            auto work = encode ? detail::fullscreenRaster(context, kTonemapSrgb, kTonemapSrgbReflection)
                               : detail::fullscreenRaster(context, kTonemapLinear, kTonemapLinearReflection);
            if (!work)
            {
                return cxx::unexpected(work.error());
            }
            work->fixed().interface.parameters.instance_layout = config.input_layout;
            work->fixed().interface.parameters.fields = {{"exposure", {}, "exposure"}};
            work->fixed().interface.resources = {{"hdr_color", {context.resource("hdr_color")}}};
            work->colors.push_back({{context.resource("color")}});
            auto pass =
                context.graph.addRasterPass(featureKey(detail::TonemapFeature::name, key, "tonemap"), std::move(*work));
            if (!pass)
            {
                return cxx::unexpected(pass.error());
            }
            return context.publish("color", pass->written.front());
        }
    } // namespace

    RenderResult<std::size_t>
    ConfigValueTraits<TonemapFeatureConfig>::retainedBytes(const TonemapFeatureConfig& value) noexcept
    {
        return sizeof(value) + detail::parameterLayoutBytes(value.input_layout);
    }

    const FeatureGraphOps& detail::TonemapFeature::graph(bool) noexcept
    {
        static const FeatureGraphOps graph{describeTonemap, buildTonemap};
        return graph;
    }
} // namespace lux::render
