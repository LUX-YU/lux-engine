#include <lux/engine/render/renderer/features/render_cluster/RenderClusterFeature.hpp>

#include <lux/engine/render/renderer/features/render_cluster/RenderClusterResources.hpp>
#include <lux/engine/render/renderer/features/BufferTransferSynchronization.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/PipelinePresets.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/resources/mesh/MeshCullCandidateSource.hpp>
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraResource.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/description/Vertex.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lux::render
{
    RenderClusterFeature::RenderClusterFeature(Config config)
        : RenderFeature(RenderFeature::Config{config.name})
        , config_(std::move(config))
    {}

    Expected<void> RenderClusterFeature::initAndAttachTo(RenderScene& scene)
    {
        auto& resources = scene.sceneRegistry().ensure<
            RenderClusterResources>();
        auto* instances = scene.sceneRegistry().find<InstanceResources>();
        if (!instances)
            return renderFailure<err::resource::NotFound>();
        auto& candidate_source = scene.sceneRegistry().ensure<
            MeshCullCandidateSource>();
        auto& context = renderContext();
        if (!resources.initializeGpuCulling(
                context.deviceContext(),
                context.deferredDestroyQueue(),
                context.framesInFlight(),
                instances->capacity()))
        {
            return renderFailure<err::feature::ResourceInitFailed>();
        }
        if (!resources.initializePicking(
                context.deviceContext(),
                context.deferredDestroyQueue(),
                context.framesInFlight()))
        {
            resources.shutdownGpuCulling();
            return renderFailure<err::feature::ResourceInitFailed>();
        }

        auto& shaders = context.globalRegistry().must<ShaderResources>();
        const auto register_pipeline = [&]<typename Pipeline, typename Layout>(
            ShaderHandle configured,
            EBuiltinShader builtin,
            const char* name,
            Pipeline& output_pipeline,
            Layout& output_layout) -> Expected<void>
        {
            const auto shader = resolveShaderStage(
                shaders, configured, builtin);
            if (!shader)
                return lux::cxx::unexpected(shader.error());
            const auto* object = shaders.get(*shader);
            if (!object)
                return renderFailure<err::shader::HandleStale>();
            const auto pipeline = context.pipelineManager().
                registerComputePipelineReflected(
                    object->module, object->info, name);
            if (!pipeline)
                return lux::cxx::unexpected(pipeline.error());
            output_pipeline = *pipeline;
            output_layout = context.pipelineManager().computeSetLayout(
                output_pipeline, 0u);
            if (output_layout == VK_NULL_HANDLE)
            {
                return renderFailure<
                    err::pipeline::ReflectedSetLayoutMissing>(0u);
            }
            return {};
        };
        if (auto result = register_pipeline(
                config_.cluster_cull_compute_shader,
                EBuiltinShader::RENDER_CLUSTER_CULL_COMP,
                "RenderClusterCull",
                cluster_cull_pipeline_,
                cluster_cull_set_layout_); !result)
        {
            candidate_source.clear();
            resources.shutdownGpuCulling();
            resources.shutdownPicking();
            return result;
        }
        if (auto result = register_pipeline(
                config_.candidate_expand_compute_shader,
                EBuiltinShader::RENDER_CLUSTER_EXPAND_COMP,
                "RenderClusterCandidateExpand",
                candidate_expand_pipeline_,
                candidate_expand_set_layout_); !result)
        {
            candidate_source.clear();
            resources.shutdownGpuCulling();
            resources.shutdownPicking();
            return result;
        }
        const auto pick_vertex = resolveShaderStage(
            shaders,
            config_.pick_vertex_shader,
            EBuiltinShader::RENDER_CLUSTER_PICK_VERT);
        const auto pick_fragment = resolveShaderStage(
            shaders,
            config_.pick_fragment_shader,
            EBuiltinShader::RENDER_CLUSTER_PICK_FRAG);
        if (!pick_vertex || !pick_fragment)
        {
            candidate_source.clear();
            resources.shutdownGpuCulling();
            resources.shutdownPicking();
            return !pick_vertex
                ? lux::cxx::unexpected(pick_vertex.error())
                : lux::cxx::unexpected(pick_fragment.error());
        }
        const std::array<ShaderHandle, 2u> pick_stage_handles{
            *pick_vertex,
            *pick_fragment};
        auto pick_stages = shaders.preparePipelineStages(pick_stage_handles);
        if (!pick_stages)
        {
            candidate_source.clear();
            resources.shutdownGpuCulling();
            resources.shutdownPicking();
            return lux::cxx::unexpected(pick_stages.error());
        }
        auto pick_template = makeOpaqueMeshTemplate();
        pick_template.debug_name = "RenderClusterRasterPick";
        pick_template.descriptor_set_count = 8u;
        if (pick_set_layout_ == VK_NULL_HANDLE)
        {
            constexpr VkDescriptorSetLayoutBinding pick_result_binding{
                0u,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                1u,
                VK_SHADER_STAGE_FRAGMENT_BIT,
                nullptr};
            const DescriptorLayoutDesc pick_result_layout{
                .bindings = {&pick_result_binding, 1u},
                .debug_name = "RenderClusterPickResultSetLayout"};
            pick_set_layout_ = context.descriptorService().layout(
                context.descriptorService().registerLayout(
                    pick_result_layout));
        }
        if (pick_set_layout_ == VK_NULL_HANDLE)
        {
            candidate_source.clear();
            resources.shutdownGpuCulling();
            resources.shutdownPicking();
            return renderFailure<err::pipeline::ReflectedSetLayoutMissing>(2u);
        }
        pick_template.explicit_set_layouts.push_back(
            {2u, pick_set_layout_});
        // Only the view domain remains shared. The low-frequency pick pass
        // pushes each object's 64-byte large-coordinate transform with its draw,
        // so it does not consume the Instance descriptor domain. The fragment
        // writable SSBO is deliberately feature-private at slot 2.
        pick_template.active_sets_mask =
            (1u << 0u) | (1u << 2u);
        pick_template.vertex_shader = pick_stages->module(0u);
        pick_template.fragment_shader = pick_stages->module(1u);
        // makeOpaqueMeshTemplate already supplies the complete Vertex layout.
        // Picking consumes position only, so replace that layout instead of
        // appending another binding/location 0 pair.
        pick_template.vertex_bindings.clear();
        pick_template.vertex_attributes.clear();
        pick_template.vertex_bindings.push_back(
            VkVertexInputBindingDescription{
                0u,
                static_cast<std::uint32_t>(sizeof(lux::rdesc::Vertex)),
                VK_VERTEX_INPUT_RATE_VERTEX});
        pick_template.vertex_attributes.push_back(
            VkVertexInputAttributeDescription{
                0u,
                0u,
                VK_FORMAT_R32G32B32_SFLOAT,
                static_cast<std::uint32_t>(
                    offsetof(lux::rdesc::Vertex, position))});
        pick_template.cull_mode = VK_CULL_MODE_NONE;
        // The fragment shader performs the exact per-fragment nearest-depth
        // reduction into the 1x1 result buffer. Disabling fixed-function depth
        // rejection keeps coincident objects deterministic as well: every
        // covered fragment participates in the packed atomicMin.
        pick_template.depth_test_enable = VK_FALSE;
        pick_template.depth_write_enable = VK_FALSE;
        pick_template.depth_compare_op = VK_COMPARE_OP_ALWAYS;
        pick_template.push_constant_ranges.push_back(
            VkPushConstantRange{
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                0u,
                112u});
        const std::array<const lux::rdesc::ShaderInfo*, 2u> pick_infos{
            &pick_stages->info(0u),
            &pick_stages->info(1u)};
        const auto pick_pipeline = context.pipelineManager().
            registerGraphicsTemplate(pick_template, pick_infos);
        if (!pick_pipeline)
        {
            candidate_source.clear();
            resources.shutdownGpuCulling();
            resources.shutdownPicking();
            return lux::cxx::unexpected(pick_pipeline.error());
        }
        pick_pipeline_ = *pick_pipeline;
        pick_set_layout_ = context.pipelineManager().templateSetLayout(
            pick_pipeline_, 2u);
        if (pick_set_layout_ == VK_NULL_HANDLE)
        {
            candidate_source.clear();
            resources.shutdownGpuCulling();
            resources.shutdownPicking();
            return renderFailure<err::pipeline::ReflectedSetLayoutMissing>(2u);
        }
        candidate_source.publish(resources.gpuCullCapacity());
        return {};
    }

    void RenderClusterFeature::onDetachFromScene(RenderScene& scene)
    {
        if (auto* resources = scene.sceneRegistry().find<
                RenderClusterResources>())
        {
            resources->shutdownGpuCulling();
            resources->shutdownPicking();
        }
        if (auto* candidate_source = scene.sceneRegistry().find<
                MeshCullCandidateSource>())
        {
            candidate_source->clear();
        }
        cluster_cull_pipeline_ = {};
        candidate_expand_pipeline_ = {};
        pick_pipeline_ = {};
        cluster_cull_set_layout_ = VK_NULL_HANDLE;
        candidate_expand_set_layout_ = VK_NULL_HANDLE;
        pick_set_layout_ = VK_NULL_HANDLE;
    }

    bool RenderClusterFeature::canRebaseSceneOrigin(
        const std::int64_t origin_delta[3]) const noexcept
    {
        const auto* resources = renderScene().sceneRegistry().find<
            RenderClusterResources>();
        return resources == nullptr ||
            resources->canRebaseSceneOrigin(origin_delta);
    }

    void RenderClusterFeature::rebaseSceneOrigin(
        const std::int64_t origin_delta[3]) noexcept
    {
        if (auto* resources = renderScene().sceneRegistry().find<
                RenderClusterResources>())
        {
            resources->rebaseSceneOrigin(origin_delta);
        }
    }

    bool RenderClusterFeature::allocateViewState(
        std::uint32_t view_index,
        RenderScene& scene)
    {
        auto* resources = scene.sceneRegistry().find<
            RenderClusterResources>();
        auto* instances = scene.sceneRegistry().find<InstanceResources>();
        if (!resources || !instances)
            return false;
        scene.forEachActiveView(
            [view_index, resources, instances](auto& view)
            {
                if (view.handle.index != view_index)
                    return;
                resources->forEachVisibleObject(
                    [&view, instances](RenderObjectHandle object)
                    {
                        const auto slot = instances->resolveSlot(object);
                        if (!instances->isAlive(slot))
                            return;
                        view.registerObject(
                            object,
                            instances->cullMetaAt(slot).bucket_id);
                    });
            });
        return true;
    }

    void RenderClusterFeature::onFrameBegin(
        const FeatureFrameContext& context)
    {
        auto& scene = renderScene();
        auto* resources = scene.sceneRegistry().find<
            RenderClusterResources>();
        auto* instances = scene.sceneRegistry().find<InstanceResources>();
        if (!resources || !instances)
            return;
        resources->onPickingFrameBegin(context.frame_index);
        auto* cameras = scene.sceneRegistry().find<ViewCameraResource>();
        for (const auto family : resources->hierarchyParents())
        {
            const auto* parent = resources->find(family);
            if (!parent)
                continue;
            bool has_camera = false;
            bool prefer_children = false;
            scene.forEachActiveView(
                [&](const auto& view)
                {
                    const auto* camera = cameras
                        ? cameras->find(view.handle.index)
                        : nullptr;
                    if (!camera)
                        return;
                    has_camera = true;
                    double distance_squared = 0.0;
                    for (std::size_t axis = 0u; axis < 3u; ++axis)
                    {
                        const auto page_delta = static_cast<std::int64_t>(
                            parent->header.bounds_center.page_delta[axis]) -
                            static_cast<std::int64_t>(
                                camera->render_origin.page_delta[axis]);
                        const auto relative =
                            static_cast<double>(page_delta) *
                                camera->coordinate_page_size +
                            parent->header.bounds_center.local[axis] -
                            camera->render_origin.local[axis];
                        distance_squared += relative * relative;
                    }
                    const auto distance = std::max(
                        0.01,
                        std::sqrt(distance_squared) -
                            parent->header.bounds_radius);
                    const auto projection_scale = std::abs(
                        static_cast<double>(
                            camera->camera_view.proj(1, 1)));
                    const auto pixel_error =
                        static_cast<double>(parent->header.lod_error) *
                        projection_scale * camera->extent.height /
                        (2.0 * distance);
                    // A family currently showing children uses the lower exit
                    // threshold. This 1.5/2.5 px band prevents a camera resting
                    // on the boundary from swapping representations every frame.
                    const auto threshold =
                        resources->prefersChildren(family)
                        ? resources->hlodExitErrorPixels()
                        : resources->hlodEnterErrorPixels();
                    prefer_children = prefer_children ||
                        pixel_error > threshold;
                });
            if (!has_camera)
                prefer_children = true;

            for (const auto change : resources->reconcileHierarchy(
                     family,
                     prefer_children,
                     scene.sceneTime(),
                     resources->transitionDurationSeconds()))
            {
                const auto* cluster = resources->find(change.id);
                if (!cluster)
                    continue;
                for (std::size_t index = 0u;
                     index < cluster->objects.size(); ++index)
                {
                    const auto object = cluster->objects[index];
                    const auto slot = instances->resolveSlot(object);
                    if (!instances->isAlive(slot))
                        continue;
                    const auto bucket = instances->cullMetaAt(slot).bucket_id;
                    auto property = instances->propertyAt(slot);
                    property.transition_start_time =
                        change.transition_start_time;
                    property.transition_duration =
                        change.transition_duration;
                    property.transition_seed = change.transition_seed;
                    property.transition_flags =
                        change.transition == RenderClusterResources::
                                ETransitionAction::NONE
                        ? 0u
                        : 1u | (change.transition ==
                                      RenderClusterResources::
                                          ETransitionAction::FADE_OUT
                                  ? 2u
                                  : 0u);
                    auto flags = property.flags;
                    if (change.visible)
                        flags |= kInstanceFlagVisible;
                    else
                        flags &= ~kInstanceFlagVisible;
                    property.flags = flags;
                    instances->writeProperty(slot, property);
                    scene.forEachActiveView(
                        [object, bucket, visible = change.visible](auto& view)
                        {
                            if (visible)
                                view.registerObject(object, bucket);
                            else
                                view.unregisterObject(object);
                        });
                }
            }
        }

        bool capacity_changed = false;
        auto* candidate_source = scene.sceneRegistry().find<
            MeshCullCandidateSource>();
        if (!resources->prepareGpuCulling(
                context.frame_index, *instances, capacity_changed))
        {
            if (candidate_source && candidate_source->active())
            {
                candidate_source->clear();
                scene.invalidateGraph(
                    EGraphInvalidationReason::MDC_STORAGE_GENERATION);
            }
            renderContext().reportError(
                renderError<err::feature::ResourceInitFailed>(),
                scene.sceneId().index,
                context.frame_index);
            return;
        }
        if (candidate_source)
            candidate_source->publish(resources->gpuCullCapacity());
        if (capacity_changed)
            scene.invalidateGraph(
                EGraphInvalidationReason::MDC_STORAGE_GENERATION);
    }

    void RenderClusterFeature::addPasses(RGBuilder& builder)
    {
        auto* resources = renderScene().sceneRegistry().find<
            RenderClusterResources>();
        auto* instances = renderScene().sceneRegistry().find<
            InstanceResources>();
        if (!resources || !instances)
        {
            return;
        }

        const auto import_storage = [&builder](
            std::string name,
            VkDeviceSize size,
            std::uint32_t stride,
            auto getter)
        {
            RGBufferDescription description{};
            description.size = std::max<VkDeviceSize>(size, stride);
            description.stride = stride;
            description.element_count = static_cast<std::uint32_t>(
                description.size / stride);
            description.usage = static_cast<ERGBufferUsageFlags>(
                ERGBufferUsageBits::STORAGE);
            description.memory_usage = ERGMemoryUsage::GPU_ONLY;
            RGImportedBufferInfo imported{};
            imported.buffer_getter = std::move(getter);
            return builder.importBuffer(
                std::move(name), description, std::move(imported));
        };

        auto* candidate_source = renderScene().sceneRegistry().find<
            MeshCullCandidateSource>();
        if (candidate_source && candidate_source->active() &&
            cluster_cull_pipeline_.valid() &&
            candidate_expand_pipeline_.valid() &&
            cluster_cull_set_layout_ != VK_NULL_HANDLE &&
            candidate_expand_set_layout_ != VK_NULL_HANDLE &&
            resources->gpuCullBufferCount() != 0u)
        {
            const auto candidate_capacity = candidate_source->capacity();
            const auto import_cull_input = [&builder, resources](
                std::string name,
                VkDeviceSize size,
                std::uint32_t stride,
                bool clusters)
            {
                RGBufferDescription description{};
                description.size = std::max<VkDeviceSize>(size, stride);
                description.stride = stride;
                description.element_count = static_cast<std::uint32_t>(
                    description.size / stride);
                description.usage = static_cast<ERGBufferUsageFlags>(
                    ERGBufferUsageBits::STORAGE);
                description.memory_usage = ERGMemoryUsage::CPU_TO_GPU;
                RGImportedBufferInfo imported{};
                imported.initial_access = VK_ACCESS_2_HOST_WRITE_BIT;
                imported.initial_stage = VK_PIPELINE_STAGE_2_HOST_BIT;
                imported.buffer_getter = [resources, clusters](
                    VkBuffer* output, std::uint32_t capacity)
                {
                    const auto count = std::min(
                        capacity, resources->gpuCullBufferCount());
                    for (std::uint32_t index = 0u; index < count; ++index)
                    {
                        output[index] = clusters
                            ? resources->gpuCullClusterBuffer(index)
                            : resources->gpuCullInstanceBuffer(index);
                    }
                    return count;
                };
                return builder.importBuffer(
                    std::move(name), description, std::move(imported));
            };

            const auto cluster_input = import_cull_input(
                "RenderClusterCullInput",
                static_cast<VkDeviceSize>(candidate_capacity) *
                    sizeof(RenderClusterResources::GpuCullCluster),
                sizeof(RenderClusterResources::GpuCullCluster),
                true);
            const auto cluster_instance_input = import_cull_input(
                "RenderClusterInstanceInput",
                static_cast<VkDeviceSize>(candidate_capacity) *
                    sizeof(RenderClusterResources::GpuCullInstance),
                sizeof(RenderClusterResources::GpuCullInstance),
                false);
            const auto candidate_dynamic_slots = import_storage(
                "RenderClusterCandidateDynamicSlots",
                static_cast<VkDeviceSize>(
                    std::max(instances->capacity(), 1u)) *
                    sizeof(std::uint32_t),
                sizeof(std::uint32_t),
                [instances](VkBuffer* output, std::uint32_t capacity)
                {
                    if (!output || capacity == 0u)
                        return 0u;
                    output[0] = instances->dynamicSlotBuffer();
                    return output[0] == VK_NULL_HANDLE ? 0u : 1u;
                });

            RGBufferDescription visibility_description{};
            visibility_description.size = static_cast<VkDeviceSize>(
                candidate_capacity) * sizeof(std::uint32_t);
            visibility_description.stride = sizeof(std::uint32_t);
            visibility_description.element_count = candidate_capacity;
            visibility_description.usage = static_cast<
                ERGBufferUsageFlags>(ERGBufferUsageBits::STORAGE);
            visibility_description.memory_usage = ERGMemoryUsage::GPU_ONLY;
            const auto visibility = builder.createBuffer(
                "RenderClusterVisibility", visibility_description);

            RGBufferDescription candidate_description{};
            candidate_description.size = static_cast<VkDeviceSize>(
                candidate_capacity) * sizeof(std::uint32_t);
            candidate_description.stride = sizeof(std::uint32_t);
            candidate_description.element_count = candidate_capacity;
            candidate_description.usage =
                ERGBufferUsageBits::STORAGE |
                ERGBufferUsageBits::TRANSFER_DST;
            candidate_description.memory_usage = ERGMemoryUsage::GPU_ONLY;
            const auto candidates = builder.createBuffer(
                MeshCullCandidateSource::kCandidateSlotsResource,
                candidate_description);

            RGBufferDescription dispatch_description{};
            dispatch_description.size = sizeof(CandidateDispatchState);
            dispatch_description.stride = sizeof(std::uint32_t);
            dispatch_description.element_count =
                sizeof(CandidateDispatchState) / sizeof(std::uint32_t);
            dispatch_description.usage = static_cast<
                ERGBufferUsageFlags>(
                    static_cast<std::uint32_t>(
                        ERGBufferUsageBits::STORAGE) |
                    static_cast<std::uint32_t>(
                        ERGBufferUsageBits::INDIRECT) |
                    static_cast<std::uint32_t>(
                        ERGBufferUsageBits::TRANSFER_DST));
            dispatch_description.memory_usage = ERGMemoryUsage::CPU_TO_GPU;
            RGImportedBufferInfo dispatch_import{};
            dispatch_import.initial_access = VK_ACCESS_2_HOST_READ_BIT;
            dispatch_import.initial_stage = VK_PIPELINE_STAGE_2_HOST_BIT;
            dispatch_import.final_access = VK_ACCESS_2_HOST_READ_BIT;
            dispatch_import.final_stage = VK_PIPELINE_STAGE_2_HOST_BIT;
            dispatch_import.buffer_getter = [resources](
                VkBuffer* output, std::uint32_t capacity)
            {
                const auto count = std::min(
                    capacity, resources->gpuCullBufferCount());
                for (std::uint32_t index = 0u; index < count; ++index)
                {
                    output[index] =
                        resources->gpuCandidateDispatchBuffer(index);
                }
                return count;
            };
            const auto dispatch_args = builder.importBuffer(
                MeshCullCandidateSource::kDispatchArgsResource,
                dispatch_description,
                dispatch_import);

            const auto cull_descriptors = builder.createTransientDS(
                "RenderClusterCullDS",
                cluster_cull_set_layout_,
                {
                    {0u, EDescriptorType::STORAGE_BUFFER, cluster_input},
                    {1u, EDescriptorType::STORAGE_BUFFER, visibility},
                });
            builder.addPass("RenderClusterCull", ERGPassType::COMPUTE)
                .setComputePipeline(cluster_cull_pipeline_)
                .bindTransientDS(0u, cull_descriptors)
                .read(cluster_input, ERGBufferRole::STORAGE)
                .write(visibility, ERGBufferRole::STORAGE)
                .setKernelFn([resources, this](
                    const PassRecordContext& context)
                {
                    if (!context.view ||
                        context.pipeline_layout == VK_NULL_HANDLE)
                    {
                        return;
                    }
                    const auto cluster_count = resources->gpuCullClusterCount(
                        context.frame.frame_index);
                    if (cluster_count == 0u)
                        return;
                    struct alignas(16) ClusterCullPush final
                    {
                        std::uint32_t scene_index{0u};
                        std::uint32_t view_index{0u};
                        std::uint32_t cluster_count{0u};
                        std::uint32_t frustum_enabled{0u};
                        std::int32_t origin_page[4]{};
                        float origin_local_page_size[4]{};
                        float view_proj[16]{};
                    };
                    static_assert(sizeof(ClusterCullPush) == 112u);
                    ClusterCullPush push{};
                    push.scene_index = context.frame.scene_index;
                    push.view_index = context.frame.view_index;
                    push.cluster_count = cluster_count;
                    auto* cameras = renderScene().sceneRegistry().find<
                        ViewCameraResource>();
                    const auto* camera = cameras
                        ? cameras->find(context.view->handle.index)
                        : nullptr;
                    if (camera)
                    {
                        push.frustum_enabled = 1u;
                        for (std::size_t axis = 0u; axis < 3u; ++axis)
                        {
                            push.origin_page[axis] =
                                camera->render_origin.page_delta[axis];
                            push.origin_local_page_size[axis] =
                                camera->render_origin.local[axis];
                        }
                        push.origin_local_page_size[3] =
                            camera->coordinate_page_size;
                        const Eigen::Matrix4f relative_view_projection =
                            viewRelativeViewProjection(*camera);
                        std::memcpy(
                            push.view_proj,
                            relative_view_projection.data(),
                            sizeof(push.view_proj));
                    }
                    vkCmdPushConstants(
                        context.cmd,
                        context.pipeline_layout,
                        VK_SHADER_STAGE_COMPUTE_BIT,
                        0u,
                        sizeof(push),
                        &push);
                    vkCmdDispatch(
                        context.cmd,
                        (cluster_count + 63u) / 64u,
                        1u,
                        1u);
                });

            const auto expand_descriptors = builder.createTransientDS(
                "RenderClusterCandidateExpandDS",
                candidate_expand_set_layout_,
                {
                    {0u, EDescriptorType::STORAGE_BUFFER,
                        cluster_instance_input},
                    {1u, EDescriptorType::STORAGE_BUFFER, visibility},
                    {2u, EDescriptorType::STORAGE_BUFFER,
                        candidate_dynamic_slots},
                    {4u, EDescriptorType::STORAGE_BUFFER, candidates},
                    {5u, EDescriptorType::STORAGE_BUFFER, dispatch_args},
                });

            constexpr std::string_view candidate_clear_pass =
                "RenderClusterCandidateClear";
            constexpr std::string_view candidate_append_pass =
                "RenderClusterCandidateAppend";
            builder.addPass(candidate_clear_pass, ERGPassType::TRANSFER)
                .write(candidates, ERGBufferRole::STORAGE)
                .write(dispatch_args, ERGBufferRole::STORAGE)
                .setKernelFn([
                    candidates,
                    dispatch_args,
                    candidate_capacity](const PassRecordContext& context)
                {
                    const auto candidate_buffer =
                        context.resolveBufferHandle(candidates);
                    const auto dispatch_buffer =
                        context.resolveBufferHandle(dispatch_args);
                    if (candidate_buffer == VK_NULL_HANDLE ||
                        dispatch_buffer == VK_NULL_HANDLE)
                    {
                        return;
                    }
                    synchronizeBeforeBufferTransferWrites(
                        context.cmd,
                        std::array{candidate_buffer, dispatch_buffer}
                    );
                    vkCmdFillBuffer(
                        context.cmd,
                        candidate_buffer,
                        0u,
                        static_cast<VkDeviceSize>(candidate_capacity) *
                            sizeof(std::uint32_t),
                        std::numeric_limits<std::uint32_t>::max());
                    vkCmdFillBuffer(
                        context.cmd,
                        dispatch_buffer,
                        0u,
                        sizeof(CandidateDispatchState),
                        0u);
                });

            struct alignas(16) CandidateExpandPush final
            {
                std::uint32_t scene_index{0u};
                std::uint32_t view_index{0u};
                std::uint32_t cluster_instance_count{0u};
                std::uint32_t dynamic_count{0u};
                std::uint32_t candidate_capacity{0u};
                std::uint32_t cluster_count{0u};
                std::uint32_t mode{0u};
                std::uint32_t reserved{0u};
            };
            static_assert(sizeof(CandidateExpandPush) == 32u);

            builder.addPass(candidate_append_pass, ERGPassType::COMPUTE)
                .setComputePipeline(candidate_expand_pipeline_)
                .bindTransientDS(0u, expand_descriptors)
                .read(cluster_instance_input, ERGBufferRole::STORAGE)
                .read(visibility, ERGBufferRole::STORAGE)
                .read(candidate_dynamic_slots, ERGBufferRole::STORAGE)
                .write(candidates, ERGBufferRole::STORAGE)
                .readWrite(dispatch_args, ERGBufferRole::STORAGE)
                .after(candidate_clear_pass)
                .setKernelFn([
                    resources,
                    instances,
                    candidate_capacity](const PassRecordContext& context)
                {
                    if (!context.view ||
                        context.pipeline_layout == VK_NULL_HANDLE)
                    {
                        return;
                    }
                    CandidateExpandPush push{};
                    push.scene_index = context.frame.scene_index;
                    push.view_index = context.frame.view_index;
                    push.cluster_instance_count =
                        resources->gpuCullInstanceCount(
                            context.frame.frame_index);
                    push.dynamic_count = instances->dynamicCount();
                    push.candidate_capacity = candidate_capacity;
                    push.cluster_count = resources->gpuCullClusterCount(
                        context.frame.frame_index);
                    vkCmdPushConstants(
                        context.cmd,
                        context.pipeline_layout,
                        VK_SHADER_STAGE_COMPUTE_BIT,
                        0u,
                        sizeof(push),
                        &push);
                    const auto input_count = std::max(
                        push.cluster_instance_count,
                        push.dynamic_count);
                    if (input_count != 0u)
                    {
                        vkCmdDispatch(
                            context.cmd,
                            (input_count + 63u) / 64u,
                            1u,
                            1u);
                    }
                });

            builder.addPass(
                    MeshCullCandidateSource::kProducerPass,
                    ERGPassType::COMPUTE)
                .setComputePipeline(candidate_expand_pipeline_)
                .bindTransientDS(0u, expand_descriptors)
                .read(candidates, ERGBufferRole::STORAGE)
                .readWrite(dispatch_args, ERGBufferRole::STORAGE)
                .after(candidate_append_pass)
                .setKernelFn([
                    resources,
                    candidate_capacity](const PassRecordContext& context)
                {
                    if (!context.view ||
                        context.pipeline_layout == VK_NULL_HANDLE)
                    {
                        return;
                    }
                    CandidateExpandPush push{};
                    push.scene_index = context.frame.scene_index;
                    push.view_index = context.frame.view_index;
                    push.candidate_capacity = candidate_capacity;
                    push.mode = 1u;
                    vkCmdPushConstants(
                        context.cmd,
                        context.pipeline_layout,
                        VK_SHADER_STAGE_COMPUTE_BIT,
                        0u,
                        sizeof(push),
                        &push);
                    vkCmdDispatch(context.cmd, 1u, 1u, 1u);
                    resources->markGpuCandidateSubmitted(
                        context.frame.frame_index);
                });
        }

        if (!pick_pipeline_.valid() ||
            pick_set_layout_ == VK_NULL_HANDLE ||
            resources->pickBufferCount() == 0u)
        {
            return;
        }

        RGBufferDescription result_description{};
        result_description.size = sizeof(std::uint32_t);
        result_description.stride = sizeof(std::uint32_t);
        result_description.element_count = 1u;
        result_description.usage = static_cast<ERGBufferUsageFlags>(
            ERGBufferUsageBits::STORAGE);
        result_description.memory_usage = ERGMemoryUsage::CPU_TO_GPU;
        RGImportedBufferInfo result_import{};
        result_import.initial_access = VK_ACCESS_2_HOST_WRITE_BIT;
        result_import.initial_stage = VK_PIPELINE_STAGE_2_HOST_BIT;
        result_import.final_access = VK_ACCESS_2_HOST_READ_BIT;
        result_import.final_stage = VK_PIPELINE_STAGE_2_HOST_BIT;
        result_import.buffer_getter = [resources](
            VkBuffer* output, std::uint32_t capacity)
        {
            const auto count = std::min(
                capacity, resources->pickBufferCount());
            for (std::uint32_t index = 0u; index < count; ++index)
                output[index] = resources->pickBuffer(index);
            return count;
        };
        const auto result = builder.importBuffer(
            "RenderClusterPickResult",
            result_description,
            result_import);

        const auto descriptors = builder.createTransientDS(
            "RenderClusterPickDS",
            pick_set_layout_,
            {
                {0u, EDescriptorType::STORAGE_BUFFER, result},
            });

        auto* meshes = renderContext().globalRegistry().find<MeshResources>();
        auto* vertex_pools = renderScene().sceneRegistry().find<
            VertexPoolRegistry>();
        if (!meshes || !vertex_pools || !vertex_pools->isInitialized())
            return;
        const auto import_segmented_buffers = [&builder, meshes](
            std::string_view prefix,
            std::uint16_t count,
            VkBuffer (MeshResources::*buffer_at)(std::uint16_t) const,
            ERGBufferUsageBits usage,
            std::uint32_t stride)
        {
            std::vector<RGResourceHandle> buffers;
            buffers.reserve(count);
            for (std::uint16_t segment = 0u; segment < count; ++segment)
            {
                RGBufferDescription description{};
                description.stride = stride;
                description.usage = static_cast<ERGBufferUsageFlags>(usage);
                description.memory_usage = ERGMemoryUsage::GPU_ONLY;
                RGImportedBufferInfo import{};
                import.buffer_getter = [meshes, buffer_at, segment](
                    VkBuffer* output,
                    std::uint32_t capacity)
                {
                    if (!output || capacity == 0u)
                        return 0u;
                    output[0] = (meshes->*buffer_at)(segment);
                    return output[0] == VK_NULL_HANDLE ? 0u : 1u;
                };
                buffers.push_back(builder.importBuffer(
                    std::string(prefix) + "." + std::to_string(segment),
                    description,
                    import));
            }
            return buffers;
        };
        const auto index_buffers = import_segmented_buffers(
            "ClassicMeshPickIndexBuffer",
            meshes->iboSegmentCount(),
            &MeshResources::indexBuffer,
            ERGBufferUsageBits::INDEX,
            sizeof(std::uint32_t));
        const auto vertex_buffers = import_segmented_buffers(
            "ClassicMeshPickVertexBuffer",
            meshes->vboSegmentCount(),
            &MeshResources::vertexBuffer,
            ERGBufferUsageBits::VERTEX,
            sizeof(lux::rdesc::Vertex));
        if (index_buffers.empty() || vertex_buffers.empty())
            return;

        auto depth_description = RGTextureDescription::Absolute(
            1u,
            1u,
            lux::common::ETextureFormat::D32_SFLOAT);
        depth_description.usage = static_cast<ERGTextureUsageFlags>(
            ERGTextureUsageBits::DEPTH_STENCIL);
        depth_description.keep_transient = true;
        const auto depth = builder.createTexture(
            "RenderClusterPickDepth", depth_description);

        auto pick_pass = builder.addPass(
            "RenderClusterRasterPick", ERGPassType::GRAPHICS);
        pick_pass.setPipeline(pick_pipeline_)
            .bindSceneDS()
            .bindTransientDS(2u, descriptors)
            .write(result, ERGBufferRole::STORAGE)
            .write(
                depth,
                lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
            .setManualViewport(true)
            .markSideEffect();
        for (const auto buffer : vertex_buffers)
            pick_pass.read(buffer, ERGBufferRole::VERTEX);
        for (const auto buffer : index_buffers)
            pick_pass.read(buffer, ERGBufferRole::INDEX);
        pick_pass.setKernelFn(
            [this,
             resources,
             instances,
             meshes,
             vertex_buffers,
             index_buffers](
                const PassRecordContext& context)
            {
                if (!context.view ||
                    context.pipeline_layout == VK_NULL_HANDLE)
                {
                    return;
                }
                const auto request = resources->pickRequestForView(
                    context.view->handle.index);
                if (!request)
                    return;
                if (request->view_generation != context.view->handle.gen)
                {
                    resources->failPick(
                        *request, ERenderPickStatus::STALE);
                    return;
                }
                const VkViewport pick_viewport{
                    0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
                const VkRect2D pick_scissor{{0, 0}, {1u, 1u}};
                vkCmdSetViewport(
                    context.cmd, 0u, 1u, &pick_viewport);
                vkCmdSetScissor(
                    context.cmd, 0u, 1u, &pick_scissor);
                struct PickPush final
                {
                    std::uint32_t scene_index{0u};
                    std::uint32_t view_index{0u};
                    std::uint32_t instance_slot{0u};
                    std::uint32_t pick_token{0u};
                    float normalized_x{0.0f};
                    float normalized_y{0.0f};
                    float maximum_distance{0.0f};
                    float scene_time{0.0f};
                    InstanceTransform transform{};
                    float transition_start_time{0.0f};
                    float transition_duration{0.0f};
                    std::uint32_t transition_seed{0u};
                    std::uint32_t transition_flags{0u};
                };
                static_assert(sizeof(PickPush) == 112u);
                PickPush push{};
                push.scene_index = context.frame.scene_index;
                push.view_index = context.frame.view_index;
                push.normalized_x = request->normalized_x;
                push.normalized_y = request->normalized_y;
                push.maximum_distance = request->maximum_distance;
                push.scene_time = renderScene().sceneTime();
                std::uint16_t bound_vbo = ~std::uint16_t{0u};
                std::uint16_t bound_ibo = ~std::uint16_t{0u};
                VkIndexType bound_index_type = VK_INDEX_TYPE_MAX_ENUM;
                resources->forEachVisiblePickObject(
                    [&](RenderObjectHandle object, std::uint32_t token)
                    {
                        const auto slot = instances->resolveSlot(object);
                        if (!instances->isAlive(slot))
                            return;
                        const auto& cull = instances->cullMetaAt(slot);
                        if (cull.lod_count == 0u)
                            return;
                        const auto& entries = instances->mdcTable().entries();
                        const auto mdc = cull.lod_mdc[0u];
                        if (mdc >= entries.size())
                            return;
                        const auto binding = instances->resourceBinding(object);
                        if (!binding)
                            return;
                        const auto* mesh = meshes->getGpuRecord(binding->mesh);
                        if (!mesh || !mesh->ready ||
                            mesh->vbo_segment >= vertex_buffers.size() ||
                            entries[mdc].ibo_segment >= index_buffers.size())
                        {
                            return;
                        }
                        if (mesh->vbo_segment != bound_vbo)
                        {
                            const auto vertex = context.resolveBufferHandle(
                                vertex_buffers[mesh->vbo_segment]);
                            if (vertex == VK_NULL_HANDLE)
                                return;
                            constexpr VkDeviceSize vertex_offset = 0u;
                            vkCmdBindVertexBuffers(
                                context.cmd,
                                0u,
                                1u,
                                &vertex,
                                &vertex_offset);
                            bound_vbo = mesh->vbo_segment;
                        }
                        if (entries[mdc].ibo_segment != bound_ibo ||
                            entries[mdc].index_type != bound_index_type)
                        {
                            const auto index = context.resolveBufferHandle(
                                index_buffers[entries[mdc].ibo_segment]);
                            if (index == VK_NULL_HANDLE)
                                return;
                            vkCmdBindIndexBuffer(
                                context.cmd,
                                index,
                                0u,
                                entries[mdc].index_type);
                            bound_ibo = entries[mdc].ibo_segment;
                            bound_index_type = entries[mdc].index_type;
                        }
                        const auto& section = instances->meshSectionAt(
                            entries[mdc].section_id);
                        if (section.index_count == 0u)
                            return;
                        push.instance_slot = slot.index;
                        push.pick_token = token;
                        push.transform = instances->transformAt(slot);
                        const auto& property =
                            instances->propertyAt(slot);
                        push.transition_start_time =
                            property.transition_start_time;
                        push.transition_duration =
                            property.transition_duration;
                        push.transition_seed = property.transition_seed;
                        push.transition_flags = property.transition_flags;
                        vkCmdPushConstants(
                            context.cmd,
                            context.pipeline_layout,
                            VK_SHADER_STAGE_VERTEX_BIT |
                                VK_SHADER_STAGE_FRAGMENT_BIT,
                            0u,
                            sizeof(PickPush),
                            &push);
                        vkCmdDrawIndexed(
                            context.cmd,
                            section.index_count,
                            1u,
                            section.first_index,
                            section.base_vertex,
                            0u);
                    });
                resources->markPickSubmitted(
                    context.frame.frame_index, *request);
            });
    }
} // namespace lux::render
