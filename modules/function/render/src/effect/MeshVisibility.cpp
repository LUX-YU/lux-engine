#include "MeshView.hpp"
#include <lux/engine/render/effect/EffectArguments.hpp>
#include <lux/engine/render/effect/EffectBundles.hpp>
#include <lux/engine/render/effect/BuiltinEffectNodes.hpp>
#include <lux/engine/render/effect/DepthPyramid.hpp>
#include <lux/engine/render/effect/GBuffer.hpp>
#include <lux/engine/render/effect/Lighting.hpp>
#include <lux/engine/render/effect/ToneMapping.hpp>
#include <lux/engine/render/effect/MeshRaster.hpp>
#include <lux/engine/render/effect/MeshSurfaceStream.hpp>
#include <lux/engine/render/effect/MeshVisibility.hpp>
#include <lux/engine/render/effect/Shadow.hpp>
#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/render/extensions/effect/EffectImplementation.hpp>
#include <lux/engine/render/extensions/effect/EffectInitializationContext.hpp>
#include <lux/engine/render/features/mesh/MeshGrouping.hpp>
#include <lux/engine/render/features/mesh/RasterMesh.hpp>
#include <lux/engine/render/features/mesh/MeshVisibilityInputs.hpp>
#include <lux/engine/render/features/mesh/MeshVisibilityWork.hpp>
#include <lux/engine/render/features/shadow/ShadowProjection.hpp>
#include <lux/engine/render/resources/instance/Binding.hpp>
#include <lux/engine/render/resources/instance/Resource.hpp>
#include <lux/engine/render/resources/material/Resource.hpp>
#include <lux/engine/render/resources/mesh/Resource.hpp>
#include <lux/engine/render/resources/texture/Resource.hpp>
#include <lux/engine/render/resources/lighting/Resource.hpp>
#include <lux/engine/render/resources/view/Resource.hpp>
#include <lux/engine/render/resources/transform/Binding.hpp>
#include <lux/engine/render/resources/transform/Resource.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <NRI.h>
#include <algorithm>
#include <cmath>

namespace lux::render
{
    namespace
    {
        enum class EMeshEffect { VISIBILITY, GBUFFER, FORWARD, DEPTH_ONLY, SHADOW };

        class PrepareMeshVisibility final
        {
        public:
            struct ShadowConfiguration final
            {
                GraphStorageSnapshotId lights;
                std::array<GraphResourceId, 3> uploads;
                detail::ShadowProjectionSettings settings;
            };

            struct Configuration final
            {
                GraphStorageSnapshotId instances;
                GraphStorageSnapshotId camera;
                GraphPassId view;
                std::array<GraphResourceId, 4> uploads;
                GraphResourceId visible, commands;
                std::array<std::uint32_t, 2> dispatch_limit;
                std::optional<detail::MeshSurfaceStream::Configuration> surface;
                std::optional<ShadowConfiguration> shadow;
            };

            struct ShadowInputs final
            {
                StorageSnapshotSlot<ShadowLightSnapshot> lights;
                std::array<PreparedBufferWriter, 3> uploads;
                detail::ShadowProjectionSettings settings;
            };

            struct LinkedData final
            {
                StorageSnapshotSlot<MeshInstanceSnapshot> instances;
                StorageSnapshotSlot<ViewGpuParameters> camera;
                std::optional<InlineConstantWriter<detail::VisibilityViewConstants>> view;
                std::array<PreparedBufferWriter, 4> uploads;
                FrameBufferSizer visible, commands;
                std::array<std::uint32_t, 2> dispatch_limit;
                std::optional<detail::MeshSurfaceStream::Inputs> surface;
                std::optional<ShadowInputs> shadow;
            };

            [[nodiscard]] static RenderResult<LinkedData>
            resolve(const Configuration& config, InstanceLinkContext& context) noexcept
            {
                const auto instances = context.resolveStorageSnapshot<MeshInstanceSnapshot>(config.instances);
                if (!instances) return cxx::unexpected(instances.error());
                const auto camera = context.resolveStorageSnapshot<ViewGpuParameters>(config.camera);
                if (!camera) return cxx::unexpected(camera.error());
                std::optional<InlineConstantWriter<detail::VisibilityViewConstants>> view;
                std::optional<ShadowInputs> shadow;
                if (config.shadow)
                {
                    const auto& source = *config.shadow;
                    const auto lights = context.resolveStorageSnapshot<ShadowLightSnapshot>(source.lights);
                    if (!lights) return cxx::unexpected(lights.error());
                    const auto views = context.resolvePreparedBuffer(source.uploads[0]);
                    if (!views) return cxx::unexpected(views.error());
                    const auto slices = context.resolvePreparedBuffer(source.uploads[1]);
                    if (!slices) return cxx::unexpected(slices.error());
                    const auto ranges = context.resolvePreparedBuffer(source.uploads[2]);
                    if (!ranges) return cxx::unexpected(ranges.error());
                    shadow = ShadowInputs{*lights, {*views, *slices, *ranges}, source.settings};
                }
                else
                {
                    const auto writer = context.resolveInlineConstants<detail::VisibilityViewConstants>(config.view);
                    if (!writer) return cxx::unexpected(writer.error());
                    view = *writer;
                }
                const auto groups = context.resolvePreparedBuffer(config.uploads[0]);
                if (!groups) return cxx::unexpected(groups.error());
                const auto candidates = context.resolvePreparedBuffer(config.uploads[1]);
                if (!candidates) return cxx::unexpected(candidates.error());
                const auto chunks = context.resolvePreparedBuffer(config.uploads[2]);
                if (!chunks) return cxx::unexpected(chunks.error());
                const auto dispatch = context.resolvePreparedBuffer(config.uploads[3]);
                if (!dispatch) return cxx::unexpected(dispatch.error());
                const auto visible = context.resolveFrameBuffer(config.visible);
                if (!visible) return cxx::unexpected(visible.error());
                const auto commands = context.resolveFrameBuffer(config.commands);
                if (!commands) return cxx::unexpected(commands.error());
                std::optional<detail::MeshSurfaceStream::Inputs> surface;
                if (config.surface)
                {
                    auto resolved = detail::MeshSurfaceStream::resolve(*config.surface, context);
                    if (!resolved) return cxx::unexpected(resolved.error());
                    surface = *resolved;
                }
                return LinkedData{
                    *instances, *camera, view, {*groups, *candidates, *chunks, *dispatch},
                    *visible, *commands, config.dispatch_limit, surface, shadow
                };
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
                if (inputs.shadow) workspace->shadow.emplace(inputs.shadow->settings);
                return workspace;
            }

            using Surface = detail::MeshSurfaceStream;
            using DrawMapping = Surface::DrawMapping;

            static RenderResult<std::unique_ptr<DrawMapping>>
            buildPrograms(
                const LinkedData& linked,
                RasterProgramBuildContext& selection,
                const DrawMapping* previous
            ) noexcept
            {
                if (!linked.surface) return cxx::unexpected(RenderError{kFrameContract});
                const auto instances = selection.readStorage(linked.instances);
                if (!instances) return cxx::unexpected(instances.error());
                return Surface::buildPrograms(*linked.surface, selection, instances->value.get(), previous);
            }

            static RenderResult<void>
            prepare(
                const LinkedData& linked,
                FramePreparation& frame,
                Workspace& workspace,
                const DrawMapping* mapping
            ) noexcept
            {
                const auto selected_view = frame.readStorage(linked.camera);
                if (!selected_view) return cxx::unexpected(selected_view.error());
                const auto& view = selected_view->value.get();
                if (workspace.shadow)
                {
                    const auto lights = frame.readStorage(linked.shadow->lights);
                    if (!lights) return cxx::unexpected(lights.error());
                    const detail::ShadowCamera camera{
                        view.inverse_view_projection, view.position_near, view.direction_far
                    };
                    const auto projected = workspace.shadow->select(camera, lights->value.get());
                    if (!projected) return cxx::unexpected(projected.error());
                    workspace.grouping.setViewCount(static_cast<std::uint32_t>(workspace.shadow->views().size()));
                    const std::array payloads{
                        std::as_bytes(workspace.shadow->views()),
                        std::as_bytes(workspace.shadow->slices()),
                        workspace.shadow->mappings()
                    };
                    for (std::size_t i = 0; i < payloads.size(); ++i)
                    {
                        const auto written = frame.writeBuffer(
                            linked.shadow->uploads[i], payloads[i], workspace.shadow->revision()
                        );
                        if (!written) return written;
                    }
                }
                else
                {
                    auto constants = frame.write(*linked.view, {view.view, view.projection});
                    if (!constants) return constants;
                }
                const auto selected = frame.readStorage(linked.instances);
                if (!selected) return cxx::unexpected(selected.error());
                const auto& snapshot = selected->value.get();
                if (workspace.selection != selected->revision)
                {
                    updateGroups(snapshot, workspace);
                    workspace.selection = selected->revision;
                }
                auto packed = workspace.grouping.pack(&snapshot, lookupGeometry, linked.dispatch_limit);
                if (!packed) return packed;
                const auto payloads = workspace.grouping.uploads();
                for (std::size_t i = 0; i < payloads.size(); ++i)
                {
                    auto written = frame.writeBuffer(linked.uploads[i], payloads[i], workspace.grouping.revision());
                    if (!written) return written;
                }
                auto sized = frame.sizeBuffer(linked.visible, workspace.grouping.visibleBytes());
                if (!sized) return sized;
                auto commands = frame.sizeBuffer(
                    linked.commands, workspace.grouping.indirectStreamCount() * sizeof(nri::DrawIndexedDesc)
                );
                if (!commands) return commands;
                if (!workspace.surface) return {};
                if (!mapping) return cxx::unexpected(RenderError{kFrameContract});
                const detail::MeshSurfaceStream::DrawInputs inputs{frame, workspace.grouping.source(), *mapping};
                if (workspace.shadow)
                    return workspace.surface->prepareShadow(
                        inputs, workspace.shadow->views(), workspace.shadow->rasterStates()
                    );
                return workspace.surface->prepare(inputs, view);
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

        RenderResult<void> initializeVisibility(EffectInitializationContext& context, std::string_view arguments) noexcept
        {
            const auto parsed = cxx::args::parse(arguments, kFrustumCulling);
            if (!parsed) return cxx::unexpected(detail::effectArgumentError(context.nodeId(), parsed.error()));
            return context.setConfiguration(MeshVisibilityConfiguration{*parsed.get<"frustum-culling">()});
        }

        struct ShadowNodeConfiguration final
        {
            detail::ShadowProjectionSettings projection{{1024, 1}, 100, 1, 0};
            bool frustum_culling = true;
        };

        RenderResult<void> initializeShadow(EffectInitializationContext& context, std::string_view arguments) noexcept
        {
            constexpr ShadowNodeConfiguration defaults;
            constexpr auto resolution = cxx::args::Option<std::uint32_t, "resolution">{}
                .def(defaults.projection.shape.resolution);
            constexpr auto distance = cxx::args::Option<float, "distance">{}.def(defaults.projection.distance).check(
                [](float value) noexcept { return std::isfinite(value) && value > 0; }
            );
            constexpr auto radius = cxx::args::Option<std::uint32_t, "pcf-radius">{}
                .def(defaults.projection.pcf_radius).check(cxx::args::inRange(0u, 4u));
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

        template <EMeshEffect Effect, bool CounterClockwise = true>
        RenderResult<void> buildVisibility(EffectCompileContext& context, std::string_view) noexcept
        {
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
            std::optional<PrepareMeshVisibility::ShadowConfiguration> shadow;
            bool frustum_culling;
            if constexpr (Effect == EMeshEffect::SHADOW)
            {
                const auto* configuration = context.configuration<ShadowNodeConfiguration>();
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
                shadow = PrepareMeshVisibility::ShadowConfiguration{
                    *lights, {views.resource, slices->resource, ranges->resource}, settings
                };
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
                }, {frustum_culling, Effect == EMeshEffect::SHADOW}
            );
            if (!passes) return cxx::unexpected(passes.error());
            auto config = PrepareMeshVisibility::Configuration{
                *instances, *camera, inline_blocks.empty() ? GraphPassId{} : inline_blocks.front(),
                {groups->resource, candidates->resource, chunks->resource, dispatch->resource},
                visible->resource, commands->resource,
                {device.compute_dispatch_limit[0], device.compute_dispatch_limit[1]}, {}, shadow
            };
            EffectResourceBundle bundle;
            std::vector<GraphPassId> producer_passes;
            std::vector<GraphStorageSnapshotId> program_inputs;
            constexpr auto output = Effect == EMeshEffect::VISIBILITY ? MeshVisibility::draws :
                Effect == EMeshEffect::GBUFFER ? GBuffer::surface :
                Effect == EMeshEffect::FORWARD ? MeshRaster::color :
                Effect == EMeshEffect::DEPTH_ONLY ? MeshRaster::depth : Shadow::shadows;
            const bool needs_raster = Effect != EMeshEffect::VISIBILITY && context.outputDemanded(output);
            if (needs_raster)
            {
                const auto& target = context.target();
                if ((Effect == EMeshEffect::GBUFFER || Effect == EMeshEffect::FORWARD) && target.samples != 1)
                    return cxx::unexpected(RenderError{kDeviceUnsupported, {context.nodeId().value}});
                if constexpr (Effect == EMeshEffect::FORWARD)
                {
                    const bool supported = target.format == rdesc::ETextureFormat::RGBA8_UNORM ||
                        target.format == rdesc::ETextureFormat::RGBA8_SRGB;
                    if (!supported) return cxx::unexpected(RenderError{kDeviceUnsupported, {context.nodeId().value}});
                }
                RasterPassTemplate stage;
                stage.purpose = "indexed-mesh";
                stage.inputs = detail::meshRasterInputs(device.record_table);
                stage.runtime_stream_programs = true;
                stage.section_depth_bias = Effect == EMeshEffect::SHADOW;
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
                if constexpr (Effect == EMeshEffect::SHADOW)
                {
                    const auto depth = context.declareResource({key + "shadow-depth", shadow->settings.shape.image()});
                    if (!depth) return cxx::unexpected(depth.error());
                    raster.depth = RasterAttachment{{{*depth, 0}}};
                    for (std::size_t i = 0; i < shadow->uploads.size(); ++i)
                        bundle.push_back({effectPortSemantic(ShadowBundle::members[i]), {shadow->uploads[i], 0}});
                }
                else if constexpr (Effect == EMeshEffect::GBUFFER)
                {
                    for (std::size_t i = 0; i < GBufferBundle::members.size(); ++i)
                    {
                        const bool is_depth = i == rdesc::GBufferLayout::color_formats.size();
                        const auto format = GBufferBundle::formats[i];
                        GraphImageDesc image{0, 0, 1, 1, format};
                        image.extent.source = EImageExtentSource::TARGET;
                        const auto attachment = context.declareResource({key + std::string{GBufferBundle::members[i]}, image});
                        if (!attachment) return cxx::unexpected(attachment.error());
                        if (is_depth) raster.depth = RasterAttachment{{{*attachment, 0}}};
                        else raster.colors.push_back({{{*attachment, 0}}});
                    }
                }
                else
                {
                    GraphImageDesc depth_image{0, 0, 1, 1, rdesc::ETextureFormat::D32_SFLOAT};
                    depth_image.extent.source = EImageExtentSource::TARGET;
                    const auto depth = context.declareResource({key + "depth", depth_image});
                    if (!depth) return cxx::unexpected(depth.error());
                    raster.depth = RasterAttachment{{{*depth, 0}}};
                    if constexpr (Effect == EMeshEffect::FORWARD)
                    {
                        GraphImageDesc color_image{0, 0, 1, 1, target.format};
                        color_image.extent.source = EImageExtentSource::TARGET;
                        const auto color = context.declareResource({key + "color", color_image});
                        if (!color) return cxx::unexpected(color.error());
                        raster.colors.push_back({{{*color, 0}}});
                    }
                }
                const auto drawn = context.addRasterPass(key + "raster", std::move(raster));
                if (!drawn) return cxx::unexpected(drawn.error());
                if constexpr (Effect == EMeshEffect::SHADOW)
                    bundle.push_back({effectPortSemantic(ShadowBundle::members.back()), drawn->written.front()});
                else if constexpr (Effect == EMeshEffect::GBUFFER)
                    for (std::size_t i = 0; i < GBufferBundle::members.size(); ++i)
                        bundle.push_back({effectPortSemantic(GBufferBundle::members[i]), drawn->written[i]});
                else
                    bundle.push_back({
                        Effect == EMeshEffect::FORWARD ? MeshRaster::color : MeshRaster::depth, drawn->written.front()
                    });
                constexpr auto purpose = Effect == EMeshEffect::SHADOW || Effect == EMeshEffect::DEPTH_ONLY
                    ? rdesc::EMaterialPurpose::SHADOW
                    : Effect == EMeshEffect::FORWARD ? rdesc::EMaterialPurpose::FORWARD : rdesc::EMaterialPurpose::GBUFFER;
                config.surface = detail::MeshSurfaceStream::Configuration{
                    drawn->pass,
                    {transforms->resource, properties->resource, alive->resource, visible->resource, commands->resource},
                    paged, device.indirect_first_instance, CounterClockwise, device.texture_descriptors, purpose
                };
                producer_passes.push_back(drawn->pass);
                program_inputs.push_back(*instances);
            }
            std::vector<GraphResourceId> prepared_buffers{
                groups->resource, candidates->resource, chunks->resource, dispatch->resource,
                visible->resource, commands->resource
            };
            std::vector<GraphStorageSnapshotId> storage_inputs{*instances, *camera};
            if (shadow)
            {
                prepared_buffers.insert(prepared_buffers.end(), shadow->uploads.begin(), shadow->uploads.end());
                storage_inputs.push_back(shadow->lights);
            }
            using Kernel = PrepareMeshVisibility;
            constexpr auto build = RasterProgramBuilder::make<Kernel, &Kernel::buildPrograms>();
            const auto program_builder = config.surface ? build : RasterProgramBuilder{};
            auto value = ConfigValue::make(config);
            if (!value) return cxx::unexpected(value.error());
            if (auto prepared = context.addPreparation({
                PreparationAlgorithm::withWorkspace<PrepareMeshVisibility, &PrepareMeshVisibility::createWorkspace>(),
                std::move(*value), {}, {}, std::move(inline_blocks),
                std::move(producer_passes), std::move(prepared_buffers), std::move(storage_inputs),
                std::move(program_inputs), program_builder
            }); !prepared)
                return prepared;
            if constexpr (Effect == EMeshEffect::VISIBILITY || Effect == EMeshEffect::GBUFFER || Effect == EMeshEffect::SHADOW)
            {
                if (context.outputDemanded(MeshVisibility::draws))
                {
                    auto published = context.publish(MeshVisibility::draws, {
                        {MeshVisibility::groups, *groups},
                        {MeshVisibility::visible, passes->visible},
                        {MeshVisibility::commands, passes->commands}
                    });
                    if (!published) return published;
                }
            }
            if (needs_raster) return context.publish(output, std::move(bundle));
            return {};
        }
    }

    CapabilityDefinition MeshVisibility::definition() noexcept
    {
        CapabilityDefinition definition;
        definition.identity = {graph::nodeTypeId(capability), std::string{capability}, version};
        definition.display_name = "Mesh Visibility";
        definition.ports = {{draws, "draws", {std::string{draws_type}, 1}, graph::EPinDirection::OUTPUT}};
        return definition;
    }

    RenderResult<void> detail::registerMeshVisibility(RenderServer& server) noexcept
    {
        auto definition = MeshVisibility::definition();
        const auto capability = definition.identity.id;
        if (auto registered = server.registerEffectCapability(std::move(definition)); !registered) return registered;
        EffectImplementation implementation;
        implementation.capability = capability;
        implementation.identity = {std::string{MeshVisibility::camera}, MeshVisibility::version};
        implementation.supported_outputs = {MeshVisibility::draws};
        implementation.initialize = initializeVisibility;
        implementation.build = buildVisibility<EMeshEffect::VISIBILITY>;
        return server.registerEffectImplementation(std::move(implementation));
    }

    CapabilityDefinition GBuffer::definition() noexcept
    {
        CapabilityDefinition definition;
        definition.identity = {graph::nodeTypeId(capability), std::string{capability}, version};
        definition.display_name = "GBuffer";
        definition.ports = {
            {surface, "surface", {std::string{GBufferBundle::type}, GBufferBundle::version},
                graph::EPinDirection::OUTPUT, UINT8_MAX},
            {draws, "draws", {std::string{MeshVisibility::draws_type}, 1},
                graph::EPinDirection::OUTPUT, UINT8_MAX}
        };
        return definition;
    }

    RenderResult<void> detail::registerGBuffer(RenderServer& server) noexcept
    {
        auto definition = GBuffer::definition();
        const auto capability = definition.identity.id;
        if (auto registered = server.registerEffectCapability(std::move(definition)); !registered) return registered;
        EffectImplementation implementation;
        implementation.capability = capability;
        implementation.supported_outputs = {GBuffer::surface, GBuffer::draws};
        implementation.initialize = initializeVisibility;
        implementation.identity = {std::string{GBuffer::indexed}, GBuffer::version};
        implementation.build = buildVisibility<EMeshEffect::GBUFFER>;
        if (auto registered = server.registerEffectImplementation(implementation); !registered) return registered;
        implementation.identity = {std::string{GBuffer::indexed_clockwise}, GBuffer::version};
        implementation.build = buildVisibility<EMeshEffect::GBUFFER, false>;
        return server.registerEffectImplementation(std::move(implementation));
    }

    CapabilityDefinition MeshRaster::definition() noexcept
    {
        CapabilityDefinition definition;
        definition.identity = {graph::nodeTypeId(capability), std::string{capability}, version};
        definition.display_name = "Mesh Raster";
        definition.ports = {
            {color, "color", {std::string{ToneMapping::display_type}, 1}, graph::EPinDirection::OUTPUT, UINT8_MAX},
            {depth, "depth", {std::string{DepthPyramid::depth_type}, 1}, graph::EPinDirection::OUTPUT, UINT8_MAX}
        };
        return definition;
    }

    RenderResult<void> detail::registerMeshRaster(RenderServer& server) noexcept
    {
        auto definition = MeshRaster::definition();
        const auto capability = definition.identity.id;
        if (auto registered = server.registerEffectCapability(std::move(definition)); !registered) return registered;
        struct Variant final
        {
            std::string_view name;
            graph::PinSemanticId output;
            EffectImplementation::Build build;
        };
        constexpr std::array variants{
            Variant{MeshRaster::forward, MeshRaster::color, buildVisibility<EMeshEffect::FORWARD>},
            Variant{MeshRaster::forward_clockwise, MeshRaster::color, buildVisibility<EMeshEffect::FORWARD, false>},
            Variant{MeshRaster::depth_only, MeshRaster::depth, buildVisibility<EMeshEffect::DEPTH_ONLY>},
            Variant{MeshRaster::depth_only_clockwise, MeshRaster::depth, buildVisibility<EMeshEffect::DEPTH_ONLY, false>}
        };
        for (const auto& variant : variants)
        {
            EffectImplementation implementation;
            implementation.capability = capability;
            implementation.identity = {std::string{variant.name}, MeshRaster::version};
            implementation.supported_outputs = {variant.output};
            implementation.initialize = initializeVisibility;
            implementation.build = variant.build;
            if (auto result = server.registerEffectImplementation(std::move(implementation)); !result) return result;
        }
        return {};
    }

    CapabilityDefinition Shadow::definition() noexcept
    {
        CapabilityDefinition definition;
        definition.identity = {graph::nodeTypeId(capability), std::string{capability}, version};
        definition.display_name = "Shadow";
        definition.ports = {
            {shadows, "shadows", {std::string{ShadowBundle::type}, ShadowBundle::version},
                graph::EPinDirection::OUTPUT, UINT8_MAX},
            {draws, "draws", {std::string{MeshVisibility::draws_type}, 1}, graph::EPinDirection::OUTPUT, UINT8_MAX}
        };
        return definition;
    }

    RenderResult<void> detail::registerShadow(RenderServer& server) noexcept
    {
        auto definition = Shadow::definition();
        const auto capability = definition.identity.id;
        if (auto registered = server.registerEffectCapability(std::move(definition)); !registered) return registered;
        EffectImplementation implementation;
        implementation.capability = capability;
        implementation.supported_outputs = {Shadow::shadows, Shadow::draws};
        implementation.initialize = initializeShadow;
        implementation.identity = {std::string{Shadow::indexed}, Shadow::version};
        implementation.build = buildVisibility<EMeshEffect::SHADOW>;
        if (auto registered = server.registerEffectImplementation(implementation); !registered) return registered;
        implementation.identity = {std::string{Shadow::indexed_clockwise}, Shadow::version};
        implementation.build = buildVisibility<EMeshEffect::SHADOW, false>;
        return server.registerEffectImplementation(std::move(implementation));
    }
} // namespace lux::render
