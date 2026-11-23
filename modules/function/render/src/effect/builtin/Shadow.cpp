#include <lux/engine/render/effect/builtin/MeshVisibilityBuild.hpp>
#include <lux/engine/render/effect/builtin/Implementations.hpp>
#include <lux/engine/render/effect/builtin/Shadow.hpp>
#include <lux/engine/render/effect/EffectArguments.hpp>
#include <lux/engine/render/effect/EffectInitializationContext.hpp>
#include <lux/engine/render/effect/builtin/MeshVisibility.hpp>
#include <cmath>

namespace lux::render
{
    namespace
    {
        RenderResult<void> initializeShadow(EffectInitializationContext& context, std::string_view arguments) noexcept
        {
            constexpr detail::ShadowNodeConfiguration defaults;
            constexpr auto resolution = cxx::args::Option<std::uint32_t, "resolution">{}
                .def(defaults.projection.shape.resolution);
            constexpr auto distance = cxx::args::Option<float, "distance">{}.def(defaults.projection.distance).check(
                [](float value) noexcept { return std::isfinite(value) && value > 0; }
            );
            constexpr auto radius = cxx::args::Option<std::uint32_t, "pcf-radius">{}
                .def(defaults.projection.pcf_radius).check(cxx::args::inRange(0u, 4u));
            constexpr auto kFrustumCulling = cxx::args::Option<bool, "frustum-culling">{}.def(true);
            const auto parsed = cxx::args::parse(arguments, resolution, distance, radius, kFrustumCulling);
            if (!parsed) return cxx::unexpected(detail::effectArgumentError(context.nodeId(), parsed.error()));
            auto configuration = defaults;
            auto& settings = configuration.projection;
            settings.shape.resolution = *parsed.get<"resolution">();
            settings.distance = *parsed.get<"distance">();
            settings.pcf_radius = *parsed.get<"pcf-radius">();
            configuration.frustum_culling = *parsed.get<"frustum-culling">();
            if (settings.shape.resolution <= 2 * (settings.pcf_radius + 2))
                return cxx::unexpected(RenderError{kInvalidConfig, {context.nodeId().value}});
            return context.setConfiguration(configuration);
        }

        RenderResult<GraphPassId>
        appendSurface(EffectCompileContext& context, const detail::MeshAttachmentInputs& inputs, RasterWork raster) noexcept
        {
            const auto& key = inputs.key;
            const auto& shape = context.configuration<detail::ShadowNodeConfiguration>()->projection.shape;
            EffectResourceBundle bundle;
            const auto depth = context.declareResource({key + "shadow-depth", shape.image()});
            if (!depth) return cxx::unexpected(depth.error());
            raster.depth = RasterAttachment{{{*depth, 0}}};
            for (std::size_t i = 0; i < inputs.shadow_uploads.size(); ++i)
                bundle.push_back({effectPortSemantic(ShadowBundle::members[i]), {inputs.shadow_uploads[i], 0}});
            const auto drawn = context.addRasterPass(key + "raster", std::move(raster));
            if (!drawn) return cxx::unexpected(drawn.error());
            bundle.push_back({effectPortSemantic(ShadowBundle::members.back()), drawn->written.front()});
            if (auto published = context.publish(Shadow::shadows, std::move(bundle)); !published)
                return cxx::unexpected(published.error());
            return drawn->pass;
        }

        template <bool CounterClockwise>
        RenderResult<void> build(EffectCompileContext& context, std::string_view) noexcept
        {
            constexpr detail::MeshSurfaceOutput output{Shadow::shadows, rdesc::EMaterialPurpose::SHADOW, appendSurface};
            return detail::appendShadowVisibility(context, output, CounterClockwise);
        }
    }

    EffectNodeDefinition Shadow::definition() noexcept
    {
        EffectNodeDefinition definition;
        definition.identity = {graph::nodeTypeId(capability), std::string{capability}, version};
        definition.display_name = "Shadow";
        definition.ports = {
            {shadows, "shadows", {std::string{ShadowBundle::type}, ShadowBundle::version},
                graph::EPinDirection::OUTPUT, UINT8_MAX},
            {draws, "draws", {std::string{MeshVisibility::draws_type}, 1}, graph::EPinDirection::OUTPUT, UINT8_MAX}
        };
        return definition;
    }

    std::array<EffectImplementation, 2> detail::shadowImplementations() noexcept
    {
        const auto variant = [](std::string_view name, EffectImplementation::Build build)
        {
            return EffectImplementation{
                .capability = graph::nodeTypeId(Shadow::capability),
                .identity = {std::string{name}, Shadow::version},
                .supported_outputs = {Shadow::shadows, Shadow::draws},
                .initialize = initializeShadow,
                .build = build
            };
        };
        return {
            variant(Shadow::indexed, build<true>),
            variant(Shadow::indexed_clockwise, build<false>)
        };
    }
}
