#include <lux/engine/render/renderer/features/streaming_feedback/StreamingFeedbackFeature.hpp>
#include <lux/engine/function/render/client/genops/StreamingFeedbackOperation.ops.hpp>

#include <StreamingFeedbackPassParams.pass.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGPassTypes.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/PipelinePresets.hpp>
#include <lux/engine/render/gpu/pipeline/VertexLayoutRegistry.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>
#include <lux/engine/render/resources/material/MaterialResources.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>
#include <lux/engine/render/resources/vertex/VertexProduction.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/resources/material/MaterialFamily.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

#include <array>
#include <bit>
#include <vector>

namespace lux::render
{
    StreamingFeedbackFeature::StreamingFeedbackFeature(Config config)
        : config_(std::move(config))
    {}

    StreamingFeedbackFeature::~StreamingFeedbackFeature()
    {
        releaseAll();
    }

    void StreamingFeedbackFeature::releaseAll() noexcept
    {
        visible_set_layout_ = VK_NULL_HANDLE;
        composite_set_layout_ = VK_NULL_HANDLE;
        destroyCommon();
    }

    Expected<void> StreamingFeedbackFeature::initAndAttachTo(RenderScene&)
    {
        return init();
    }

    void StreamingFeedbackFeature::onDetachFromScene(RenderScene&)
    {
        releaseAll();
    }

    Expected<void> StreamingFeedbackFeature::init()
    {
        auto& context = renderContext();
        auto& shaders = context.globalRegistry().must<ShaderResources>();

        const std::array backfill{
            ShaderStageSlot{
                EBuiltinShader::MESH_CULL_UNIFIED_COMP,
                &config_.cull_shader},
            ShaderStageSlot{
                EBuiltinShader::MDC_COMPACT_COMP,
                &config_.compact_shader},
            ShaderStageSlot{
                EBuiltinShader::HIGHLIGHT_MASK_VERT,
                &config_.mask_vert},
            ShaderStageSlot{
                EBuiltinShader::STREAMING_FEEDBACK_MASK_FRAG,
                &config_.mask_frag},
            ShaderStageSlot{
                EBuiltinShader::STREAMING_FEEDBACK_COMPOSITE_FRAG,
                &config_.composite_frag}};
        if (auto filled = resolveShaderStages(shaders, backfill); !filled)
            return filled;

        if (auto initialized = initCommon(
                config_.cull_shader,
                config_.extension_flags);
            !initialized)
        {
            return initialized;
        }

        auto layout_id = context.descriptorService().registerLayout(
            storageBufferVertexLayout("StreamingFeedbackVisibleSetLayout"));
        visible_set_layout_ = context.descriptorService().layout(layout_id);
        mask_sampler_ = context.descriptorService().sampler(
            SamplerDesc::linearClamp());

        {
            auto mesh_template = makeOpaqueMeshTemplate();
            mesh_template.descriptor_set_count = 8;
            mesh_template.blend_enable = VK_FALSE;
            mesh_template.depth_test_enable = VK_FALSE;
            mesh_template.depth_write_enable = VK_FALSE;
            mesh_template.cull_mode = VK_CULL_MODE_NONE;
            mesh_template.debug_name = "StreamingFeedbackMaskDraw";
            mesh_template.push_constant_ranges.push_back(
                {VK_SHADER_STAGE_VERTEX_BIT, 0, kViewPushPrefixSize});

            auto& layouts = context.globalRegistry().must<VertexLayoutRegistry>();
            const ShaderHandle mask_frag = config_.mask_frag;
            auto pipelines = registerFamilyPipelines(
                mesh_template,
                config_.mask_vert,
                layouts,
                kDefaultVertexLayoutId,
                [mask_frag](EShadingModel) noexcept
                {
                    return mask_frag;
                });
            if (!pipelines)
                return pipelines;
        }

        {
            const std::array requests{
                PipelineStageRequest{EBuiltinShader::TONEMAP_VERT, {}},
                PipelineStageRequest{
                    EBuiltinShader::STREAMING_FEEDBACK_COMPOSITE_FRAG,
                    config_.composite_frag}};
            auto stages = preparePipelineStages(shaders, requests);
            if (!stages)
                return lux::cxx::unexpected(stages.error());

            auto pipeline_template = makeFullscreenTemplate(
                "StreamingFeedbackComposite",
                pass_gen::kStreamingFeedbackPassParamsPCTotalSize,
                true);
            pipeline_template.vertex_shader = stages->module(0);
            pipeline_template.fragment_shader = stages->module(1);
            const std::array<const rdesc::ShaderInfo*, 2> infos{
                &stages->info(0),
                &stages->info(1)};
            auto pipeline = context.pipelineManager().registerGraphicsTemplate(
                pipeline_template,
                infos);
            if (!pipeline)
                return lux::cxx::unexpected(pipeline.error());
            composite_pipeline_ = *pipeline;
            composite_set_layout_ = context.pipelineManager().templateSetLayout(
                *pipeline,
                1);
            if (composite_set_layout_ == VK_NULL_HANDLE)
                return renderFailure<err::pipeline::ReflectedSetLayoutMissing>(1);
        }

        if (auto compact = initCompactPipeline(
                config_.compact_shader,
                "StreamingFeedbackCompactLayout");
            !compact)
        {
            return lux::cxx::unexpected(compact.error());
        }
        return {};
    }

    void StreamingFeedbackFeature::addPasses(RGBuilder& builder)
    {
        auto& context = renderContext();
        RGTextureDescription mask_description =
            RGTextureDescription::Relative(
                1.0f,
                1.0f,
                lux::rdesc::ETextureFormat::R8_UNORM);
        mask_description.usage =
            static_cast<ERGTextureUsageFlags>(
                ERGTextureUsageBits::COLOR_ATTACHMENT) |
            static_cast<ERGTextureUsageFlags>(
                ERGTextureUsageBits::SAMPLED);
        auto mask = builder.createTexture(
            config_.mask_target,
            mask_description);

        // Empty active sets create no GPU work and allocate no live transient
        // backing.  The condition encloses cull, compact, mask and composite.
        auto chain = builder.conditionChain([this]() noexcept
        {
            constexpr std::uint32_t bit = std::countr_zero(
                kInstanceFlagStreamingFeedback);
            return instance_res_ != nullptr &&
                instance_res_->flagBitCount(bit) != 0u;
        });

        addCullAndCompactPasses(builder, CullCompactParams{
            .prefix = "Sf",
            .phase = ECoreRenderPhase::GBuffer,
            .domain = EPassDomain::GBuffer,
            .cull_pass_name = "StreamingFeedbackCull",
            .compact_pass_name = "StreamingFeedbackCompact",
            .descriptor_layout_version =
                config_.descriptor_layout_version,
            .extension_flags = config_.extension_flags});

        auto visible = builder.createTransientDS(
            "StreamingFeedbackVisibleDS",
            visible_set_layout_,
            {{0, EDescriptorType::STORAGE_BUFFER, visible_instance_rg_}});

        auto* material_resources =
            context.globalRegistry().find<MaterialResources>();
        auto buckets = collectVariantBuckets(material_resources);
        auto* vertex_pools =
            renderScene().sceneRegistry().find<VertexPoolRegistry>();

        auto draw = builder.addPass(
                "StreamingFeedbackMaskDraw",
                ERGPassType::GRAPHICS)
            .write(mask, lux::render::ETextureRole::COLOR_ATTACHMENT)
            .setPipeline(bucket_pipelines_.pick(0u, buckets[0]))
            .bindSceneDS()
            .useEngineSet(EDescriptorSetSlot::Instance)
            .bindTransientDS(5, visible)
            .read(draw_indirect_rg_, ERGBufferRole::INDIRECT)
            .read(draw_count_rg_, ERGBufferRole::INDIRECT)
            .read(visible_instance_rg_, ERGBufferRole::STORAGE)
            .after("StreamingFeedbackCompact")
            .stage(ERenderStage::Overlay);

        const auto bucket_count = static_cast<std::uint32_t>(buckets.size());
        for (std::uint32_t bucket = 1; bucket < bucket_count; ++bucket)
            draw.addPipeline(bucket_pipelines_.pick(bucket, buckets[bucket]));
        if (vertex_pools != nullptr && vertex_pools->isInitialized())
            draw.useEngineSet(EDescriptorSetSlot::VertexPool);
        if (auto* producers =
                renderScene().sceneRegistry().find<VertexProductionRegistry>())
        {
            for (const auto& producer : producers->producers())
                draw.read(
                    builder.referenceBuffer(producer.rg_buffer_name),
                    ERGBufferRole::STORAGE);
        }
        {
            std::vector<std::uint32_t> variant_features;
            variant_features.reserve(bucket_count);
            for (const auto& bucket : buckets)
                variant_features.push_back(bucket.feature_mask);
            draw.setPipelineVariantFeatures(variant_features);
        }
        const auto index_buffers = importSharedIndexBuffers(builder);
        draw.setKernel("MeshDraw", makeKernelConfig(MeshDrawKernelConfig{
            .draw_count_rg = draw_count_rg_,
            .indirect_rg = draw_indirect_rg_,
            .index_buffers_rg = index_buffers.data(),
            .index_buffer_count = static_cast<std::uint32_t>(
                index_buffers.size()),
            .geometry_mask = supportedGeometryMask(),
            .mdc_count = mdcCount(),
            .mdc_entries = instance_res_->mdcTable().entries().data(),
            .family_count = 0u}));

        const StreamingFeedbackPassParams params{
            .mask = mask,
            .mask_sampler = mask_sampler_,
            .color_out = builder.referenceTexture(config_.color_target),
            .scalars = {
                .color_r = config_.color[0],
                .color_g = config_.color[1],
                .color_b = config_.color[2],
                .intensity = config_.intensity,
                .tile_size = config_.tile_size,
                .speed = config_.speed,
                .time_seconds = 0.0f,
                .pattern = static_cast<float>(config_.pattern)}};
        auto descriptors = pass_gen::createTransientDS(
            builder,
            composite_set_layout_,
            params);
        auto composite = builder.addPass(
            "StreamingFeedbackComposite",
            ERGPassType::GRAPHICS);
        pass_gen::declareGraphIO(composite, params);
        composite
            .setPipeline(composite_pipeline_)
            .bindSceneDS()
            .bindTransientDS(1, descriptors)
            .setKernelFn([this, scalars = params.scalars](
                             const PassRecordContext& record) mutable noexcept
            {
                scalars.time_seconds = std::chrono::duration<float>(
                    std::chrono::steady_clock::now() - start_time_).count();
                pass_gen::pushScalars(record, scalars);
                vkCmdDraw(record.cmd, 3, 1, 0, 0);
            })
            .setKernel("FullscreenQuad")
            .after("StreamingFeedbackMaskDraw")
            .stage(ERenderStage::Overlay);
    }
}
