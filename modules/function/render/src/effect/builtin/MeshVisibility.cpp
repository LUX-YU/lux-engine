#include <lux/engine/render/frame/FrameError.hpp>
#include "MeshView.hpp"
#include <lux/engine/render/effect/EffectArguments.hpp>
#include <lux/engine/render/effect/builtin/MeshVisibilityBuild.hpp>
#include <lux/engine/render/effect/builtin/Implementations.hpp>
#include <lux/engine/render/effect/builtin/GBuffer.hpp>
#include <lux/engine/render/effect/MeshSurfaceStream.hpp>
#include <lux/engine/render/effect/builtin/MeshVisibility.hpp>
#include <lux/engine/render/effect/builtin/Shadow.hpp>
#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/render/effect/EffectImplementation.hpp>
#include <lux/engine/render/effect/EffectInitializationContext.hpp>
#include <lux/engine/render/features/mesh/MeshGrouping.hpp>
#include <lux/engine/render/features/mesh/RasterMesh.hpp>
#include <lux/engine/render/features/mesh/MeshVisibilityInputs.hpp>
#include <lux/engine/render/features/mesh/MeshVisibilityWork.hpp>
#include <lux/engine/render/features/shadow/ShadowProjection.hpp>
#include <lux/engine/render/resource/instance/Binding.hpp>
#include <lux/engine/render/resource/instance/Resource.hpp>
#include <lux/engine/render/resource/material/Resource.hpp>
#include <lux/engine/render/resource/mesh/Resource.hpp>
#include <lux/engine/render/resource/texture/Resource.hpp>
#include <lux/engine/render/resource/lighting/Resource.hpp>
#include <lux/engine/render/resource/view/Resource.hpp>
#include <lux/engine/render/resource/transform/Binding.hpp>
#include <lux/engine/render/resource/transform/Resource.hpp>
#include <NRI.h>
#include <algorithm>
#include <cmath>

namespace lux::render
{
    namespace
    {
        template <bool ShadowPass> class PrepareMeshVisibility final
        {
        public:
            struct Configuration final
            {
                std::array<std::uint32_t, 2> dispatch_limit;
                std::optional<detail::MeshSurfaceStream::Configuration> surface;
                std::optional<detail::ShadowProjectionSettings> shadow;
            };

            struct LinkedData final
            {
                std::array<std::uint32_t, 2> dispatch_limit;
                std::optional<detail::MeshSurfaceStream::Inputs> surface;
                std::optional<detail::ShadowProjectionSettings> shadow;
            };

            [[nodiscard]] static RenderResult<LinkedData>
            resolve(const Configuration& config, std::span<const RasterOperands> streams) noexcept
            {
                if (streams.size() != static_cast<std::size_t>(config.surface.has_value()))
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                std::optional<detail::MeshSurfaceStream::Inputs> surface;
                if (config.surface)
                {
                    auto resolved = detail::MeshSurfaceStream::resolve(*config.surface, streams.front());
                    if (!resolved) return cxx::unexpected(resolved.error());
                    surface = *resolved;
                }
                return LinkedData{config.dispatch_limit, surface, config.shadow};
            }

            struct Workspace final
            {
                detail::MeshGrouping grouping;
                std::optional<detail::MeshSurfaceStream> surface;
                std::optional<detail::ShadowProjectionData> shadow;
                std::vector<std::uint32_t> previous_slots, current_slots;
                std::optional<std::uint64_t> selection;
            };

            static RenderResult<std::unique_ptr<Workspace>>
            createWorkspace(const LinkedData& inputs, const TargetDesc&) noexcept
            {
                auto workspace = std::make_unique<Workspace>();
                if (inputs.surface) workspace->surface.emplace(*inputs.surface);
                if (inputs.shadow) workspace->shadow.emplace(*inputs.shadow);
                return workspace;
            }

            using Surface = detail::MeshSurfaceStream;
            using DrawMapping = Surface::DrawMapping;

            static RenderResult<std::unique_ptr<DrawMapping>>
            buildPrograms(
                const LinkedData& linked,
                RasterProgramBuildContext& selection,
                const SnapshotValues<MeshInstanceSnapshot>& sources,
                const DrawMapping* previous
            ) noexcept
            {
                if (!linked.surface) return cxx::unexpected(RenderError{kFrameContract});
                return Surface::buildPrograms(*linked.surface, selection, std::get<0>(sources).value.get(), previous);
            }

            using FrameInputs = std::conditional_t<ShadowPass,
                CpuFrameInputs<const MeshInstanceSnapshot&, const ViewGpuParameters&, const ShadowLightSnapshot&>,
                CpuFrameInputs<const MeshInstanceSnapshot&, const ViewGpuParameters&>
            >;

            using Result = std::conditional_t<ShadowPass, std::tuple<>, std::tuple<detail::VisibilityViewConstants>>;

            static RenderResult<Result>
            prepare(
                const LinkedData& linked,
                const FrameInputs& inputs,
                Workspace& workspace,
                const DrawMapping* mapping
            ) noexcept
            {
                auto& frame = inputs.frame;
                const auto& view = std::get<1>(inputs.values).value.get();
                if constexpr (ShadowPass)
                {
                    const auto& lights = std::get<2>(inputs.values).value.get();
                    const detail::ShadowCamera camera{
                        view.inverse_view_projection, view.position_near, view.direction_far
                    };
                    const auto projected = workspace.shadow->select(camera, lights);
                    if (!projected) return cxx::unexpected(projected.error());
                    workspace.grouping.setViewCount(static_cast<std::uint32_t>(workspace.shadow->views().size()));
                    const std::array payloads{
                        std::as_bytes(workspace.shadow->views()),
                        std::as_bytes(workspace.shadow->slices()),
                        workspace.shadow->mappings()
                    };
                    constexpr auto first_shadow = std::tuple_size_v<decltype(workspace.grouping.uploads())>;
                    for (std::size_t i = 0; i < payloads.size(); ++i)
                    {
                        const auto written = frame.writeBuffer(
                            first_shadow + i, payloads[i], workspace.shadow->revision()
                        );
                        if (!written) return cxx::unexpected(written.error());
                    }
                }
                const auto& selected = std::get<0>(inputs.values);
                const auto& snapshot = selected.value.get();
                if (workspace.selection != selected.revision)
                {
                    updateGroups(snapshot, workspace);
                    workspace.selection = selected.revision;
                }
                auto packed = workspace.grouping.pack(&snapshot, lookupGeometry, linked.dispatch_limit);
                if (!packed) return cxx::unexpected(packed.error());
                const auto payloads = workspace.grouping.uploads();
                for (std::size_t i = 0; i < payloads.size(); ++i)
                {
                    auto written = frame.writeBuffer(i, payloads[i], workspace.grouping.revision());
                    if (!written) return cxx::unexpected(written.error());
                }
                auto sized = frame.sizeBuffer(0, workspace.grouping.visibleBytes());
                if (!sized) return cxx::unexpected(sized.error());
                auto commands = frame.sizeBuffer(
                    1, workspace.grouping.indirectStreamCount() * sizeof(nri::DrawIndexedDesc)
                );
                if (!commands) return cxx::unexpected(commands.error());
                if (workspace.surface)
                {
                    if (!mapping) return cxx::unexpected(RenderError{kFrameContract});
                    const Surface::DrawInputs draw_inputs{frame, workspace.grouping.source(), *mapping};
                    RenderResult<void> prepared;
                    if constexpr (ShadowPass)
                        prepared = workspace.surface->prepareShadow(
                            draw_inputs, workspace.shadow->views(), workspace.shadow->rasterStates()
                        );
                    else prepared = workspace.surface->prepare(draw_inputs, view);
                    if (!prepared) return cxx::unexpected(prepared.error());
                }
                if constexpr (ShadowPass) return Result{};
                else return Result{detail::VisibilityViewConstants{view.view, view.projection}};
            }

        private:
            static RenderResult<MeshArenaSection>
            lookupGeometry(const void* input, MeshId mesh, std::uint32_t submesh) noexcept
            {
                const auto& sections = static_cast<const MeshInstanceSnapshot*>(input)->sections;
                const auto key = std::pair{mesh, submesh};
                const auto section = std::ranges::lower_bound(
                    sections, key, std::less<>{},
                    [](const auto& entry) noexcept { return std::pair{entry.mesh, entry.submesh}; }
                );
                if (section == sections.end() || section->mesh != mesh || section->submesh != submesh)
                    return cxx::unexpected(RenderError{kStorageIdentity});
                return section->arena;
            }

            static void updateGroups(const MeshInstanceSnapshot& snapshot, Workspace& workspace) noexcept
            {
                workspace.current_slots.clear();
                workspace.current_slots.reserve(snapshot.instances.size());
                for (const auto& instance : snapshot.instances)
                {
                    if (!instance.render_enabled) continue;
                    const auto slot = extension_detail::ResourceIdentity::local(instance.instance).index;
                    const detail::MeshGrouping::Selection selected{
                        instance.mesh, instance.material, instance.program, instance.submesh
                    };
                    workspace.grouping.update(slot, &selected);
                    workspace.current_slots.push_back(slot);
                }
                std::ranges::sort(workspace.current_slots);
                auto active = workspace.current_slots.begin();
                for (auto slot : workspace.previous_slots)
                {
                    while (active != workspace.current_slots.end() && *active < slot) ++active;
                    if (active == workspace.current_slots.end() || *active != slot) workspace.grouping.update(slot, nullptr);
                }
                workspace.previous_slots.swap(workspace.current_slots);
            }
        };

        struct MeshVisibilityConfiguration final
        {
            bool frustum_culling;
        };

        constexpr auto kFrustumCulling = cxx::args::Option<bool, "frustum-culling">{}.def(true);
    }

    RenderResult<void>
    detail::initializeVisibility(EffectInitializationContext& context, std::string_view arguments) noexcept
    {
        const auto parsed = cxx::args::parse(arguments, kFrustumCulling);
        if (!parsed) return cxx::unexpected(detail::effectArgumentError(context.nodeId(), parsed.error()));
        return context.setConfiguration(MeshVisibilityConfiguration{*parsed.get<"frustum-culling">()});
    }

    namespace
    {
        template <bool ShadowPass>
        RenderResult<void> appendVisibility(
            EffectCompileContext& context, const detail::MeshSurfaceOutput* surface, bool counter_clockwise
        ) noexcept
        {
            using Kernel = PrepareMeshVisibility<ShadowPass>;
            const auto& device = context.device();
            const auto max_bytes = std::min(device.storage_buffer_max_range, UINT64_MAX / 2) & ~std::uint64_t{3};
            const auto read = [&](std::string_view binding, ResourceNameView owner, std::uint64_t bytes, auto projection)
            {
                return context.importResource(binding, {ResourceName{owner.name()}}, {
                    "", GraphBufferDesc{bytes}, EGraphOrigin::IMPORTED, {GraphBufferRange{0, bytes}},
                    EGraphUse::STORAGE_READ
                }, static_cast<std::uint32_t>(projection));
            };
            const bool paged = device.buffer_device_address;
            const auto transform_role = paged ? ETransformGraphProjection::PAGE_ADDRESSES :
                ETransformGraphProjection::RECORDS;
            const auto transform_bytes = paged ? kTransformInitialPageAddresses * sizeof(std::uint64_t) :
                sizeof(TransformMatrix);
            const auto transforms = read(MeshVisibility::transforms_binding, kTransformResourceName, transform_bytes,
                transform_role);
            if (!transforms) return cxx::unexpected(transforms.error());
            const auto properties_role = paged ? EMeshInstanceBinding::PAGED_ROOT : EMeshInstanceBinding::PROPERTIES;
            const auto properties = read(MeshVisibility::properties_binding, kInstanceResourceName,
                meshInstanceProjectionBytes(properties_role), properties_role);
            if (!properties) return cxx::unexpected(properties.error());
            constexpr auto alive_role = EMeshInstanceBinding::ALIVE;
            const auto alive = paged ? properties : read(MeshVisibility::alive_binding, kInstanceResourceName,
                meshInstanceProjectionBytes(alive_role), alive_role);
            if (!alive) return cxx::unexpected(alive.error());
            const auto instances = context.importStorageSnapshot<MeshInstanceSnapshot>(
                MeshVisibility::instances_binding, {ResourceName{kInstanceResourceName.name()}}
            );
            if (!instances) return cxx::unexpected(instances.error());

            const auto camera = context.importStorageSnapshot<ViewGpuParameters>(
                ViewParameterResource::snapshot_binding, {ResourceName{kViewParameterResourceName.name()}},
                ESnapshotScope::JOB_LOCAL
            );
            if (!camera) return cxx::unexpected(camera.error());
            const std::string key = "mesh-visibility/" + std::to_string(context.nodeId().value) + "/";
            ResourceVersion views;
            std::vector<GraphPassId> inline_blocks;
            std::vector<GraphStorageSnapshotId> storage_inputs{*instances, *camera};
            std::array<GraphResourceId, 3> shadow_uploads{};
            std::optional<detail::ShadowProjectionSettings> shadow;
            bool frustum_culling;
            if constexpr (ShadowPass)
            {
                const auto* configuration = context.configuration<detail::ShadowNodeConfiguration>();
                if (!configuration) return cxx::unexpected(RenderError{kCapabilityDeclaration, {context.nodeId().value}});
                auto settings = configuration->projection;
                settings.maximum_layers = device.texture_layer_limit;
                frustum_culling = configuration->frustum_culling;
                const auto lights = context.importStorageSnapshot<ShadowLightSnapshot>(
                    Shadow::lights_binding, {ResourceName{kLightResourceName.name()}}
                );
                if (!lights) return cxx::unexpected(lights.error());
                constexpr auto view_bytes = sizeof(MeshVisibilityView);
                const auto projected_views = context.declarePreparedBuffer(
                    "shadow-views", view_bytes, std::min(max_bytes, std::uint64_t{settings.maximum_layers} * view_bytes)
                );
                if (!projected_views) return cxx::unexpected(projected_views.error());
                constexpr auto slice_bytes = sizeof(ShadowSliceGpu);
                const auto slices = context.declarePreparedBuffer(
                    "shadow-slices", slice_bytes, std::min(max_bytes, std::uint64_t{settings.maximum_layers} * slice_bytes)
                );
                if (!slices) return cxx::unexpected(slices.error());
                const auto ranges = context.declarePreparedBuffer(
                    "shadow-ranges", sizeof(ShadowMappingHeader) + sizeof(LightShadowRange), max_bytes
                );
                if (!ranges) return cxx::unexpected(ranges.error());
                views = *projected_views;
                storage_inputs.push_back(*lights);
                shadow_uploads = {views.resource, slices->resource, ranges->resource};
                shadow = settings;
            }
            else
            {
                const auto* configuration = context.configuration<MeshVisibilityConfiguration>();
                if (!configuration) return cxx::unexpected(RenderError{kCapabilityDeclaration, {context.nodeId().value}});
                frustum_culling = configuration->frustum_culling;
                const auto view_resource = context.declareResource({key + "views", GraphBufferDesc{sizeof(MeshVisibilityView)}});
                if (!view_resource) return cxx::unexpected(view_resource.error());
                const auto shader = context.addProgram(GraphProgram{kMeshView.load(context.shaderTarget())});
                if (!shader) return cxx::unexpected(shader.error());
                ComputeWork view;
                view.interface.program = *shader;
                view.interface.resources = {{"views", {{*view_resource, 0}, {}, EGraphAccess::WRITE}}};
                view.interface.parameters.setInlineConstant(detail::VisibilityViewConstants{});
                view.dispatch = DispatchArguments{{1, 1, 1}};
                view.allow_async_compute = true;
                const auto view_pass = context.addComputePass(key + "view", std::move(view));
                if (!view_pass) return cxx::unexpected(view_pass.error());
                views = view_pass->written.front();
                inline_blocks.push_back(view_pass->pass);
            }

            const auto groups = context.declarePreparedBuffer("groups", sizeof(detail::MeshDrawGroupRecord), max_bytes);
            if (!groups) return cxx::unexpected(groups.error());
            const auto candidates = context.declarePreparedBuffer("candidates", sizeof(std::uint32_t), max_bytes);
            if (!candidates) return cxx::unexpected(candidates.error());
            const auto chunks = context.declarePreparedBuffer("chunks", sizeof(std::uint32_t) * 2, max_bytes);
            if (!chunks) return cxx::unexpected(chunks.error());
            constexpr auto dispatch_bytes = 2 * sizeof(nri::DispatchDesc);
            const auto dispatch = context.declarePreparedBuffer(
                "dispatch", dispatch_bytes, dispatch_bytes, EGraphUse::INDIRECT
            );
            if (!dispatch) return cxx::unexpected(dispatch.error());
            const auto visible = context.declareFrameBuffer("visible", 256, max_bytes);
            if (!visible) return cxx::unexpected(visible.error());
            constexpr auto command_bytes = sizeof(nri::DrawIndexedDesc) * kMeshWindingCount;
            const auto commands = context.declareFrameBuffer("commands", command_bytes, max_bytes);
            if (!commands) return cxx::unexpected(commands.error());
            const auto passes = detail::addMeshVisibilityPasses(
                context, key, {
                    *transforms, *alive, *properties, views,
                    *groups, *candidates, *chunks, *visible, *commands, *dispatch
                }, {frustum_culling, ShadowPass}
            );
            if (!passes) return cxx::unexpected(passes.error());
            typename Kernel::Configuration config{
                {device.compute_dispatch_limit[0], device.compute_dispatch_limit[1]}, {}, shadow
            };
            std::vector<GraphPassId> draw_streams;
            if (surface && context.outputDemanded(surface->output))
            {
                RasterPassTemplate stage;
                stage.purpose = "indexed-mesh";
                stage.inputs = detail::meshRasterInputs(device.record_table);
                stage.programs = RasterProgramSource{
                    RasterProgramBuilder::make<Kernel, &Kernel::buildPrograms>(), {*instances}
                };
                stage.section_depth_bias = ShadowPass;
                const auto record_table = device.record_table ?
                    static_cast<std::uint32_t>(EStorageCapability::STORAGE_BUFFER_ARRAY) : 0u;
                stage.draw_resources = {
                    {
                        .binding = std::string{GBuffer::geometry_binding}, .source = MeshResource::drawResourceType(),
                        .indexed_geometry = true
                    },
                    {std::string{GBuffer::materials_binding}, MaterialResource::drawResourceType(), record_table}
                };
                if (device.texture_descriptors)
                {
                    stage.draw_resources.push_back({
                        std::string{GBuffer::textures_binding}, TextureResource::drawResourceType(),
                        static_cast<std::uint32_t>(EStorageCapability::SAMPLED_IMAGE_ARRAY), true
                    });
                }
                const auto add_read = [&](ResourceVersion version, EGraphUse use)
                {
                    stage.resources.push_back({.version = version, .usage = use});
                };
                add_read(*transforms, EGraphUse::STORAGE_READ);
                add_read(*properties, EGraphUse::STORAGE_READ);
                if (!paged) add_read(*alive, EGraphUse::STORAGE_READ);
                add_read(passes->visible, EGraphUse::STORAGE_READ);
                add_read(passes->commands, EGraphUse::INDIRECT);
                RasterWork raster;
                raster.program = std::move(stage);
                auto drawn = surface->append(context, {key, shadow_uploads}, std::move(raster));
                if (!drawn) return cxx::unexpected(drawn.error());
                config.surface = detail::MeshSurfaceStream::Configuration{
                    {transforms->resource, properties->resource, alive->resource, visible->resource, commands->resource},
                    paged, device.indirect_first_instance, counter_clockwise, device.texture_descriptors, surface->purpose
                };
                draw_streams.push_back(*drawn);
            }
            std::vector<GraphResourceId> prepared_buffers{
                groups->resource, candidates->resource, chunks->resource, dispatch->resource
            };
            if constexpr (ShadowPass)
                prepared_buffers.insert(prepared_buffers.end(), shadow_uploads.begin(), shadow_uploads.end());
            if (auto prepared = context.addCpuCall<Kernel, &Kernel::createWorkspace>(
                std::move(config), {
                    .inline_blocks = std::move(inline_blocks),
                    .uploads = std::move(prepared_buffers), .frame_sizes = {visible->resource, commands->resource},
                    .storage_inputs = std::move(storage_inputs), .draw_streams = std::move(draw_streams)
                }
            ); !prepared)
                return prepared;
            if (context.outputDemanded(MeshVisibility::draws))
                return context.publish(MeshVisibility::draws, {
                    {MeshVisibility::groups, *groups},
                    {MeshVisibility::visible, passes->visible},
                    {MeshVisibility::commands, passes->commands}
                });
            return {};
        }
    }

    RenderResult<void>
    detail::appendCameraVisibility(EffectCompileContext& context, const MeshSurfaceOutput* surface, bool ccw) noexcept
    {
        return appendVisibility<false>(context, surface, ccw);
    }

    RenderResult<void>
    detail::appendShadowVisibility(EffectCompileContext& context, const MeshSurfaceOutput& surface, bool ccw) noexcept
    {
        return appendVisibility<true>(context, &surface, ccw);
    }

    EffectNodeDefinition MeshVisibility::definition() noexcept
    {
        EffectNodeDefinition definition;
        definition.identity = {graph::nodeTypeId(capability), std::string{capability}, version};
        definition.display_name = "Mesh Visibility";
        definition.ports = {{draws, "draws", {std::string{draws_type}, 1}, graph::EPinDirection::OUTPUT}};
        return definition;
    }

    EffectImplementation detail::meshVisibilityImplementation() noexcept
    {
        EffectImplementation implementation;
        implementation.capability = graph::nodeTypeId(MeshVisibility::capability);
        implementation.identity = {std::string{MeshVisibility::camera}, MeshVisibility::version};
        implementation.supported_outputs = {MeshVisibility::draws};
        implementation.initialize = initializeVisibility;
        implementation.build = [](EffectCompileContext& context, std::string_view) noexcept
        {
            return appendCameraVisibility(context, nullptr, true);
        };
        return implementation;
    }
}
