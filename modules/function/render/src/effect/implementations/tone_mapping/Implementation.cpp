#include "TonemapHdrLinear.hpp"
#include "TonemapLinear.hpp"
#include "TonemapSrgb.hpp"
#include <cmath>
#include <lux/engine/render/effect/implementations/common/EffectArguments.hpp>
#include <lux/engine/render/effect/capabilities/DeferredLighting.hpp>
#include <lux/engine/render/assembly/BuiltinEffects.hpp>
#include <lux/engine/render/effect/capabilities/ToneMapping.hpp>
#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/render/effect/EffectImplementation.hpp>
#include <lux/engine/render/effect/implementations/common/FullscreenRaster.hpp>
#include <lux/engine/render/runtime/commands/CommandBinding.hpp>
#include <lux/engine/render/runtime/commands/EffectCommands.hpp>
#include <lux/engine/render/frame/FrameError.hpp>
#include <array>
#include <lux/engine/render/graph/ConstantBlock.hpp>

namespace lux::render::detail
{
    struct TonemapConstants final
    {
        float exposure;
    };

    struct TonemapExposure final
    {
        TonemapConstants constants;

        [[nodiscard]] RenderResult<void> setExposure(float exposure) noexcept
        {
            if (!std::isfinite(exposure)) return cxx::unexpected(RenderError{kFrameContract});
            constants.exposure = exposure;
            return {};
        }
    };
} // namespace lux::render::detail

namespace lux::render
{
    template <> struct ConstantBlockTraits<detail::TonemapConstants> final
    {
        static ConstantBlockLayout layout(rdesc::EShaderTarget target) noexcept
        {
            static constexpr std::array members{
                ConstantMemberLayout{"exposure", offsetof(detail::TonemapConstants, exposure), rdesc::EDataScalar::FLOAT}
            };
            static_assert(sizeof(detail::TonemapConstants) == 4);
            // DXIL's constant buffer alignment is distinct from the packed root DWORD payload.
            const unsigned alignment = target == rdesc::EShaderTarget::DXIL ? 16u : 4u;
            return {sizeof(detail::TonemapConstants), alignment, members};
        }
    };
} // namespace lux::render

namespace lux::render
{
    namespace
    {
        enum class EColorOutput : std::uint8_t { LINEAR, SRGB, HDR_LINEAR };

        class ToneMapping final : public EffectImplementation
        {
        public:
            ToneMapping(EColorOutput output, float exposure) noexcept : output_(output), exposure_{{exposure}}
            {
                bindState(exposure_);
            }

            RenderResult<void> onBuild(EffectCompileContext&) const noexcept override;

        private:
            EColorOutput output_;
            detail::TonemapExposure exposure_;
        };

        template <EColorOutput Output>
        EffectCreateResult createToneMapping(graph::NodeId node, std::string_view argument) noexcept
        {
            constexpr auto exposure = cxx::args::Option<float, "exposure">{}.def(1.0f).check(
                [](float value) noexcept { return std::isfinite(value); }
            );
            const auto parsed = cxx::args::parse(argument, exposure);
            if (!parsed) return cxx::unexpected(detail::effectArgumentError(node, parsed.error()));
            return std::make_unique<ToneMapping>(Output, *parsed.get<"exposure">());
        }

        RenderResult<void> ToneMapping::onBuild(EffectCompileContext& context) const noexcept
        {
            const auto input = context.inputs(ToneMappingPorts::hdr_color.semantic);
            const bool valid_input = input.size() == 1 && input.front().size() == 1;
            const auto& target = context.target();
            const auto format = target.format;
            const bool is_srgb_format = rdesc::isSrgbTextureFormat(format);
            const bool is_unorm_format =
                format == rdesc::ETextureFormat::RGBA8_UNORM || format == rdesc::ETextureFormat::BGRA8_UNORM;
            const bool hdr = output_ == EColorOutput::HDR_LINEAR;
            const bool linear_hdr_target = format == rdesc::ETextureFormat::RGBA16_SFLOAT;
            const bool valid_format = hdr ? linear_hdr_target : is_unorm_format || (output_ == EColorOutput::SRGB &&
                (format == rdesc::ETextureFormat::RGBA8_SRGB || format == rdesc::ETextureFormat::BGRA8_SRGB));
            if (!valid_input || target.samples != 1 || !valid_format)
                return cxx::unexpected(RenderError{kCapabilityDeclaration, {context.nodeId().value}});

            const std::string key = "tone-mapping/" + std::to_string(context.nodeId().value);
            GraphImageDesc image{0, 0, 1, 1, format};
            image.extent.source = EImageExtentSource::TARGET;
            const auto color = context.declareResource({key, image});
            if (!color) return cxx::unexpected(color.error());
            const bool encode_srgb = output_ == EColorOutput::SRGB && !is_srgb_format;
            const auto& fragment = hdr ? kTonemapHdrLinear : encode_srgb ? kTonemapSrgb : kTonemapLinear;
            auto work = detail::fullscreenRaster(context, GraphProgram{fragment});
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
            return context.publish(ToneMappingPorts::color.semantic, {{ToneMappingPorts::color.semantic, pass->written.front()}});
        }
    }

    std::array<ImplementationEntry, 3> detail::toneMappingImplementations() noexcept
    {
        ImplementationEntry implementation;
        implementation.capability = capabilities::ToneMapping.id;
        implementation.identity = {"lux.render.tone_mapping.linear", capabilities::ToneMapping.version};
        implementation.supported_outputs = {ToneMappingPorts::color.semantic};
        implementation.create = createToneMapping<EColorOutput::LINEAR>;
        auto srgb = implementation;
        srgb.identity.canonical_name = "lux.render.tone_mapping.srgb";
        srgb.create = createToneMapping<EColorOutput::SRGB>;
        auto hdr = implementation;
        hdr.identity.canonical_name = "lux.render.tone_mapping.hdr_linear";
        hdr.create = createToneMapping<EColorOutput::HDR_LINEAR>;
        return {std::move(implementation), std::move(srgb), std::move(hdr)};
    }
} // namespace lux::render

namespace lux::render::detail
{
    namespace
    {
        void update(TonemapExposure& state, const SetToneMappingExposure::Request& request, AckResponse& response) noexcept
        {
            const auto result = state.setExposure(request.exposure);
            response.error = result ? RenderError{} : result.error();
        }
    }

    CommandBinding toneMappingExposureBinding() noexcept
    {
        return CommandBinding::declareForEffect<SetToneMappingExposure, TonemapExposure, &update>();
    }
}
