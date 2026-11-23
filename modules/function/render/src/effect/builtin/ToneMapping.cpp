#include "TonemapLinear.hpp"
#include "TonemapSrgb.hpp"
#include <cmath>
#include <lux/engine/render/effect/EffectArguments.hpp>
#include <lux/engine/render/effect/builtin/Lighting.hpp>
#include <lux/engine/render/effect/builtin/Implementations.hpp>
#include <lux/engine/render/effect/builtin/ToneMapping.hpp>
#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/render/effect/EffectImplementation.hpp>
#include <lux/engine/render/effect/EffectInitializationContext.hpp>
#include <lux/engine/render/features/common/FullscreenRaster.hpp>
#include <lux/engine/render/features/postprocess/TonemapExposure.hpp>

namespace lux::render
{
    namespace
    {
        RenderResult<void>
        initializeToneMapping(EffectInitializationContext& context, std::string_view argument) noexcept
        {
            constexpr auto exposure = cxx::args::Option<float, "exposure">{}.def(1.0f).check(
                [](float value) noexcept { return std::isfinite(value); }
            );
            const auto parsed = cxx::args::parse(argument, exposure);
            if (!parsed) return cxx::unexpected(detail::effectArgumentError(context.nodeId(), parsed.error()));
            return context.initializeState<detail::TonemapExposure>(detail::TonemapExposure{{*parsed.get<"exposure">()}});
        }

        template <bool Srgb>
        RenderResult<void> buildToneMapping(EffectCompileContext& context, std::string_view) noexcept
        {
            const auto input = context.inputs(ToneMapping::hdr_color);
            const bool valid_input = input.size() == 1 && input.front().size() == 1;
            const auto& target = context.target();
            const auto format = target.format;
            const bool is_srgb_format = rdesc::isSrgbTextureFormat(format);
            const bool is_unorm_format =
                format == rdesc::ETextureFormat::RGBA8_UNORM || format == rdesc::ETextureFormat::BGRA8_UNORM;
            const bool valid_srgb = Srgb &&
                (format == rdesc::ETextureFormat::RGBA8_SRGB || format == rdesc::ETextureFormat::BGRA8_SRGB);
            if (!valid_input || target.samples != 1 || (!is_unorm_format && !valid_srgb))
                return cxx::unexpected(RenderError{kCapabilityDeclaration, {context.nodeId().value}});

            const std::string key = "tone-mapping/" + std::to_string(context.nodeId().value);
            GraphImageDesc image{0, 0, 1, 1, format};
            image.extent.source = EImageExtentSource::TARGET;
            const auto color = context.declareResource({key, image});
            if (!color) return cxx::unexpected(color.error());
            const bool encode = Srgb && !is_srgb_format;
            auto shaders = encode ? GraphProgram{kTonemapSrgb} : GraphProgram{kTonemapLinear};
            auto work = detail::fullscreenRaster(context, std::move(shaders));
            if (!work) return cxx::unexpected(work.error());
            auto& shader = work->fixed().interface;
            shader.parameters.setInlineConstant(detail::TonemapConstants{0});
            shader.resources = {{"hdr_color", {input.front().front().version}}};
            work->colors.push_back({{{*color, 0}}});
            auto pass = context.addRasterPass(key, std::move(*work));
            if (!pass) return cxx::unexpected(pass.error());
            auto copy = context.copyInlineBlock<detail::TonemapConstants, detail::TonemapExposure,
                                                &detail::TonemapExposure::constants>(pass->pass);
            if (!copy) return copy;
            return context.publish(ToneMapping::color, {{ToneMapping::color, pass->written.front()}});
        }
    }

    EffectNodeDefinition ToneMapping::definition() noexcept
    {
        EffectNodeDefinition definition;
        definition.identity = {graph::nodeTypeId(capability), std::string{capability}, version};
        definition.display_name = "Tone Mapping";
        definition.ports = {
            {hdr_color, "hdr_color", {std::string{HdrColorBundle::type}, HdrColorBundle::version},
                graph::EPinDirection::INPUT, 1, true},
            {color, "color", {std::string{display_type}, 1}, graph::EPinDirection::OUTPUT}
        };
        return definition;
    }

    std::array<EffectImplementation, 2> detail::toneMappingImplementations() noexcept
    {
        EffectImplementation implementation;
        implementation.capability = graph::nodeTypeId(ToneMapping::capability);
        implementation.identity = {std::string{ToneMapping::linear}, ToneMapping::version};
        implementation.supported_outputs = {ToneMapping::color};
        implementation.initialize = initializeToneMapping;
        implementation.build = buildToneMapping<false>;
        auto srgb = implementation;
        srgb.identity.canonical_name = ToneMapping::srgb;
        srgb.build = buildToneMapping<true>;
        return {std::move(implementation), std::move(srgb)};
    }
} // namespace lux::render
