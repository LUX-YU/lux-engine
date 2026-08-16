#include <lux/engine/render/renderer/features/postprocess/FogFeature.hpp>

#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/PipelinePresets.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace lux::render
{
    Expected<void> FogFeature::initAndAttachTo(RenderScene&)
    {
        auto& context = renderContext();
        auto& shaders = context.globalRegistry().must<ShaderResources>();
        const std::array requests{
            PipelineStageRequest{
                EBuiltinShader::TONEMAP_VERT,
                config_.vertex_shader},
            PipelineStageRequest{
                EBuiltinShader::FOG_FRAG,
                config_.fragment_shader}};
        auto stages = preparePipelineStages(shaders, requests);
        if (!stages)
            return lux::cxx::unexpected(stages.error());

        color_sampler_ = context.descriptorService().sampler(
            SamplerDesc::linearClamp());
        depth_sampler_ = context.descriptorService().sampler(
            SamplerDesc::nearestClamp());
        auto pipeline = makeFullscreenTemplate(
            "Fog",
            8u + static_cast<std::uint32_t>(sizeof(GpuParams)),
            false);
        pipeline.vertex_shader = stages->module(0);
        pipeline.fragment_shader = stages->module(1);
        auto registered = context.pipelineManager()
            .registerGraphicsTemplate(pipeline, stages->infos());
        if (!registered)
            return lux::cxx::unexpected(registered.error());
        pipeline_ = *registered;
        input_layout_ = context.pipelineManager().templateSetLayout(
            pipeline_, 1u);
        if (input_layout_ == VK_NULL_HANDLE)
            return renderFailure<err::pipeline::ReflectedSetLayoutMissing>(1);
        return {};
    }

    void FogFeature::onDetachFromScene(RenderScene&)
    {
        input_layout_ = VK_NULL_HANDLE;
        color_sampler_ = VK_NULL_HANDLE;
        depth_sampler_ = VK_NULL_HANDLE;
    }

    void FogFeature::setParams(
        const FogSetParamsPayload& params) noexcept
    {
        params_.color_r = std::clamp(params.color[0], 0.0f, 1.0f);
        params_.color_g = std::clamp(params.color[1], 0.0f, 1.0f);
        params_.color_b = std::clamp(params.color[2], 0.0f, 1.0f);
        params_.density = std::max(params.density, 0.0f);
        params_.start_distance = std::max(params.start_distance, 0.0f);
        params_.reference_height = params.reference_height;
        params_.height_falloff = std::max(params.height_falloff, 0.0f);
        params_.maximum_opacity = std::clamp(
            params.maximum_opacity, 0.0f, 1.0f);
        params_.enabled = params.enabled != 0u ? 1u : 0u;
    }

    void FogFeature::addPasses(RGBuilder& builder)
    {
        auto hdr = builder.referenceTexture("LitColor");
        auto depth = builder.referenceTexture(
            targetSlotName(TargetSlot::LINEAR_DEPTH));
        RGTextureDescription description = RGTextureDescription::Relative(
            1.0f,
            1.0f,
            renderScene().pipelineConfig().lit_color_format);
        description.usage =
            static_cast<ERGTextureUsageFlags>(
                ERGTextureUsageBits::COLOR_ATTACHMENT) |
            static_cast<ERGTextureUsageFlags>(
                ERGTextureUsageBits::SAMPLED);
        const auto output = builder.createTexture(
            "FogColor", description);
        const auto descriptors = builder.createTransientDS(
            "FogInputDS",
            input_layout_,
            {
                {0, EDescriptorType::COMBINED_IMAGE_SAMPLER,
                 hdr, color_sampler_,
                 EImageLayout::SHADER_READ_ONLY_OPTIMAL},
                {1, EDescriptorType::COMBINED_IMAGE_SAMPLER,
                 depth, depth_sampler_,
                 EImageLayout::SHADER_READ_ONLY_OPTIMAL},
            });
        builder.addPass("FogComposite", ERGPassType::GRAPHICS)
            .read(hdr, lux::common::ETextureRole::SAMPLED)
            .read(depth, lux::common::ETextureRole::SAMPLED)
            .write(output, lux::common::ETextureRole::COLOR_ATTACHMENT)
            .setPipeline(pipeline_)
            .bindSceneDS()
            .bindTransientDS(1, descriptors)
            .setKernelFn([this](const PassRecordContext& record)
            {
                vkCmdPushConstants(
                    record.cmd,
                    record.pipeline_layout,
                    record.pc_stage_flags,
                    8u,
                    sizeof(params_),
                    &params_);
                vkCmdDraw(record.cmd, 3u, 1u, 0u, 0u);
            })
            .setKernel("FogCompositePass")
            .stage(ERenderStage::PostProcess);
    }
} // namespace lux::render
