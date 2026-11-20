#include "MeshView.hpp"
#include <lux/engine/render/effect/BuiltinEffectNodes.hpp>
#include <lux/engine/render/effect/DepthPyramid.hpp>
#include <lux/engine/render/effect/GBuffer.hpp>
#include <lux/engine/render/effect/Lighting.hpp>
#include <lux/engine/render/effect/MeshRaster.hpp>
#include <lux/engine/render/effect/MeshSurfaceStream.hpp>
#include <lux/engine/render/effect/MeshVisibility.hpp>
#include <lux/engine/render/effect/Shadow.hpp>
#include <lux/engine/render/extensions/effect/EffectBuildContext.hpp>
#include <lux/engine/render/extensions/effect/EffectImplementation.hpp>
#include <lux/engine/render/extensions/effect/EffectInitializationContext.hpp>
#include <lux/engine/render/features/mesh/MeshGrouping.hpp>
#include <lux/engine/render/features/mesh/RasterMesh.hpp>
#include <lux/engine/render/features/mesh/MeshVisibilityInputs.hpp>
#include <lux/engine/render/features/mesh/MeshVisibilityWork.hpp>
#include <lux/engine/render/features/shadow/ShadowProjection.hpp>
#include <lux/engine/render/resources/instance/Binding.hpp>
#include <lux/engine/render/resources/instance/Resource.hpp>
#include <lux/engine/render/resources/lighting/Resource.hpp>
#include <lux/engine/render/resources/transform/Binding.hpp>
#include <lux/engine/render/resources/transform/Resource.hpp>
#include <lux/engine/render/runtime/RenderServer.hpp>
#include <NRI.h>
#include <algorithm>
#include <charconv>
#include <cmath>

namespace lux::render
{
    namespace
    {
        enum class EMeshEffect { VISIBILITY, GBUFFER, FORWARD, DEPTH_ONLY, SHADOW };

        class PrepareMeshVisibility final : public InstanceData
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

            struct ResolvedInputs final
            {
                StorageSnapshotSlot<MeshInstanceSnapshot> instances;
                std::optional<InlineConstantWriter<detail::VisibilityViewConstants>> view;
                std::array<PreparedBufferWriter, 4> uploads;
                FrameBufferSizer visible, commands;
                std::array<std::uint32_t, 2> dispatch_limit;
                std::optional<detail::MeshSurfaceStream::Inputs> surface;
                std::optional<ShadowInputs> shadow;
            };

            [[nodiscard]] static RenderResult<ResolvedInputs>
            resolve(const Configuration& config, InstanceLinkContext& context) noexcept
            {
                const auto instances = context.resolveStorageSnapshot<MeshInstanceSnapshot>(config.instances);
                if (!instances) return cxx::unexpected(instances.error());
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
                return ResolvedInputs{
                    *instances, view, {*groups, *candidates, *chunks, *dispatch},
                    *visible, *commands, config.dispatch_limit, surface, shadow
                };
            }

            [[nodiscard]] static InstanceDataResult
            instantiate(const ResolvedInputs& inputs, InstanceInstantiationContext&) noexcept
            {
                return std::make_unique<PrepareMeshVisibility>(inputs);
            }

            explicit PrepareMeshVisibility(ResolvedInputs inputs) noexcept : inputs_(inputs)
            {
                if (inputs.surface) surface_.emplace(*inputs.surface);
                if (inputs.shadow) shadow_.emplace(inputs.shadow->settings);
            }

            RenderResult<void> selectRasterPrograms(RasterProgramSelection& selection) noexcept override
            {
                if (!surface_) return cxx::unexpected(RenderError{kFrameContract});
                const auto instances = selection.readStorage(inputs_.instances);
                if (!instances) return cxx::unexpected(instances.error());
                return surface_->selectPrograms(selection, instances->value.get());
            }

            RenderResult<void> prepare(FramePreparation& frame) noexcept override
            {
                const auto view = frame.viewParameters();
                if (!view) return cxx::unexpected(view.error());
                if (shadow_)
                {
                    const auto lights = frame.readStorage(inputs_.shadow->lights);
                    if (!lights) return cxx::unexpected(lights.error());
                    const auto& value = view->value;
                    const detail::ShadowCamera camera{
                        value.inverse_view_projection, value.position_near, value.direction_far
                    };
                    const auto projected = shadow_->select(camera, lights->value.get());
                    if (!projected) return cxx::unexpected(projected.error());
                    grouping_.setViewCount(static_cast<std::uint32_t>(shadow_->views().size()));
                    const std::array payloads{
                        std::as_bytes(shadow_->views()), std::as_bytes(shadow_->slices()), shadow_->mappings()
                    };
                    for (std::size_t i = 0; i < payloads.size(); ++i)
                    {
                        const auto written = frame.writeBuffer(inputs_.shadow->uploads[i], payloads[i], shadow_->revision());
                        if (!written) return written;
                    }
                }
                else
                {
                    auto constants = frame.write(*inputs_.view, {view->value.view, view->value.projection});
                    if (!constants) return constants;
                }
                const auto selected = frame.readStorage(inputs_.instances);
                if (!selected) return cxx::unexpected(selected.error());
                const auto& snapshot = selected->value.get();
                if (selection_ != selected->revision)
                {
                    updateGroups(snapshot);
                    selection_ = selected->revision;
                }
                if (auto packed = grouping_.pack(&snapshot, lookupGeometry, inputs_.dispatch_limit); !packed)
                    return packed;
                const auto payloads = grouping_.uploads();
                for (std::size_t i = 0; i < payloads.size(); ++i)
                {
                    if (auto written = frame.writeBuffer(inputs_.uploads[i], payloads[i], grouping_.revision()); !written)
                        return written;
                }
                if (auto sized = frame.sizeBuffer(inputs_.visible, grouping_.visibleBytes()); !sized) return sized;
                auto commands = frame.sizeBuffer(
                    inputs_.commands, grouping_.indirectStreamCount() * sizeof(nri::DrawIndexedDesc)
                );
                if (!commands) return commands;
                if (shadow_ && surface_)
                    return surface_->prepareShadow(frame, grouping_.source(), shadow_->views(), shadow_->rasterStates());
                return surface_ ? surface_->prepare(frame, grouping_.source(), view->value) : RenderResult<void>{};
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

            void updateGroups(const MeshInstanceSnapshot& snapshot) noexcept
            {
                current_slots_.clear();
                current_slots_.reserve(snapshot.instances.size());
                for (const auto& instance : snapshot.instances)
                {
                    if (!instance.render_enabled) continue;
                    const auto slot = extension_detail::ResourceIdentity::local(instance.instance).index;
                    const detail::MeshGrouping::Selection selected{
                        instance.mesh, instance.material, instance.program, instance.submesh
                    };
                    grouping_.update(slot, &selected);
                    current_slots_.push_back(slot);
                }
                std::ranges::sort(current_slots_);
                auto active = current_slots_.begin();
                for (auto slot : previous_slots_)
                {
                    while (active != current_slots_.end() && *active < slot) ++active;
                    if (active == current_slots_.end() || *active != slot) grouping_.update(slot, nullptr);
                }
                previous_slots_.swap(current_slots_);
            }

            ResolvedInputs inputs_;
            detail::MeshGrouping grouping_;
            std::optional<detail::MeshSurfaceStream> surface_;
            std::optional<detail::ShadowProjectionData> shadow_;
            std::vector<std::uint32_t> previous_slots_, current_slots_;
            std::uint64_t selection_ = 0;
        };

        RenderResult<void> initializeVisibility(EffectInitializationContext& context, std::string_view args) noexcept
        {
            if (args.empty() || args == "unculled") return {};
            return cxx::unexpected(RenderError{kInvalidConfig, {context.nodeId().value}});
        }

        struct ShadowNodeConfiguration final
        {
            detail::ShadowProjectionSettings projection{{1024, 1}, 100, 1, 0};
            bool frustum_culling = true;
        };

        RenderResult<ShadowNodeConfiguration> parseShadow(std::string_view arguments) noexcept
        {
            ShadowNodeConfiguration configuration;
            if (arguments.empty()) return configuration;
            constexpr std::string_view unculled = ",unculled";
            if (arguments.ends_with(unculled))
            {
                arguments.remove_suffix(unculled.size());
                configuration.frustum_culling = false;
            }
            const auto delimiter = arguments.find(',');
            const auto next = delimiter == std::string_view::npos ? delimiter : arguments.find(',', delimiter + 1);
            if (next == std::string_view::npos) return cxx::unexpected(RenderError{kInvalidConfig});
            const auto read = [](std::string_view text, auto& value) noexcept
            {
                const auto end = text.data() + text.size();
                const auto result = std::from_chars(text.data(), end, value);
                return result.ec == std::errc{} && result.ptr == end;
            };
            auto& settings = configuration.projection;
            const bool parsed_size = read(arguments.substr(0, delimiter), settings.shape.resolution);
            const bool parsed_distance = read(arguments.substr(delimiter + 1, next - delimiter - 1), settings.distance);
            const bool parsed_radius = read(arguments.substr(next + 1), settings.pcf_radius);
            const bool invalid_distance = !std::isfinite(settings.distance) || settings.distance <= 0;
            const bool invalid_shape = settings.pcf_radius > 4 ||
                settings.shape.resolution <= 2 * (settings.pcf_radius + 2);
            if (!parsed_size || !parsed_distance || !parsed_radius || invalid_distance || invalid_shape)
                return cxx::unexpected(RenderError{kInvalidConfig});
            return configuration;
        }

        RenderResult<void> initializeShadow(EffectInitializationContext& context, std::string_view args) noexcept
        {
            if (const auto parsed = parseShadow(args); !parsed)
                return cxx::unexpected(RenderError{kInvalidConfig, {context.nodeId().value}});
            return {};
        }

        template <EMeshEffect Effect, bool CounterClockwise = true>
        RenderResult<void> buildVisibility(EffectBuildContext& context, std::string_view args) noexcept
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

            auto& graph = context.builder();
            const std::string key = "mesh-visibility/" + std::to_string(context.nodeId().value) + "/";
            ResourceVersion views;
            std::vector<GraphPassId> inline_blocks;
            std::optional<PrepareMeshVisibility::ShadowConfiguration> shadow;
            bool frustum_culling = args.empty();
            if constexpr (Effect == EMeshEffect::SHADOW)
            {
                auto configuration = parseShadow(args);
                if (!configuration) return cxx::unexpected(configuration.error());
                auto& settings = configuration->projection;
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
                const auto view_resource = graph.declareResource({key + "views", GraphBufferDesc{sizeof(MeshVisibilityView)}});
                if (!view_resource) return cxx::unexpected(view_resource.error());
                const auto shader = graph.addProgram(GraphProgram{kMeshView.load(context.shaderTarget())});
                if (!shader) return cxx::unexpected(shader.error());
                ComputeWork view;
                view.interface.program = *shader;
                view.interface.resources = {{"views", {{*view_resource, 0}, {}, EGraphAccess::WRITE}}};
                view.interface.parameters.setInlineConstant(detail::VisibilityViewConstants{});
                view.dispatch = DispatchArguments{{1, 1, 1}};
                view.allow_async_compute = true;
                const auto view_pass = graph.addComputePass(key + "view", std::move(view));
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
                graph, key, {
                    *transforms, *alive, *properties, views,
                    *groups, *candidates, *chunks, *visible, *commands, *dispatch
                }, frustum_culling, Effect == EMeshEffect::SHADOW, device.indirect_first_instance
            );
            if (!passes) return cxx::unexpected(passes.error());
            auto config = PrepareMeshVisibility::Configuration{
                *instances, inline_blocks.empty() ? GraphPassId{} : inline_blocks.front(),
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
                const auto mesh = context.importRasterMeshes(GBuffer::geometry_binding);
                if (!mesh) return cxx::unexpected(mesh.error());
                const auto materials = context.importRasterMaterials(GBuffer::materials_binding);
                if (!materials) return cxx::unexpected(materials.error());
                const auto textures = context.importOptionalRasterTextures(GBuffer::textures_binding);
                if (!textures) return cxx::unexpected(textures.error());
                RasterPassTemplate stage;
                stage.purpose = "indexed-mesh";
                stage.inputs = detail::meshRasterInputs();
                stage.runtime_stream_programs = true;
                stage.section_depth_bias = Effect == EMeshEffect::SHADOW;
                stage.mesh_geometry = *mesh;
                stage.shared_materials = *materials;
                stage.shared_textures = *textures;
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
                    const auto depth = graph.declareResource({key + "shadow-depth", shadow->settings.shape.image()});
                    if (!depth) return cxx::unexpected(depth.error());
                    raster.depth = RasterAttachment{{{*depth, 0}}};
                    for (std::size_t i = 0; i < shadow->uploads.size(); ++i)
                        bundle.push_back({effectPortSemantic(Lighting::shadow_members[i]), {shadow->uploads[i], 0}});
                }
                else if constexpr (Effect == EMeshEffect::GBUFFER)
                {
                    for (std::size_t i = 0; i < Lighting::surface_members.size(); ++i)
                    {
                        const bool is_depth = i == rdesc::GBufferLayout::color_formats.size();
                        const auto format = is_depth ? rdesc::GBufferLayout::depth_format : rdesc::GBufferLayout::color_formats[i];
                        GraphImageDesc image{0, 0, 1, 1, format};
                        image.extent.source = EImageExtentSource::TARGET;
                        const auto attachment = graph.declareResource({key + std::string{Lighting::surface_members[i]}, image});
                        if (!attachment) return cxx::unexpected(attachment.error());
                        if (is_depth) raster.depth = RasterAttachment{{{*attachment, 0}}};
                        else raster.colors.push_back({{{*attachment, 0}}});
                    }
                }
                else
                {
                    GraphImageDesc depth_image{0, 0, 1, 1, rdesc::ETextureFormat::D32_SFLOAT};
                    depth_image.extent.source = EImageExtentSource::TARGET;
                    const auto depth = graph.declareResource({key + "depth", depth_image});
                    if (!depth) return cxx::unexpected(depth.error());
                    raster.depth = RasterAttachment{{{*depth, 0}}};
                    if constexpr (Effect == EMeshEffect::FORWARD)
                    {
                        GraphImageDesc color_image{0, 0, 1, 1, target.format};
                        color_image.extent.source = EImageExtentSource::TARGET;
                        const auto color = graph.declareResource({key + "color", color_image});
                        if (!color) return cxx::unexpected(color.error());
                        raster.colors.push_back({{{*color, 0}}});
                    }
                }
                const auto drawn = graph.addRasterPass(key + "raster", std::move(raster));
                if (!drawn) return cxx::unexpected(drawn.error());
                if constexpr (Effect == EMeshEffect::SHADOW)
                    bundle.push_back({effectPortSemantic(Lighting::shadow_members.back()), drawn->written.front()});
                else if constexpr (Effect == EMeshEffect::GBUFFER)
                    for (std::size_t i = 0; i < Lighting::surface_members.size(); ++i)
                        bundle.push_back({effectPortSemantic(Lighting::surface_members[i]), drawn->written[i]});
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
                    paged, device.indirect_first_instance, CounterClockwise, purpose
                };
                producer_passes.push_back(drawn->pass);
                program_inputs.push_back(*instances);
            }
            std::vector<GraphResourceId> prepared_buffers{
                groups->resource, candidates->resource, chunks->resource, dispatch->resource,
                visible->resource, commands->resource
            };
            std::vector<GraphStorageSnapshotId> storage_inputs{*instances};
            if (shadow)
            {
                prepared_buffers.insert(prepared_buffers.end(), shadow->uploads.begin(), shadow->uploads.end());
                storage_inputs.push_back(shadow->lights);
            }
            auto value = ConfigValue::make(config);
            if (!value) return cxx::unexpected(value.error());
            if (auto prepared = context.addPreparation({
                InstanceDataFactory::make<PrepareMeshVisibility>(), std::move(*value), {}, {}, std::move(inline_blocks),
                std::move(producer_passes), std::move(prepared_buffers), std::move(storage_inputs),
                std::move(program_inputs)
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
            {surface, "surface", {std::string{rdesc::GBufferLayout::semantic}, 1},
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
            {shadows, "shadows", {std::string{Lighting::shadow_type}, 1}, graph::EPinDirection::OUTPUT, UINT8_MAX},
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
