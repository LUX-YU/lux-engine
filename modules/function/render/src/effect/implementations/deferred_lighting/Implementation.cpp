#include <lux/engine/render/resource/lighting/Binding.hpp>
#include <lux/engine/render/frame/FrameError.hpp>
#include "DeferredLighting.hpp"
#include "DeferredLightingClustered.hpp"
#include "DeferredLightingClusteredShadow.hpp"
#include "DeferredLightingShadow.hpp"
#include <algorithm>
#include <cmath>
#include <lux/engine/render/effect/implementations/common/EffectArguments.hpp>
#include <lux/engine/render/effect/capabilities/DeferredGeometry.hpp>
#include <lux/engine/render/effect/capabilities/ShadowRendering.hpp>
#include <lux/engine/render/assembly/BuiltinEffects.hpp>
#include <lux/engine/render/effect/capabilities/DeferredLighting.hpp>
#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/render/effect/EffectImplementation.hpp>
#include <lux/engine/render/effect/implementations/common/FullscreenRaster.hpp>
#include <lux/engine/render/effect/implementations/deferred_lighting/ClusteredLightingWork.hpp>
#include <lux/engine/render/effect/implementations/deferred_lighting/DeferredLightingWork.hpp>
#include <lux/engine/render/resource/lighting/Resource.hpp>
#include <lux/engine/render/resource/view/Resource.hpp>
#include <lux/engine/render/runtime/commands/CommandBinding.hpp>
#include <lux/engine/render/runtime/commands/LightingCommands.hpp>
#include <array>

namespace lux::render::detail
{
    struct alignas(16) LightingEnvironment final
    {
        std::array<float, 4> radiance{};
    };

    struct LightingState final
    {
        LightingEnvironment environment;

        [[nodiscard]] RenderResult<void> setEnvironment(const std::array<float, 3>& radiance) noexcept
        {
            const bool invalid = std::ranges::any_of(radiance, [](float channel) noexcept {
                return !std::isfinite(channel) || channel < 0;
            });
            if (invalid) return cxx::unexpected(RenderError{kFrameContract});
            std::ranges::copy(radiance, environment.radiance.begin());
            return {};
        }
    };
}

namespace lux::render::detail
{
    RenderResult<RasterWork>
    deferredLightingWork(EffectCompileContext& builder, const DeferredLightingInputs& inputs) noexcept
    {
        constexpr std::array shaders{
            &kDeferredLighting, &kDeferredLightingShadow, &kDeferredLightingClustered, &kDeferredLightingClusteredShadow
        };
        const auto variant = 2 * unsigned(inputs.clusters.has_value()) + unsigned(inputs.shadows.has_value());
        auto work = fullscreenRaster(builder, GraphProgram{*shaders[variant]});
        if (!work) return cxx::unexpected(work.error());
        auto& shader = work->fixed().interface;
        const auto bind = [&](const auto& names, const auto& versions) noexcept
        {
            for (std::size_t i = 0; i < names.size(); ++i)
                shader.resources.push_back({std::string{names[i]}, {versions[i]}});
        };
        bind(GBufferBundle::members, inputs.surface);
        bind(kLightBindings, inputs.lights);
        shader.resources.push_back({"scene_view", {inputs.view}});
        if (inputs.shadows)
        {
            bind(ShadowBundle::members, *inputs.shadows);
            const auto sampler =
                builder.addSampler({ESamplerFilter::NEAREST, ESamplerAddress::CLAMP_TO_BORDER, EDepthCompare::LESS_EQUAL, 1});
            shader.samplers.push_back({"shadow_compare", {sampler}, kGraphInvalidIndex, true});
        }
        if (inputs.clusters)
        {
            constexpr std::array members{"cluster_grid", "cluster_offsets", "cluster_indices", "cluster_status"};
            bind(members, *inputs.clusters);
        }
        return work;
    }

} // namespace lux::render::detail

namespace lux::render
{
    template <> struct ConstantBlockTraits<detail::LightingEnvironment> final
    {
        static ConstantBlockLayout layout(rdesc::EShaderTarget) noexcept
        {
            using Block = detail::LightingEnvironment;
            static constexpr std::array members{
                ConstantMemberLayout{"environment", offsetof(Block, radiance), rdesc::EDataScalar::FLOAT, 4}
            };
            return {sizeof(Block), alignof(Block), members};
        }
    };

    namespace
    {
        using BundleVersions = std::array<ResourceVersion, 4>;
        RenderResult<BundleVersions> resolveLightingBundle(
            const auto& input,
            const std::array<std::string_view, 4>& members,
            graph::NodeId node
        ) noexcept
        {
            if (input.size() != 1 || input.front().size() != members.size())
                return cxx::unexpected(RenderError{kCapabilityDeclaration, {node.value}});
            BundleVersions result;
            const auto& bundle = input.front();
            for (std::size_t i = 0; i < members.size(); ++i)
            {
                const auto member = std::ranges::find(bundle, effectPortSemantic(members[i]), &EffectResourceValue::member);
                if (member == bundle.end()) return cxx::unexpected(RenderError{kCapabilityDeclaration, {node.value, i}});
                result[i] = member->version;
            }
            return result;
        }

        RenderResult<ClusterGrid> parseClusterGrid(graph::NodeId node, std::string_view arguments) noexcept
        {
            constexpr ClusterGrid defaults;
            constexpr auto count_range = cxx::args::inRange(1u, UINT32_MAX - 1);
            constexpr auto x = cxx::args::Option<std::uint32_t, "x">{}.def(defaults.cells[0]).check(count_range);
            constexpr auto y = cxx::args::Option<std::uint32_t, "y">{}.def(defaults.cells[1]).check(count_range);
            constexpr auto z = cxx::args::Option<std::uint32_t, "z">{}.def(defaults.cells[2]).check(count_range);
            constexpr auto capacity = cxx::args::Option<std::uint32_t, "index-capacity">{}
                .def(defaults.index_capacity).check(count_range);
            constexpr auto depth = cxx::args::Option<EClusterDepth, "depth">{}.def(defaults.depth).convert(
                [](std::string_view text, EClusterDepth& value) noexcept
                {
                    if (text == "linear") value = EClusterDepth::LINEAR;
                    else if (text == "logarithmic") value = EClusterDepth::LOGARITHMIC;
                    else return cxx::args::EErrorCode::INVALID_VALUE;
                    return cxx::args::EErrorCode::OK;
                }
            );
            const auto parsed = cxx::args::parse(arguments, x, y, z, capacity, depth);
            if (!parsed) return cxx::unexpected(detail::effectArgumentError(node, parsed.error()));
            const ClusterGrid grid{
                {*parsed.get<"x">(), *parsed.get<"y">(), *parsed.get<"z">()},
                *parsed.get<"index-capacity">(),
                *parsed.get<"depth">()
            };
            if (!grid.valid()) return cxx::unexpected(RenderError{kInvalidConfig, {node.value}});
            return grid;
        }

        template <bool Clustered>
        class Lighting final : public EffectImplementation
        {
        public:
            explicit Lighting(ClusterGrid grid = {}) noexcept
                : EffectImplementation(Clustered ? sizeof(grid) : 0), grid_(grid) { bindState(state_); }
            RenderResult<void> onBuild(EffectCompileContext&) const noexcept override;

        private:
            ClusterGrid grid_;
            detail::LightingState state_;
        };

        template <bool Clustered>
        EffectCreateResult createLighting(graph::NodeId node, std::string_view arguments) noexcept
        {
            if constexpr (Clustered)
            {
                auto grid = parseClusterGrid(node, arguments);
                if (!grid) return cxx::unexpected(grid.error());
                return std::make_unique<Lighting<true>>(*grid);
            }
            else
            {
                if (!arguments.empty()) return cxx::unexpected(RenderError{kInvalidConfig, {node.value}});
                return std::make_unique<Lighting<false>>();
            }
        }

        template <bool Clustered>
        RenderResult<void> Lighting<Clustered>::onBuild(EffectCompileContext& context) const noexcept
        {
            const auto node = context.nodeId();
            auto surface = resolveLightingBundle(context.inputs(DeferredLightingPorts::surface.semantic), GBufferBundle::members, node);
            if (!surface) return cxx::unexpected(surface.error());
            detail::DeferredLightingInputs lighting{*surface, {}};
            GraphImageDesc extent;
            for (std::size_t i = 0; i < surface->size(); ++i)
            {
                const auto* description = context.resourceDescription((*surface)[i].resource);
                const auto* image = description ? std::get_if<GraphImageDesc>(description) : nullptr;
                const auto format = GBufferBundle::formats[i];
                const bool invalid_shape = image &&
                    (image->format != format || image->mips != 1 || image->layers != 1 ||
                     image->runtime_layers || image->samples != 1);
                if (!image || invalid_shape)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {node.value, i}});
                auto shape = *image;
                shape.format = rdesc::ETextureFormat::RGBA16_SFLOAT;
                if (i == 0)
                    extent = shape;
                else if (shape != extent)
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor, {node.value, i}});
            }
            const auto shadows = context.inputs(DeferredLightingPorts::shadows.semantic);
            if (!shadows.empty())
            {
                auto bound = resolveLightingBundle(shadows, ShadowBundle::members, node);
                if (!bound) return cxx::unexpected(bound.error());
                lighting.shadows = *bound;
            }
            constexpr std::array sizes{
                sizeof(DirectionalLightGpu), sizeof(PointLightGpu), sizeof(SpotLightGpu), sizeof(std::uint32_t) * 4
            };
            for (std::uint32_t index = 0; index < sizes.size(); ++index)
            {
                const auto bytes = sizes[index];
                auto input = context.importProjection(kSceneLights, kLightBindings[index], {
                    GraphBufferDesc{bytes}, {GraphBufferRange{0, bytes}},
                    EGraphUse::STORAGE_READ
                }, index);
                if (!input) return cxx::unexpected(input.error());
                lighting.lights[index] = *input;
            }
            const std::string key = "lighting/" + std::to_string(node.value);
            auto view = context.importProjection(
                kViewParameters, ViewParameterResource::gpu_binding, ViewParameterResource::projection()
            );
            if (!view) return cxx::unexpected(view.error());
            lighting.view = *view;
            if constexpr (Clustered)
            {
                const auto cluster_key = key + "/clusters";
                const auto lists = detail::buildClusterLightLists(context, cluster_key, grid_, lighting);
                if (!lists) return cxx::unexpected(lists.error());
                const auto& b = *lists;
                lighting.clusters = {{b.grid, b.offsets, b.indices, b.status}};
                if (context.outputDemanded(DeferredLightingPorts::cluster_lists.semantic))
                {
                    EffectResourceBundle bundle;
                    for (const auto& [name, field] : detail::clusterBindings)
                        bundle.push_back({effectPortSemantic(name), b.*field});
                    if (auto published = context.publish(DeferredLightingPorts::cluster_lists.semantic, std::move(bundle)); !published)
                        return published;
                }
            }
            if (context.outputDemanded(DeferredLightingPorts::hdr.semantic))
            {
                const auto hdr = context.declareResource({key, extent});
                if (!hdr) return cxx::unexpected(hdr.error());
                auto work = detail::deferredLightingWork(context, lighting);
                if (!work) return cxx::unexpected(work.error());
                work->fixed().interface.parameters.setInlineConstant(detail::LightingEnvironment{});
                work->colors.push_back({{{*hdr, 0}}});
                auto pass = context.addRasterPass(key, std::move(*work));
                if (!pass) return cxx::unexpected(pass.error());
                auto copy = context.copyInlineBlock<detail::LightingEnvironment, detail::LightingState,
                                                    &detail::LightingState::environment>(pass->pass);
                if (!copy) return copy;
                if (auto published = context.publish(DeferredLightingPorts::hdr.semantic, {{DeferredLightingPorts::hdr.semantic, pass->written.front()}}); !published)
                    return published;
            }
            return {};
        }
    }

    std::array<ImplementationEntry, 2> detail::lightingImplementations() noexcept
    {
        ImplementationEntry implementation;
        implementation.capability = capabilities::DeferredLighting.id;
        implementation.identity = {"lux.render.lighting.deferred.direct", capabilities::DeferredLighting.version};
        implementation.supported_outputs = {DeferredLightingPorts::hdr.semantic};
        implementation.create = createLighting<false>;
        auto clustered = implementation;
        clustered.identity.canonical_name = "lux.render.lighting.deferred.clustered";
        clustered.supported_outputs.push_back(DeferredLightingPorts::cluster_lists.semantic);
        clustered.create = createLighting<true>;
        return {std::move(implementation), std::move(clustered)};
    }
} // namespace lux::render

namespace lux::render::detail
{
    namespace
    {
        void update(LightingState& state, const SetLightingEnvironment::Request& request, AckResponse& response) noexcept
        {
            const auto result = state.setEnvironment(request.radiance);
            response.error = result ? RenderError{} : result.error();
        }
    }

    CommandBinding lightingEnvironmentBinding() noexcept
    {
        return CommandBinding::declareForEffect<SetLightingEnvironment, LightingState, &update>();
    }
}
