#include <lux/engine/render/renderer/features/terrain/TerrainFeature.hpp>

#include <lux/engine/render/renderer/features/terrain/TerrainResources.hpp>
#include <lux/engine/render/renderer/features/BufferTransferSynchronization.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraResource.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/PipelinePresets.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/graph/RGRecorder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/function/render/client/features/deferred/DeferredGBufferOperation.hpp>
#include <lux/engine/render/renderer/features/deferred/GBufferTypes.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/scene/View.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

namespace lux::render
{
    TerrainFeature::TerrainFeature(Config config)
        : RenderFeature(RenderFeature::Config{config.name}), config_(std::move(config))
    {
    }

    Expected<void> TerrainFeature::initAndAttachTo(RenderScene& scene)
    {
        if (config_.page_capacity == 0u || config_.maximum_selected_patches == 0u)
            return renderFailure<err::feature::ResourceInitFailed>();
        resources_ = &scene.sceneRegistry().ensure<TerrainResources>(config_.page_capacity);
        auto& context = renderContext();
        resources_->setRetireScheduler(
            &contextView().retireScheduler(),
            static_cast<FrameRetireScheduler::OwnerToken>(reinterpret_cast<std::uintptr_t>(resources_))
        );
        if (!resources_->initializeGpuCache(
                context.deviceContext(),
                context.deferredDestroyQueue(),
                context.framesInFlight()))
        {
            resources_ = nullptr;
            return renderFailure<err::feature::ResourceInitFailed>();
        }

        auto& shaders = context.globalRegistry().must<ShaderResources>();
        const auto select_shader =
            resolveShaderStage(shaders, config_.patch_select_shader, EBuiltinShader::TERRAIN_PATCH_SELECT_COMP);
        if (!select_shader)
            return lux::cxx::unexpected(select_shader.error());
        const auto* select_object = shaders.get(*select_shader);
        if (!select_object)
            return renderFailure<err::shader::HandleStale>();
        const auto select_pipeline = context.pipelineManager().registerComputePipelineReflected(
            select_object->module,
            select_object->info,
            "TerrainPatchSelect"
        );
        if (!select_pipeline)
            return lux::cxx::unexpected(select_pipeline.error());
        patch_select_pipeline_ = *select_pipeline;
        patch_select_layout_ = context.pipelineManager().computeSetLayout(patch_select_pipeline_, 0u);
        if (patch_select_layout_ == VK_NULL_HANDLE)
            return renderFailure<err::pipeline::ReflectedSetLayoutMissing>(0u);

        const auto vertex_shader =
            resolveShaderStage(shaders, config_.patch_vertex_shader, EBuiltinShader::TERRAIN_PATCH_VERT);
        const auto fragment_shader =
            resolveShaderStage(shaders, config_.patch_fragment_shader, EBuiltinShader::TERRAIN_PATCH_FRAG);
        if (!vertex_shader || !fragment_shader)
        {
            return !vertex_shader ? lux::cxx::unexpected(vertex_shader.error())
                                  : lux::cxx::unexpected(fragment_shader.error());
        }
        const std::array<ShaderHandle, 2u> stage_handles{*vertex_shader, *fragment_shader};
        const auto stages = shaders.preparePipelineStages(stage_handles);
        if (!stages)
            return lux::cxx::unexpected(stages.error());
        auto terrain_template = makeOpaqueMeshTemplate();
        terrain_template.debug_name = "TerrainPatchGBuffer";
        // Runtime slots 0..2 belong to the Global/Bindless/Feature domains.
        // The source shader's set 2 is a private four-buffer Terrain shape;
        // the domain merger relocates that complete shape after the fixed
        // domains. Leave capacity for the resulting private runtime slot.
        terrain_template.descriptor_set_count = 4u;
        terrain_template.active_sets_mask = (1u << 0u) | (1u << 1u) | (1u << 2u);
        terrain_template.vertex_shader = stages->module(0u);
        terrain_template.fragment_shader = stages->module(1u);
        terrain_template.vertex_bindings.clear();
        terrain_template.vertex_attributes.clear();
        // Heightfield terrain is single-sided. Keeping BACK/CCW prevents an
        // underground camera from rasterizing the complete terrain underside
        // and turning every near-plane crossing patch into full-screen work.
        terrain_template.cull_mode = VK_CULL_MODE_BACK_BIT;
        terrain_template.depth_compare_op = VK_COMPARE_OP_LESS;
        terrain_template.push_constant_ranges.push_back(VkPushConstantRange{VK_SHADER_STAGE_VERTEX_BIT, 0u, 16u});
        const std::array<const lux::rdesc::ShaderInfo*, 2u> stage_infos{&stages->info(0u), &stages->info(1u)};
        const auto terrain_pipeline = context.pipelineManager().registerGraphicsTemplate(terrain_template, stage_infos);
        if (!terrain_pipeline)
            return lux::cxx::unexpected(terrain_pipeline.error());
        patch_pipeline_ = *terrain_pipeline;
        const auto private_layout = context.pipelineManager().templatePrivateSetLayout(patch_pipeline_, 2u);
        if (!private_layout)
            return renderFailure<err::pipeline::ReflectedSetLayoutMissing>(2u);
        patch_layout_ = private_layout.layout;
        patch_slot_ = private_layout.runtime_slot;
        return {};
    }

    void TerrainFeature::onDetachFromScene(RenderScene&)
    {
        if (resources_)
            resources_->shutdownGpuCache();
        resources_ = nullptr;
        cameras_ = nullptr;
        wanted_views_.clear();
        patch_select_pipeline_ = {};
        patch_pipeline_ = {};
        patch_select_layout_ = VK_NULL_HANDLE;
        patch_layout_ = VK_NULL_HANDLE;
        patch_slot_ = 2u;
    }

    bool TerrainFeature::canRebaseSceneOrigin(const std::int64_t origin_delta[3]) const noexcept
    {
        if (resources_ && !resources_->canRebaseSceneOrigin(origin_delta))
        {
            return false;
        }
        for (const auto& view : wanted_views_)
        {
            if (!canRebaseRenderPageDelta(view.position.page_delta, origin_delta))
            {
                return false;
            }
        }
        return true;
    }

    void TerrainFeature::rebaseSceneOrigin(const std::int64_t origin_delta[3]) noexcept
    {
        if (resources_)
            resources_->rebaseSceneOrigin(origin_delta);
        for (auto& view : wanted_views_)
        {
            rebaseRenderPageDelta(view.position.page_delta, origin_delta);
        }
    }

    void TerrainFeature::onFrameBegin(const FeatureFrameContext& context)
    {
        if (!resources_)
            return;
        resources_->beginFrame();
        resources_->onSelectionFrameBegin(context.frame_index);
        if (!cameras_)
            cameras_ = renderScene().sceneRegistry().find<ViewCameraResource>();
        wanted_views_.clear();
        if (cameras_)
        {
            renderScene().forEachActiveView([this](View& view) {
                const auto* camera = cameras_->find(view.handle.index);
                if (!camera)
                    return;
                wanted_views_.push_back(TerrainResources::ViewOrigin{
                    camera->render_origin,
                    camera->coordinate_page_size,
                    static_cast<float>(std::max(view.current_extent.height, 1u)) *
                        std::fabs(camera->camera_view.proj.data()[5]) * 0.5f}
                );
            }
            );
        }
        resources_->reconcileWanted(
            wanted_views_,
            config_.wanted_radius,
            renderScene().sceneTime(),
            config_.demotion_delay_frames
        );
    }

    void TerrainFeature::addPasses(RGBuilder& builder)
    {
        if (!resources_ || !patch_select_pipeline_.valid() || !patch_pipeline_.valid())
        {
            return;
        }

        const auto import_buffer =
            [&builder](std::string_view name, VkBuffer buffer, std::uint64_t size, std::uint32_t stride) {
                RGBufferDescription description{};
                description.size = size;
                description.stride = stride;
                description.element_count = stride == 0u ? 0u : static_cast<std::uint32_t>(size / stride);
                description.usage = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::STORAGE);
                description.memory_usage = ERGMemoryUsage::CPU_TO_GPU;
                RGImportedBufferInfo imported{};
                imported.initial_access = VK_ACCESS_2_HOST_WRITE_BIT;
                imported.initial_stage = VK_PIPELINE_STAGE_2_HOST_BIT;
                imported.final_access = VK_ACCESS_2_HOST_WRITE_BIT;
                imported.final_stage = VK_PIPELINE_STAGE_2_HOST_BIT;
                imported.buffer_getter = [buffer](VkBuffer* output, std::uint32_t capacity) {
                    if (!output || capacity == 0u || buffer == VK_NULL_HANDLE)
                        return 0u;
                    output[0] = buffer;
                    return 1u;
                };
                return builder.importBuffer(name, description, imported);
            };
        RGBufferDescription metadata_description{};
        metadata_description.size =
            static_cast<std::uint64_t>(sizeof(TerrainResources::GpuPageMeta)) * resources_->fallbackCapacityPages();
        metadata_description.stride = sizeof(TerrainResources::GpuPageMeta);
        metadata_description.element_count = resources_->fallbackCapacityPages();
        metadata_description.usage = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::STORAGE);
        metadata_description.memory_usage = ERGMemoryUsage::CPU_TO_GPU;
        RGImportedBufferInfo metadata_import{};
        metadata_import.initial_access = VK_ACCESS_2_HOST_WRITE_BIT;
        metadata_import.initial_stage = VK_PIPELINE_STAGE_2_HOST_BIT;
        metadata_import.final_access = VK_ACCESS_2_HOST_WRITE_BIT;
        metadata_import.final_stage = VK_PIPELINE_STAGE_2_HOST_BIT;
        metadata_import.buffer_getter = [resources = resources_](VkBuffer* output, std::uint32_t capacity) {
            const auto count = std::min(capacity, resources->pageMetadataBufferCount());
            for (std::uint32_t index = 0u; index < count; ++index)
                output[index] = resources->pageMetadataBuffer(index);
            return count;
        };
        const auto metadata = builder.importBuffer("TerrainPageMetadata", metadata_description, metadata_import);
        const auto full_pages = import_buffer(
            "TerrainFullPages",
            resources_->fullPageBuffer(),
            static_cast<std::uint64_t>(TerrainResources::fullPageStride()) * resources_->capacityPages(),
            static_cast<std::uint32_t>(TerrainResources::fullPageStride())
        );
        const auto fallback_pages = import_buffer(
            "TerrainFallbackPages",
            resources_->fallbackPageBuffer(),
            static_cast<std::uint64_t>(TerrainResources::fallbackPageStride()) * resources_->fallbackCapacityPages(),
            static_cast<std::uint32_t>(TerrainResources::fallbackPageStride())
        );

        const auto maximum_patches =
            std::min(resources_->fallbackCapacityPages() * 64u, config_.maximum_selected_patches);
        RGBufferDescription count_description{};
        count_description.size = sizeof(std::uint32_t);
        count_description.stride = sizeof(std::uint32_t);
        count_description.element_count = 1u;
        count_description.usage = ERGBufferUsageBits::STORAGE | ERGBufferUsageBits::TRANSFER_DST;
        count_description.memory_usage = ERGMemoryUsage::GPU_TO_CPU;
        RGImportedBufferInfo count_import{};
        count_import.initial_access = VK_ACCESS_2_HOST_READ_BIT;
        count_import.initial_stage = VK_PIPELINE_STAGE_2_HOST_BIT;
        count_import.final_access = VK_ACCESS_2_HOST_READ_BIT;
        count_import.final_stage = VK_PIPELINE_STAGE_2_HOST_BIT;
        count_import.buffer_getter = [resources = resources_](VkBuffer* output, std::uint32_t capacity) {
            const auto count = std::min(capacity, resources->selectionCountBufferCount());
            for (std::uint32_t index = 0u; index < count; ++index)
                output[index] = resources->selectionCountBuffer(index);
            return count;
        };
        const auto selected_count = builder.importBuffer("TerrainSelectedPatchCount", count_description, count_import);
        RGBufferDescription selection_description{};
        selection_description.size =
            static_cast<std::uint64_t>(maximum_patches) * sizeof(TerrainResources::GpuTerrainPatch);
        selection_description.stride = sizeof(TerrainResources::GpuTerrainPatch);
        selection_description.element_count = maximum_patches;
        selection_description.usage = static_cast<ERGBufferUsageFlags>(ERGBufferUsageBits::STORAGE);
        selection_description.memory_usage = ERGMemoryUsage::GPU_ONLY;
        const auto selection = builder.createBuffer("TerrainPatchSelection", selection_description);
        RGBufferDescription indirect_description{};
        indirect_description.size = sizeof(VkDrawIndirectCommand);
        indirect_description.stride = sizeof(VkDrawIndirectCommand);
        indirect_description.element_count = 1u;
        indirect_description.usage = ERGBufferUsageBits::STORAGE | ERGBufferUsageBits::INDIRECT;
        indirect_description.usage |= ERGBufferUsageBits::TRANSFER_DST;
        indirect_description.memory_usage = ERGMemoryUsage::GPU_ONLY;
        const auto indirect = builder.createBuffer("TerrainPatchIndirect", indirect_description);

        const auto select_descriptors = builder.createTransientDS(
            "TerrainPatchSelectDS",
            patch_select_layout_,
            {
                {0u, EDescriptorType::STORAGE_BUFFER, metadata},
                {1u, EDescriptorType::STORAGE_BUFFER, selection},
                {2u, EDescriptorType::STORAGE_BUFFER, selected_count},
                {3u, EDescriptorType::STORAGE_BUFFER, indirect},
            }
        );
        builder.addPass("TerrainPatchSelect", ERGPassType::COMPUTE)
            .setComputePipeline(patch_select_pipeline_)
            .bindTransientDS(0u, select_descriptors)
            .read(metadata, ERGBufferRole::STORAGE)
            .write(selection, ERGBufferRole::STORAGE)
            .write(selected_count, ERGBufferRole::STORAGE)
            .write(indirect, ERGBufferRole::STORAGE)
            .setKernelFn([this, indirect, selected_count, maximum_patches](const PassRecordContext& context) {
                const auto indirect_buffer = context.resolveBufferHandle(indirect);
                const auto count_buffer = context.resolveBufferHandle(selected_count);
                if (indirect_buffer == VK_NULL_HANDLE || count_buffer == VK_NULL_HANDLE ||
                    context.pipeline_layout == VK_NULL_HANDLE)
                {
                    return;
                }
                synchronizeBeforeBufferTransferWrites(context.cmd, std::array{indirect_buffer, count_buffer});
                vkCmdFillBuffer(context.cmd, indirect_buffer, 0u, sizeof(VkDrawIndirectCommand), 0u);
                vkCmdFillBuffer(context.cmd, count_buffer, 0u, sizeof(std::uint32_t), 0u);
                std::array<VkBufferMemoryBarrier2, 2u> barriers{};
                for (auto& barrier : barriers)
                {
                    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                    barrier.offset = 0u;
                    barrier.size = VK_WHOLE_SIZE;
                }
                barriers[0].buffer = indirect_buffer;
                barriers[1].buffer = count_buffer;
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.bufferMemoryBarrierCount = static_cast<std::uint32_t>(barriers.size());
                dependency.pBufferMemoryBarriers = barriers.data();
                vkCmdPipelineBarrier2(context.cmd, &dependency);

                struct alignas(16) SelectPush final
                {
                    std::uint32_t page_capacity{0u};
                    std::uint32_t maximum_patches{0u};
                    std::uint32_t view_index{0u};
                    std::uint32_t full_page_capacity{0u};
                    std::int32_t camera_page[4]{};
                    float camera_local_page_size[4]{};
                    float viewport_projection[4]{};
                    float view_rotation_row_0[4]{};
                    float view_rotation_row_1[4]{};
                    float view_rotation_row_2[4]{};
                };
                static_assert(sizeof(SelectPush) == 112u);
                SelectPush push{};
                push.page_capacity = resources_->fallbackCapacityPages();
                push.maximum_patches = maximum_patches;
                push.view_index = context.frame.view_index;
                push.full_page_capacity = resources_->capacityPages();
                auto* cameras = renderScene().sceneRegistry().find<ViewCameraResource>();
                const auto* camera = context.view && cameras ? cameras->find(context.view->handle.index) : nullptr;
                if (camera)
                {
                    for (std::size_t axis = 0u; axis < 3u; ++axis)
                    {
                        push.camera_page[axis] = camera->render_origin.page_delta[axis];
                        push.camera_local_page_size[axis] = camera->render_origin.local[axis];
                    }
                    push.camera_local_page_size[3] = camera->coordinate_page_size;
                    push.viewport_projection[0] = static_cast<float>(std::max(context.view->current_extent.height, 1u));
                    push.viewport_projection[1] = std::fabs(camera->camera_view.proj.data()[5]);
                    push.viewport_projection[2] = 64.0f;
                    push.viewport_projection[3] = std::fabs(camera->camera_view.proj.data()[0]);
                    for (std::size_t column = 0u; column < 3u; ++column)
                    {
                        push.view_rotation_row_0[column] = camera->camera_view.view(0, column);
                        push.view_rotation_row_1[column] = camera->camera_view.view(1, column);
                        push.view_rotation_row_2[column] = camera->camera_view.view(2, column);
                    }
                }
                else
                {
                    push.maximum_patches = 0u;
                    push.camera_local_page_size[3] = 1024.0f;
                }
                vkCmdPushConstants(
                    context.cmd,
                    context.pipeline_layout,
                    VK_SHADER_STAGE_COMPUTE_BIT,
                    0u,
                    sizeof(push),
                    &push
                );
                vkCmdDispatch(context.cmd, 1u, 1u, 1u);
                resources_->markSelectionSubmitted(context.frame.frame_index);
            }
            );

        const auto patch_descriptors = builder.createTransientDS(
            "TerrainPatchRasterDS",
            patch_layout_,
            {
                {0u, EDescriptorType::STORAGE_BUFFER, metadata},
                {1u, EDescriptorType::STORAGE_BUFFER, full_pages},
                {2u, EDescriptorType::STORAGE_BUFFER, fallback_pages},
                {3u, EDescriptorType::STORAGE_BUFFER, selection},
            }
        );
        const GBufferLayout gbuffer{};
        builder.addPass("TerrainGBuffer", ERGPassType::GRAPHICS)
            .setPipeline(patch_pipeline_)
            .bindSceneDS()
            .bindTransientDS(patch_slot_, patch_descriptors)
            .read(metadata, ERGBufferRole::STORAGE)
            .read(full_pages, ERGBufferRole::STORAGE)
            .read(fallback_pages, ERGBufferRole::STORAGE)
            .read(selection, ERGBufferRole::STORAGE)
            .read(indirect, ERGBufferRole::INDIRECT)
            .write(
                builder.referenceTexture(gbuffer.albedo_metallic, ERGReference::Required),
                lux::render::ETextureRole::COLOR_ATTACHMENT
            )
            .write(
                builder.referenceTexture(gbuffer.normal_roughness, ERGReference::Required),
                lux::render::ETextureRole::COLOR_ATTACHMENT
            )
            .write(
                builder.referenceTexture(gbuffer.emissive_ao, ERGReference::Required),
                lux::render::ETextureRole::COLOR_ATTACHMENT
            )
            .write(
                builder.referenceTexture("SceneDepth", ERGReference::Required),
                lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT
            )
            // Selection and VkDrawIndirectCommand are produced by the compute
            // pass in this frame.  The GBuffer pass also participates in the
            // wider deferred painter order, so that unrelated constraint must
            // not be allowed to move this consumer ahead of its producer.
            .after("TerrainPatchSelect")
            .before(kDeferredGBufferDrawPassName)
            .stage(ERenderStage::Geometry)
            .setKernelFn([indirect](const PassRecordContext& context) {
                const auto buffer = context.resolveBufferHandle(indirect);
                if (buffer == VK_NULL_HANDLE || context.pipeline_layout == VK_NULL_HANDLE)
                {
                    return;
                }
                struct RasterPush final
                {
                    std::uint32_t scene_index{0u};
                    std::uint32_t view_index{0u};
                    std::uint32_t full_stride{0u};
                    std::uint32_t fallback_stride{0u};
                };
                RasterPush push{};
                push.scene_index = context.frame.scene_index;
                push.view_index = context.frame.view_index;
                push.full_stride = static_cast<std::uint32_t>(TerrainResources::fullPageStride());
                push.fallback_stride = static_cast<std::uint32_t>(TerrainResources::fallbackPageStride());
                vkCmdPushConstants(
                    context.cmd,
                    context.pipeline_layout,
                    VK_SHADER_STAGE_VERTEX_BIT,
                    0u,
                    sizeof(push),
                    &push
                );
                vkCmdDrawIndirect(context.cmd, buffer, 0u, 1u, sizeof(VkDrawIndirectCommand));
            }
            );
    }
} // namespace lux::render
