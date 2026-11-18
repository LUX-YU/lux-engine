#include "TonemapLinear.hpp"
#include "TonemapSrgb.hpp"
#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/features/BuiltinFeatures.hpp>
#include <lux/engine/render/features/common/FullscreenRaster.hpp>
#include <lux/engine/render/features/common/ParameterLayout.hpp>
#include <lux/engine/render/features/postprocess/TonemapFeature.hpp>

namespace lux::render
{
    RenderResult<void>
    detail::TonemapFeature::Ports::describe(const Config* config, std::string_view, FeaturePorts& ports) noexcept
    {
        if (auto valid = validateEffectOutput(config->output); !valid)
        {
            return valid;
        }
        const bool valid_format = config->output.format == rdesc::ETextureFormat::RGBA8_UNORM ||
                                  config->output.format == rdesc::ETextureFormat::RGBA8_SRGB ||
                                  config->output.format == rdesc::ETextureFormat::BGRA8_UNORM ||
                                  config->output.format == rdesc::ETextureFormat::BGRA8_SRGB;
        const bool invalid_encoding =
            config->encoding > EColorEncoding::SRGB ||
            (rdesc::isSrgbTextureFormat(config->output.format) && config->encoding != EColorEncoding::SRGB);
        if (!valid_format || invalid_encoding || config->output.samples != 1)
        {
            return cxx::unexpected(RenderError{kFeatureContract});
        }
        GraphImageDesc hdr{0, 0, 1, 1, rdesc::ETextureFormat::RGBA16_SFLOAT};
        hdr.extent.source = EImageExtentSource::TARGET;
        FeaturePort input{"hdr_color", EFeaturePort::INPUT, hdr};
        input.semantic = "lux.color.linear-hdr.v1";
        ports.resources.push_back(std::move(input));
        auto color = hdr;
        color.format = config->output.format;
        ports.resources.push_back({"color", EFeaturePort::OUTPUT, color});
        return {};
    }

    RenderResult<void>
    detail::TonemapFeature::build(const Config* config, std::string_view key, FeatureBuildContext& context) noexcept
    {
        const bool encode =
            config->encoding == EColorEncoding::SRGB && !rdesc::isSrgbTextureFormat(config->output.format);
        auto work = encode ? detail::fullscreenRaster(context, GraphProgram{kTonemapSrgb})
                           : detail::fullscreenRaster(context, GraphProgram{kTonemapLinear});
        if (!work)
        {
            return cxx::unexpected(work.error());
        }
        auto& input = work->fixed().interface;
        input.parameters.source = EParameterSource::EFFECT;
        input.parameters.instance_layout = detail::effectParameterLayout();
        input.parameters.fields = {{"exposure", {}, "exposure"}};
        input.resources = {{"hdr_color", {context.resource("hdr_color")}}};
        work->colors.push_back({{context.resource("color")}});
        auto pass =
            context.graph.addRasterPass(featureKey(detail::TonemapFeature::name, key, "tonemap"), std::move(*work));
        if (!pass)
        {
            return cxx::unexpected(pass.error());
        }
        return context.publish("color", pass->written.front());
    }

} // namespace lux::render
