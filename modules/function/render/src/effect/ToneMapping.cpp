#include "TonemapLinear.hpp"
#include "TonemapSrgb.hpp"
#include <charconv>
#include <lux/engine/render/effect/EffectBundles.hpp>
#include <lux/engine/render/effect/BuiltinEffectNodes.hpp>
#include <lux/engine/render/effect/ToneMapping.hpp>
#include <lux/engine/render/extensions/CommandBinding.hpp>
#include <lux/engine/render/extensions/effect/EffectBuildContext.hpp>
#include <lux/engine/render/extensions/effect/EffectImplementation.hpp>
#include <lux/engine/render/extensions/effect/EffectInitializationContext.hpp>
#include <lux/engine/render/features/common/FullscreenRaster.hpp>
#include <lux/engine/render/features/postprocess/TonemapExposure.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>

namespace lux::render
{
    namespace
    {
        class ToneMappingPreparation final : public InstanceData
        {
        public:
            using Configuration = GraphPassId;

            struct ResolvedInputs final
            {
                NodeStateSlot<detail::TonemapExposure> state;
                InlineConstantWriter<detail::TonemapConstants> writer;
            };

            static RenderResult<ResolvedInputs> resolve(const Configuration& pass, InstanceLinkContext& link) noexcept
            {
                auto state = link.resolveNodeState<detail::TonemapExposure>();
                if (!state) return cxx::unexpected(state.error());
                auto writer = link.resolveInlineConstants<detail::TonemapConstants>(pass);
                if (!writer) return cxx::unexpected(writer.error());
                return ResolvedInputs{*state, *writer};
            }

            static InstanceDataResult instantiate(const ResolvedInputs& inputs, InstanceInstantiationContext&) noexcept
            {
                return std::make_unique<ToneMappingPreparation>(inputs);
            }

            explicit ToneMappingPreparation(ResolvedInputs inputs) noexcept : inputs_(inputs) {}

            RenderResult<void> prepare(FramePreparation& frame) noexcept override
            {
                const auto state = frame.readNodeState(inputs_.state);
                if (!state) return cxx::unexpected(state.error());
                return frame.write(inputs_.writer, state->constants);
            }

        private:
            ResolvedInputs inputs_;
        };

        RenderResult<void>
        initializeToneMapping(EffectInitializationContext& context, std::string_view argument) noexcept
        {
            float exposure = 1;
            if (!argument.empty())
            {
                const auto end = argument.data() + argument.size();
                const auto parsed = std::from_chars(argument.data(), end, exposure);
                if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(exposure))
                    return cxx::unexpected(RenderError{kInvalidConfig, {context.nodeId().value}});
            }
            return context.initializeState<detail::TonemapExposure>(detail::TonemapExposure{{exposure}});
        }

        template <bool Srgb>
        RenderResult<void> buildToneMapping(EffectBuildContext& context, std::string_view) noexcept
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

            auto& builder = context.builder();
            const std::string key = "tone-mapping/" + std::to_string(context.nodeId().value);
            GraphImageDesc image{0, 0, 1, 1, format};
            image.extent.source = EImageExtentSource::TARGET;
            const auto color = builder.declareResource({key, image});
            if (!color) return cxx::unexpected(color.error());
            const bool encode = Srgb && !is_srgb_format;
            auto shaders = encode ? GraphProgram{kTonemapSrgb} : GraphProgram{kTonemapLinear};
            auto work = detail::fullscreenRaster(builder, std::move(shaders));
            if (!work) return cxx::unexpected(work.error());
            auto& shader = work->fixed().interface;
            shader.parameters.setInlineConstant(detail::TonemapConstants{0});
            shader.resources = {{"hdr_color", {input.front().front().version}}};
            work->colors.push_back({{{*color, 0}}});
            auto pass = builder.addRasterPass(key, std::move(*work));
            if (!pass) return cxx::unexpected(pass.error());
            auto configuration = ConfigValue::make(pass->pass);
            if (!configuration) return cxx::unexpected(configuration.error());
            if (auto added = context.addPreparation({
                    InstanceDataFactory::make<ToneMappingPreparation>(), std::move(*configuration), {}, {}, {pass->pass}
                }); !added)
                return added;
            return context.publish(ToneMapping::color, {{ToneMapping::color, pass->written.front()}});
        }
    }

    CapabilityDefinition ToneMapping::definition() noexcept
    {
        CapabilityDefinition definition;
        definition.identity = {graph::nodeTypeId(capability), std::string{capability}, version};
        definition.display_name = "Tone Mapping";
        definition.ports = {
            {hdr_color, "hdr_color", {std::string{HdrColorBundle::type}, HdrColorBundle::version},
                graph::EPinDirection::INPUT, 1, true},
            {color, "color", {std::string{display_type}, 1}, graph::EPinDirection::OUTPUT}
        };
        return definition;
    }

    RenderResult<void> detail::registerToneMapping(RenderServer& server) noexcept
    {
        const auto command = server.registerNodeEffectCommand(
            CommandBinding::declareForEffect<SetToneMappingExposure, TonemapExposure, &TonemapExposure::setNode>()
        );
        if (!command) return cxx::unexpected(command.error());
        auto definition = ToneMapping::definition();
        const auto type = definition.identity.id;
        definition.commands = {command->command};
        if (auto added = server.registerEffectCapability(std::move(definition)); !added) return added;
        EffectImplementation implementation;
        implementation.capability = type;
        implementation.identity = {std::string{ToneMapping::linear}, ToneMapping::version};
        implementation.supported_outputs = {ToneMapping::color};
        implementation.command_bindings = {*command};
        implementation.initialize = initializeToneMapping;
        implementation.build = buildToneMapping<false>;
        auto srgb = implementation;
        srgb.identity.canonical_name = ToneMapping::srgb;
        srgb.build = buildToneMapping<true>;
        if (auto added = server.registerEffectImplementation(std::move(implementation)); !added) return added;
        return server.registerEffectImplementation(std::move(srgb));
    }
} // namespace lux::render
