#include <lux/engine/render/frame/FrameError.hpp>
#include "DeferredLighting.hpp"
#include "DeferredLightingClustered.hpp"
#include "DeferredLightingClusteredShadow.hpp"
#include "DeferredLightingShadow.hpp"
#include <algorithm>
#include <cmath>
#include <lux/engine/render/effect/EffectArguments.hpp>
#include <lux/engine/render/effect/builtin/GBuffer.hpp>
#include <lux/engine/render/effect/builtin/Shadow.hpp>
#include <lux/engine/render/effect/builtin/Implementations.hpp>
#include <lux/engine/render/effect/builtin/Lighting.hpp>
#include <lux/engine/render/features/lighting/LightingState.hpp>
#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/render/effect/EffectImplementation.hpp>
#include <lux/engine/render/effect/EffectInitializationContext.hpp>
#include <lux/engine/render/features/common/FullscreenRaster.hpp>
#include <lux/engine/render/features/lighting/ClusteredLightingWork.hpp>
#include <lux/engine/render/features/lighting/DeferredLightingWork.hpp>
#include <lux/engine/render/resource/lighting/Resource.hpp>
#include <lux/engine/render/resource/view/Resource.hpp>

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
        bind(Lighting::light_bindings, inputs.lights);
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

    struct alignas(16) LightingConstants final
    {
        TransformMatrix inverse_view_projection;
        std::array<float, 4> camera_position, viewport, background, environment;
    };
} // namespace lux::render::detail

namespace lux::render
{
    template <> struct ConstantBlockTraits<detail::LightingConstants> final
    {
        static ConstantBlockLayout layout(rdesc::EShaderTarget) noexcept
        {
            using Block = detail::LightingConstants;
            constexpr auto scalar = rdesc::EDataScalar::FLOAT;
            static constexpr std::array members{
                ConstantMemberLayout{"inverse_view_projection", offsetof(Block, inverse_view_projection),
                    scalar, 4, 4, 16, rdesc::EShaderMatrixOrder::COLUMN_MAJOR},
                ConstantMemberLayout{"camera_position", offsetof(Block, camera_position), scalar, 4},
                ConstantMemberLayout{"viewport", offsetof(Block, viewport), scalar, 4},
                ConstantMemberLayout{"background", offsetof(Block, background), scalar, 4},
                ConstantMemberLayout{"environment", offsetof(Block, environment), scalar, 4}
            };
            return {sizeof(Block), alignof(Block), members};
        }
    };

    namespace
    {
        struct ClusterPreparation final
        {
            using Configuration = detail::ClusterBuildConstants;
            using LinkedData = Configuration;

            static RenderResult<std::tuple<detail::ClusterBuildConstants>>
            prepare(const LinkedData& base, const CpuFrameInputs<const ViewGpuParameters&>& inputs) noexcept
            {
                const auto& view = std::get<0>(inputs.values).value.get();
                auto constants = base;
                constants.inverse_view_projection = view.inverse_view_projection;
                constants.position_near = view.position_near;
                constants.direction_far = view.direction_far;
                return std::tuple{constants};
            }
        };

        struct LightingPreparation final
        {
            using Configuration = std::monostate;
            using LinkedData = Configuration;

            static RenderResult<std::tuple<detail::LightingConstants>>
            prepare(const LinkedData&, const CpuFrameInputs<detail::LightingState, const ViewGpuParameters&>& inputs) noexcept
            {
                const auto& [state, camera] = inputs.values;
                const auto& view = camera.value.get();
                return std::tuple{detail::LightingConstants{
                    view.inverse_view_projection, view.position_near, view.viewport, view.background, state.value.environment
                }};
            }
        };

        using BundleVersions = std::array<ResourceVersion, 4>;
        RenderResult<BundleVersions> resolveLightingBundle(
            std::span<const EffectResourceBundle> input,
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
        RenderResult<void> initializeLighting(EffectInitializationContext& context, std::string_view arguments) noexcept
        {
            if constexpr (Clustered)
            {
                auto grid = parseClusterGrid(context.nodeId(), arguments);
                if (!grid) return cxx::unexpected(grid.error());
                if (auto configured = context.setConfiguration(*grid); !configured) return configured;
            }
            else if (!arguments.empty())
                return cxx::unexpected(RenderError{kInvalidConfig, {context.nodeId().value}});
            return context.initializeState<detail::LightingState>();
        }

        template <bool Clustered>
        RenderResult<void> buildLighting(EffectCompileContext& context, std::string_view) noexcept
        {
            const auto node = context.nodeId();
            auto surface = resolveLightingBundle(context.inputs(Lighting::surface), GBufferBundle::members, node);
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
            const auto shadows = context.inputs(Lighting::shadows);
            if (!shadows.empty())
            {
                auto bound = resolveLightingBundle(shadows, ShadowBundle::members, node);
                if (!bound) return cxx::unexpected(bound.error());
                lighting.shadows = *bound;
            }
            constexpr std::array sizes{
                sizeof(DirectionalLightGpu), sizeof(PointLightGpu), sizeof(SpotLightGpu), sizeof(std::uint32_t) * 4
            };
            const ResourceRequirement owner{ResourceName{kLightResourceName.name()}};
            for (std::uint32_t index = 0; index < sizes.size(); ++index)
            {
                const auto bytes = sizes[index];
                auto input = context.importResource(Lighting::light_bindings[index], owner, {
                    "", GraphBufferDesc{bytes}, EGraphOrigin::IMPORTED, {GraphBufferRange{0, bytes}},
                    EGraphUse::STORAGE_READ
                }, index);
                if (!input) return cxx::unexpected(input.error());
                lighting.lights[index] = *input;
            }
            const std::string key = "lighting/" + std::to_string(node.value);
            const auto camera = context.importStorageSnapshot<ViewGpuParameters>(
                ViewParameterResource::snapshot_binding, {ResourceName{kViewParameterResourceName.name()}},
                ESnapshotScope::JOB_LOCAL
            );
            if (!camera) return cxx::unexpected(camera.error());
            if constexpr (Clustered)
            {
                const auto* grid = context.configuration<ClusterGrid>();
                if (!grid) return cxx::unexpected(RenderError{kCapabilityDeclaration, {node.value}});
                const auto cluster_key = key + "/clusters";
                const auto lists = detail::buildClusterLightLists(context, cluster_key, *grid, lighting.lights);
                if (!lists) return cxx::unexpected(lists.error());
                const auto& b = lists->buffers;
                lighting.clusters = {{b.grid, b.offsets, b.indices, b.status}};
                if (context.outputDemanded(Lighting::cluster_lists))
                {
                    EffectResourceBundle bundle;
                    for (const auto& [name, field] : detail::clusterBindings)
                        bundle.push_back({effectPortSemantic(name), b.*field});
                    if (auto published = context.publish(Lighting::cluster_lists, std::move(bundle)); !published)
                        return published;
                }
                auto prepared = context.addCpuCall<ClusterPreparation>(detail::clusterBuildConstants(*grid), {
                    .inline_blocks = {lists->build}, .storage_inputs = {*camera}
                });
                if (!prepared) return prepared;
            }
            if (context.outputDemanded(Lighting::hdr))
            {
                const auto hdr = context.declareResource({key, extent});
                if (!hdr) return cxx::unexpected(hdr.error());
                auto work = detail::deferredLightingWork(context, lighting);
                if (!work) return cxx::unexpected(work.error());
                work->fixed().interface.parameters.setInlineConstant(detail::LightingConstants{});
                work->colors.push_back({{{*hdr, 0}}});
                auto pass = context.addRasterPass(key, std::move(*work));
                if (!pass) return cxx::unexpected(pass.error());
                auto state = context.importEffectState<detail::LightingState>();
                if (!state) return cxx::unexpected(state.error());
                auto prepared = context.addCpuCall<LightingPreparation>({}, {
                    .inputs = {*state}, .inline_blocks = {pass->pass}, .storage_inputs = {*camera}
                });
                if (!prepared) return prepared;
                if (auto published = context.publish(Lighting::hdr, {{Lighting::hdr, pass->written.front()}}); !published)
                    return published;
            }
            return {};
        }
    }

    EffectNodeDefinition Lighting::definition() noexcept
    {
        EffectNodeDefinition definition;
        definition.identity = {graph::nodeTypeId(capability), std::string{capability}, version};
        definition.display_name = "Lighting";
        definition.ports = {
            {surface, "surface", {std::string{GBufferBundle::type}, GBufferBundle::version},
                graph::EPinDirection::INPUT, 1, true},
            {shadows, "shadows", {std::string{ShadowBundle::type}, ShadowBundle::version}, graph::EPinDirection::INPUT, 1},
            {hdr, "hdr_color", {std::string{HdrColorBundle::type}, HdrColorBundle::version},
                graph::EPinDirection::OUTPUT, UINT8_MAX},
            {cluster_lists, "cluster_lists", {std::string{ClusterListBundle::type}, ClusterListBundle::version},
                graph::EPinDirection::OUTPUT, UINT8_MAX}
        };
        return definition;
    }

    std::array<EffectImplementation, 2> detail::lightingImplementations() noexcept
    {
        EffectImplementation implementation;
        implementation.capability = graph::nodeTypeId(Lighting::capability);
        implementation.identity = {std::string{Lighting::direct}, Lighting::version};
        implementation.supported_outputs = {Lighting::hdr};
        implementation.initialize = initializeLighting<false>;
        implementation.build = buildLighting<false>;
        auto clustered = implementation;
        clustered.identity.canonical_name = Lighting::clustered;
        clustered.supported_outputs.push_back(Lighting::cluster_lists);
        clustered.initialize = initializeLighting<true>;
        clustered.build = buildLighting<true>;
        return {std::move(implementation), std::move(clustered)};
    }
} // namespace lux::render
