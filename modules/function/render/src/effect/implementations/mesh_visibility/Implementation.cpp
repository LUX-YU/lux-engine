#include <lux/engine/render/resource/instance/Requirement.hpp>
#include <lux/engine/render/frame/FrameError.hpp>
#include "MeshView.hpp"
#include <lux/engine/render/effect/implementations/common/EffectArguments.hpp>
#include <lux/engine/render/assembly/BuiltinEffects.hpp>
#include <lux/engine/render/effect/capabilities/DeferredGeometry.hpp>
#include <lux/engine/render/effect/implementations/mesh_common/SurfaceDraws.hpp>
#include <lux/engine/render/effect/capabilities/MeshVisibility.hpp>
#include <lux/engine/render/effect/capabilities/ShadowRendering.hpp>
#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/render/effect/EffectImplementation.hpp>
#include <lux/engine/render/effect/implementations/mesh_common/DrawBuckets.hpp>
#include <lux/engine/render/effect/implementations/mesh_common/IndexedDraws.hpp>
#include <lux/engine/render/effect/implementations/mesh_visibility/VisibilityInputs.hpp>
#include <lux/engine/render/effect/implementations/mesh_visibility/MeshCulling.hpp>
#include <lux/engine/render/effect/implementations/shadow/ShadowProjection.hpp>
#include <lux/engine/render/resource/instance/Binding.hpp>
#include <lux/engine/render/resource/instance/Resource.hpp>
#include <lux/engine/render/resource/material/Resource.hpp>
#include <lux/engine/render/resource/mesh/Resource.hpp>
#include <lux/engine/render/resource/texture/Resource.hpp>
#include <lux/engine/render/resource/lighting/Resource.hpp>
#include <lux/engine/render/resource/view/Resource.hpp>
#include <lux/engine/render/resource/transform/Binding.hpp>
#include <lux/engine/render/resource/transform/Resource.hpp>
#include <lux/engine/render/device/Error.hpp>
#include <NRI.h>
#include <algorithm>
#include <cmath>

namespace lux::render
{
    namespace
    {
        struct PrepareCameraSurface final
        {
            using Workspace = detail::MeshSurfaceStream;
            using Configuration = Workspace::Configuration;
            using LinkedData = Workspace::Inputs;
            using FrameInputs = EffectFrameInputs<MeshDrawTopology, ViewGpuParameters>;

            static RenderResult<LinkedData>
            resolve(const Configuration& config, std::span<const RasterOperands, 1> streams) noexcept
            {
                return Workspace::resolve(config, streams.front());
            }

            static RenderResult<std::unique_ptr<Workspace>>
            createWorkspace(const LinkedData& linked, const TargetDesc&, const InstalledDrawSources& sources) noexcept
            {
                auto surface = Workspace::bind(linked, sources);
                if (!surface) return cxx::unexpected(surface.error());
                return std::make_unique<Workspace>(std::move(*surface));
            }

            static RenderResult<void>
            onFrame(const LinkedData&, const FrameInputs& inputs, EffectFrameWriter&, Workspace& surface) noexcept
            {
                const auto& topology = std::get<0>(inputs.values).value.get();
                const auto& viewport = std::get<1>(inputs.values).value.get().viewport;
                const RasterState state{{viewport[0], viewport[1], viewport[2], viewport[3]}};
                return surface.prepare(topology, topology.draw_groups, std::span{&state, 1});
            }
        };

        struct PrepareShadowVisibility final
        {
            struct Configuration final
            {
                std::array<std::uint32_t, 2> dispatch_limit;
                detail::ShadowProjectionSettings projection;
                std::optional<detail::MeshSurfaceStream::Configuration> surface;
            };

            struct LinkedData final
            {
                std::array<std::uint32_t, 2> dispatch_limit;
                detail::ShadowProjectionSettings projection;
                std::optional<detail::MeshSurfaceStream::Inputs> surface;
            };

            static RenderResult<LinkedData>
            resolve(const Configuration& config, std::span<const RasterOperands> streams) noexcept
            {
                if (streams.size() != static_cast<std::size_t>(config.surface.has_value()))
                    return cxx::unexpected(RenderError{kGraphInvalidDescriptor});
                LinkedData linked{config.dispatch_limit, config.projection, {}};
                if (config.surface)
                {
                    auto surface = detail::MeshSurfaceStream::resolve(*config.surface, streams.front());
                    if (!surface) return cxx::unexpected(surface.error());
                    linked.surface = *surface;
                }
                return linked;
            }

            struct Workspace final
            {
                detail::ShadowProjectionCache shadow;
                detail::MeshVisibilityLayout visibility;
                std::optional<detail::MeshSurfaceStream> surface;
            };

            static RenderResult<std::unique_ptr<Workspace>>
            createWorkspace(const LinkedData& inputs, const TargetDesc&, const InstalledDrawSources& sources) noexcept
            {
                auto workspace = std::make_unique<Workspace>(detail::ShadowProjectionCache{inputs.projection});
                if (inputs.surface)
                {
                    auto surface = detail::MeshSurfaceStream::bind(*inputs.surface, sources);
                    if (!surface) return cxx::unexpected(surface.error());
                    workspace->surface.emplace(std::move(*surface));
                }
                return workspace;
            }

            using FrameInputs = EffectFrameInputs<MeshDrawTopology, ViewGpuParameters, ShadowLightSnapshot>;

            static RenderResult<void>
            onFrame(const LinkedData& linked, const FrameInputs& inputs, EffectFrameWriter& writer, Workspace& workspace) noexcept
            {
                const auto& [instances, view_input, lights] = inputs.values;
                const auto& topology = instances.value.get();
                auto& shadow = workspace.shadow;
                if (auto projected = shadow.select(view_input.value.get(), lights); !projected)
                    return cxx::unexpected(projected.error());
                auto& visibility = workspace.visibility;
                const auto layers = static_cast<std::uint32_t>(shadow.rasterStates().size());
                const auto groups = visibility.select(topology, layers, linked.dispatch_limit);
                if (!groups) return cxx::unexpected(groups.error());

                const std::array uploads{
                    FrameBufferUpload{std::as_bytes(*groups), visibility.revision()},
                    FrameBufferUpload{std::as_bytes(std::span{visibility.dispatch()}), visibility.dispatchRevision()},
                    shadow.viewUpload(), shadow.sliceUpload(), shadow.mappingUpload()
                };
                const auto streams = std::uint64_t{groups->size()} * layers * kMeshWindingCount;
                const std::array<std::uint64_t, 2> extents{
                    visibility.visibleBytes(), streams * sizeof(nri::DrawIndexedDesc)
                };
                if (auto published = writer.publishBuffers(uploads, extents); !published) return published;
                if (workspace.surface) return workspace.surface->prepare(topology, *groups, shadow.rasterStates());
                return {};
            }
        };

        constexpr auto kFrustumCulling = cxx::args::Option<bool, "frustum-culling">{}.def(true);
    }

    RenderResult<bool> detail::parseFrustumCulling(graph::NodeId node, std::string_view arguments) noexcept
    {
        const auto parsed = cxx::args::parse(arguments, kFrustumCulling);
        if (!parsed) return cxx::unexpected(detail::effectArgumentError(node, parsed.error()));
        return *parsed.get<"frustum-culling">();
    }

    namespace
    {
        template <bool ShadowPass, bool WithSurface>
        auto appendVisibility(
            EffectCompileContext& context, detail::MeshRasterAttachments* surface,
            bool frustum_culling, const detail::ShadowProjectionSettings* projection
        ) noexcept -> RenderResult<std::conditional_t<ShadowPass, detail::ShadowVisibilityPass, PassResult>>
        {
            const auto& device = context.device();
            const auto max_bytes = std::min(device.storage_buffer_max_range, UINT64_MAX / 2) & ~std::uint64_t{3};
            const auto instancesInput = [&](std::string_view binding, EMeshInstanceBinding role)
            {
                return context.importProjection(kSceneMeshInstances, binding, meshInstanceProjection(role),
                    static_cast<std::uint32_t>(role));
            };
            const bool paged = device.buffer_device_address;
            const auto transform_role = transformGraphProjection(paged);
            const auto transforms = context.importProjection(
                kSceneTransforms, kSceneTransforms.binding, TransformResource::projection(transform_role),
                static_cast<std::uint32_t>(transform_role)
            );
            if (!transforms) return cxx::unexpected(transforms.error());
            const auto properties_role = paged ? EMeshInstanceBinding::PAGED_ROOT : EMeshInstanceBinding::PROPERTIES;
            const auto properties = instancesInput("mesh.properties", properties_role);
            if (!properties) return cxx::unexpected(properties.error());
            constexpr auto alive_role = EMeshInstanceBinding::ALIVE;
            const auto alive = paged ? properties : instancesInput("mesh.alive", alive_role);
            if (!alive) return cxx::unexpected(alive.error());
            const std::string key = "mesh-visibility/" + std::to_string(context.nodeId().value) + "/";
            ResourceVersion views;
            std::vector<ResourceViewSlot> resource_views;
            if constexpr (ShadowPass || WithSurface)
            {
                const auto instances = context.readResourceView<MeshDrawTopology>(
                    kSceneMeshInstances, kSceneMeshInstances.binding
                );
                if (!instances) return cxx::unexpected(instances.error());
                resource_views.push_back(*instances);
                const auto camera = context.readResourceView<ViewGpuParameters>(
                    kViewParameters, kViewParameters.binding
                );
                if (!camera) return cxx::unexpected(camera.error());
                resource_views.push_back(*camera);
            }
            std::array<GraphResourceId, 3> shadow_uploads{};
            std::optional<detail::ShadowProjectionSettings> shadow;
            PassResult raster_pass{};
            if constexpr (ShadowPass)
            {
                auto settings = *projection;
                settings.maximum_layers = device.texture_layer_limit;
                const auto lights = context.readResourceView<ShadowLightSnapshot>(
                    kSceneLights, "shadow.lights"
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
                resource_views.push_back(*lights);
                shadow_uploads = {views.resource, slices->resource, ranges->resource};
                shadow = settings;
            }
            else
            {
                const auto view_resource = context.declareResource({key + "views", GraphBufferDesc{sizeof(MeshVisibilityView)}});
                if (!view_resource) return cxx::unexpected(view_resource.error());
                const auto shader = context.addProgram(GraphProgram{kMeshView.load(context.shaderTarget())});
                if (!shader) return cxx::unexpected(shader.error());
                ComputeWork view;
                view.interface.program = *shader;
                const auto row = context.importProjection(
                    kViewParameters, ViewParameterResource::gpu_binding, ViewParameterResource::projection()
                );
                if (!row) return cxx::unexpected(row.error());
                view.interface.resources = {
                    {"views", {{*view_resource, 0}, {}, EGraphAccess::WRITE}}, {"scene_view", {*row}}
                };
                view.dispatch = DispatchArguments{{1, 1, 1}};
                view.allow_async_compute = true;
                const auto view_pass = context.addComputePass(key + "view", std::move(view));
                if (!view_pass) return cxx::unexpected(view_pass.error());
                views = view_pass->written.front();
            }

            const auto groups = ShadowPass ? context.declarePreparedBuffer("groups", sizeof(MeshDrawGroupRecord), max_bytes) :
                instancesInput("mesh.camera_groups", EMeshInstanceBinding::CAMERA_GROUPS);
            if (!groups) return cxx::unexpected(groups.error());
            constexpr auto candidates_role = EMeshInstanceBinding::CANDIDATES;
            constexpr auto chunks_role = EMeshInstanceBinding::CHUNKS;
            const auto candidates = instancesInput("mesh.candidates", candidates_role);
            if (!candidates) return cxx::unexpected(candidates.error());
            const auto chunks = instancesInput("mesh.chunks", chunks_role);
            if (!chunks) return cxx::unexpected(chunks.error());
            constexpr auto dispatch_bytes = 2 * sizeof(nri::DispatchDesc);
            const auto dispatch = ShadowPass ? context.declarePreparedBuffer(
                "dispatch", dispatch_bytes, dispatch_bytes, EGraphUse::INDIRECT
            ) : instancesInput("mesh.camera_dispatch", EMeshInstanceBinding::CAMERA_DISPATCH);
            if (!dispatch) return cxx::unexpected(dispatch.error());
            const auto workBuffer = [&](std::string_view name, std::uint64_t minimum, EMeshVisibilityExtent field)
            {
                std::optional<ResourceBufferExtent> extent;
                if constexpr (!ShadowPass)
                    extent = ResourceBufferExtent{kSceneMeshInstances.binding, static_cast<std::uint32_t>(field)};
                return context.declareFrameBuffer(name, minimum, max_bytes, extent);
            };
            const auto visible = workBuffer("visible", 256, EMeshVisibilityExtent::VISIBLE_MEMBERS);
            if (!visible) return cxx::unexpected(visible.error());
            constexpr auto command_bytes = sizeof(nri::DrawIndexedDesc) * kMeshWindingCount;
            const auto commands = workBuffer("commands", command_bytes, EMeshVisibilityExtent::INDEXED_COMMANDS);
            if (!commands) return cxx::unexpected(commands.error());
            const auto passes = detail::addMeshVisibilityPasses(
                context, key, {
                    *transforms, *alive, *properties, views,
                    *groups, *candidates, *chunks, *visible, *commands, *dispatch
                }, {frustum_culling, ShadowPass}
            );
            if (!passes) return cxx::unexpected(passes.error());
            std::optional<detail::MeshSurfaceStream::Configuration> surface_config;
            std::vector<GraphPassId> draw_streams;
            if constexpr (WithSurface)
            {
                RasterPassTemplate stage;
                stage.purpose = "indexed-mesh";
                stage.inputs = detail::meshRasterInputs(device.record_table);
                stage.programs = RasterPassTemplate::ReadyPrograms{};
                stage.section_depth_bias = ShadowPass;
                DrawResourceReads material_reads;
                if (device.record_table) material_reads |= EDrawResourceRead::STORAGE_RECORDS;
                stage.draw_resources = {
                    {
                        .binding = std::string{"mesh.geometry"}, .source = ResourceName{MeshResource::descriptor.type.name()},
                        .reads = EDrawResourceRead::INDEXED_GEOMETRY
                    },
                    {std::string{"mesh.materials"}, ResourceName{MaterialResource::descriptor.type.name()}, material_reads}
                };
                if (device.texture_descriptors)
                {
                    stage.draw_resources.push_back({
                        std::string{"mesh.textures"}, ResourceName{TextureResource::descriptor.type.name()},
                        EDrawResourceRead::SAMPLED_IMAGES, true
                    });
                }
                const auto add_read = [&](ResourceVersion version, EGraphUse use)
                {
                    stage.resources.push_back({.version = version, .usage = use});
                };
                add_read(*transforms, EGraphUse::STORAGE_READ);
                add_read(*properties, EGraphUse::STORAGE_READ);
                if (!paged) add_read(*alive, EGraphUse::STORAGE_READ);
                add_read(views, EGraphUse::STORAGE_READ);
                add_read(passes->visible, EGraphUse::STORAGE_READ);
                add_read(passes->commands, EGraphUse::INDIRECT);
                surface->work.program = std::move(stage);
                auto drawn = context.addRasterPass(key + "raster", std::move(surface->work));
                if (!drawn) return cxx::unexpected(drawn.error());
                surface_config = detail::MeshSurfaceStream::Configuration{
                    {transforms->resource, properties->resource, alive->resource, views.resource, visible->resource, commands->resource},
                    device.indirect_first_instance, surface->counter_clockwise, device.texture_descriptors, surface->purpose
                };
                draw_streams.push_back(drawn->pass);
                raster_pass = std::move(*drawn);
            }
            if constexpr (ShadowPass || WithSurface)
            {
                FrameAlgorithmUses uses{.resource_views = std::move(resource_views), .draw_streams = std::move(draw_streams)};
                if constexpr (ShadowPass)
                {
                    uses.frame_sizes = {visible->resource, commands->resource};
                    uses.uploads = {groups->resource, dispatch->resource, shadow_uploads[0], shadow_uploads[1], shadow_uploads[2]};
                    PrepareShadowVisibility::Configuration config{
                        {device.compute_dispatch_limit[0], device.compute_dispatch_limit[1]}, *shadow, std::move(surface_config)
                    };
                    auto prepared = context.onFrame<&PrepareShadowVisibility::onFrame, &PrepareShadowVisibility::resolve, &PrepareShadowVisibility::createWorkspace>(
                        std::move(config), std::move(uses)
                    );
                    if (!prepared) return cxx::unexpected(prepared.error());
                }
                else
                {
                    auto prepared = context.onFrame<&PrepareCameraSurface::onFrame, &PrepareCameraSurface::resolve, &PrepareCameraSurface::createWorkspace>(
                        std::move(*surface_config), std::move(uses)
                    );
                    if (!prepared) return cxx::unexpected(prepared.error());
                }
            }
            if (context.outputDemanded(MeshVisibilityPorts::draws.semantic))
            {
                auto published = context.publish(MeshVisibilityPorts::draws.semantic, {
                    {VisibleDraws::groups, *groups},
                    {VisibleDraws::visible, passes->visible},
                    {VisibleDraws::commands, passes->commands}
                });
                if (!published) return cxx::unexpected(published.error());
            }
            if constexpr (ShadowPass) return detail::ShadowVisibilityPass{std::move(raster_pass), shadow_uploads};
            else return raster_pass;
        }
    }

    RenderResult<PassResult>
    detail::appendCameraVisibility(EffectCompileContext& context, std::optional<MeshRasterAttachments> raster, bool cull) noexcept
    {
        if (raster) return appendVisibility<false, true>(context, &*raster, cull, nullptr);
        return appendVisibility<false, false>(context, nullptr, cull, nullptr);
    }

    RenderResult<detail::ShadowVisibilityPass> detail::appendShadowVisibility(
        EffectCompileContext& context, std::optional<MeshRasterAttachments> raster,
        const ShadowProjectionSettings& projection, bool frustum_culling
    ) noexcept
    {
        if (raster) return appendVisibility<true, true>(context, &*raster, frustum_culling, &projection);
        return appendVisibility<true, false>(context, nullptr, frustum_culling, &projection);
    }

    ImplementationEntry detail::meshVisibilityImplementation() noexcept
    {
        ImplementationEntry implementation;
        implementation.capability = capabilities::MeshVisibility.id;
        implementation.identity = {"lux.render.mesh_visibility.camera", capabilities::MeshVisibility.version};
        implementation.supported_outputs = {MeshVisibilityPorts::draws.semantic};
        static constexpr auto build = +[](EffectCompileContext& context, bool cull) noexcept -> RenderResult<void>
        {
            auto pass = appendCameraVisibility(context, {}, cull);
            if (!pass) return cxx::unexpected(pass.error());
            return {};
        };
        implementation.create = CameraVisibility<build>::create;
        return implementation;
    }
}
