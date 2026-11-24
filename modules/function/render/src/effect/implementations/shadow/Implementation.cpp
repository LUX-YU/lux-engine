#include <lux/engine/render/resource/instance/Resource.hpp>
#include <lux/engine/render/effect/implementations/mesh_visibility/MeshCulling.hpp>
#include <lux/engine/render/assembly/BuiltinEffects.hpp>
#include <lux/engine/render/effect/capabilities/ShadowRendering.hpp>
#include <lux/engine/render/effect/implementations/common/EffectArguments.hpp>
#include <lux/engine/render/effect/capabilities/MeshVisibility.hpp>
#include <lux/engine/render/effect/implementations/shadow/ShadowProjection.hpp>
#include <cmath>

namespace lux::render
{
    namespace
    {
        struct ShadowSettings final
        {
            detail::ShadowProjectionSettings projection{{1024, 1}, 100, 1, 0};
            bool frustum_culling = true;
        };

        template <bool CounterClockwise>
        class Shadow final : public EffectImplementation
        {
        public:
            explicit Shadow(ShadowSettings settings) noexcept : EffectImplementation(sizeof(settings)), settings_(settings) {}
            RenderResult<void> onBuild(EffectCompileContext&) const noexcept override;

        private:
            ShadowSettings settings_;
        };

        template <bool CounterClockwise>
        EffectCreateResult createShadow(graph::NodeId node, std::string_view arguments) noexcept
        {
            constexpr ShadowSettings defaults;
            constexpr auto resolution = cxx::args::Option<std::uint32_t, "resolution">{}
                .def(defaults.projection.shape.resolution);
            constexpr auto distance = cxx::args::Option<float, "distance">{}.def(defaults.projection.distance).check(
                [](float value) noexcept { return std::isfinite(value) && value > 0; }
            );
            constexpr auto radius = cxx::args::Option<std::uint32_t, "pcf-radius">{}
                .def(defaults.projection.pcf_radius).check(cxx::args::inRange(0u, 4u));
            constexpr auto kFrustumCulling = cxx::args::Option<bool, "frustum-culling">{}.def(true);
            const auto parsed = cxx::args::parse(arguments, resolution, distance, radius, kFrustumCulling);
            if (!parsed) return cxx::unexpected(detail::effectArgumentError(node, parsed.error()));
            auto configuration = defaults;
            auto& settings = configuration.projection;
            settings.shape.resolution = *parsed.get<"resolution">();
            settings.distance = *parsed.get<"distance">();
            settings.pcf_radius = *parsed.get<"pcf-radius">();
            configuration.frustum_culling = *parsed.get<"frustum-culling">();
            if (settings.shape.resolution <= 2 * (settings.pcf_radius + 2))
                return cxx::unexpected(RenderError{kInvalidConfig, {node.value}});
            return std::make_unique<Shadow<CounterClockwise>>(configuration);
        }

        template <bool CounterClockwise>
        RenderResult<void> Shadow<CounterClockwise>::onBuild(EffectCompileContext& context) const noexcept
        {
            const bool needs_depth = context.outputDemanded(ShadowRenderingPorts::shadows.semantic);
            std::optional<detail::MeshRasterAttachments> raster;
            if (needs_depth)
            {
                const auto key = "mesh-visibility/" + std::to_string(context.nodeId().value) + "/shadow-depth";
                const auto depth = context.declareResource({key, settings_.projection.shape.image()});
                if (!depth) return cxx::unexpected(depth.error());
                raster.emplace(rdesc::EMaterialPurpose::SHADOW, CounterClockwise, RasterWork{});
                raster->work.depth = RasterAttachment{{{*depth, 0}}};
            }
            auto draws = detail::appendShadowVisibility(context, std::move(raster), settings_.projection, settings_.frustum_culling);
            if (!draws) return cxx::unexpected(draws.error());
            if (!needs_depth) return {};
            EffectResourceBundle bundle;
            for (std::size_t i = 0; i < draws->shadow_buffers.size(); ++i)
                bundle.push_back({effectPortSemantic(ShadowBundle::members[i]), {draws->shadow_buffers[i], 0}});
            bundle.push_back({effectPortSemantic(ShadowBundle::members.back()), draws->raster.written.front()});
            return context.publish(ShadowRenderingPorts::shadows.semantic, std::move(bundle));
        }
    }

    std::array<ImplementationEntry, 2> detail::shadowImplementations() noexcept
    {
        const auto variant = [](std::string_view name, ImplementationEntry::Create create)
        {
            return ImplementationEntry{
                .capability = capabilities::ShadowRendering.id,
                .identity = {std::string{name}, capabilities::ShadowRendering.version},
                .supported_outputs = {ShadowRenderingPorts::shadows.semantic, ShadowRenderingPorts::draws.semantic},
                .create = create
            };
        };
        return {
            variant("lux.render.shadow.indexed", createShadow<true>),
            variant("lux.render.shadow.indexed.clockwise", createShadow<false>)
        };
    }
}
