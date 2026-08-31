#include <array>
#include <lux/engine/render/renderer/features/postprocess/SsaoFeature.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/PipelinePresets.hpp> // makeFullscreenTemplate
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/resources/lighting/LightResources.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

#include <cassert>
#include <vector>

namespace lux::render
{

    lux::render::Expected<void> SsaoFeature::initAndAttachTo(RenderScene& scene)
    {
        auto& ctx = renderContext();
        auto& shaders = ctx.globalRegistry().must<ShaderResources>();

        const std::array stage_requests{
            PipelineStageRequest{EBuiltinShader::TONEMAP_VERT, {}},
            PipelineStageRequest{EBuiltinShader::SSAO_FRAG, {}}};

        auto stages = preparePipelineStages(shaders, stage_requests);
        if (!stages)
            return lux::cxx::unexpected(stages.error());

        // 线性深度采样:最近邻(深度差窗口不能吃跨边缘插值出来的中间值)。
        input_sampler_ = ctx.descriptorService().sampler(SamplerDesc::nearestClamp());

        GraphicsPipelineTemplate tmpl = makeFullscreenTemplate("Ssao", /*push_constant_size=*/8, /*alpha_blend=*/false);
        tmpl.vertex_shader = stages->module(0);
        tmpl.fragment_shader = stages->module(1);

        auto pipeline = ctx.pipelineManager().registerGraphicsTemplate(tmpl, stages->infos());
        if (!pipeline)
            return lux::cxx::unexpected(pipeline.error());

        pipeline_ = *pipeline;
        input_ds_layout_ = ctx.pipelineManager().templateSetLayout(pipeline_, 1);
        if (input_ds_layout_ == VK_NULL_HANDLE)
            return renderFailure<err::pipeline::ReflectedSetLayoutMissing>(1);

        // b11 的接收端。LightResources 由 LightFeature ensure —— 装配顺序上
        // 灯光特性先行(压测/编排器均如此);缺席时本特性静默不发布(AO 仍
        // 会被渲染,只是没人消费)。
        light_res_ = scene.resources().find<LightResources>();
        return {};
    }

    void SsaoFeature::onDetachFromScene(RenderScene& /*scene*/)
    {
        // 把 b11 槽还给默认白 —— AO 纹理是图 persistent,特性卸载后由图回收,
        // 不还槽就是悬空 view。
        if (light_res_ != nullptr)
            light_res_->provideShadingInput(EShadingInputSlot::AmbientOcclusion, VK_NULL_HANDLE);
        light_res_ = nullptr;
        provided_view_ = VK_NULL_HANDLE;
        input_ds_layout_ = VK_NULL_HANDLE;
    }

    void SsaoFeature::addPasses(RGBuilder& builder)
    {
        auto linear = builder.referenceTexture(targetSlotName(TargetSlot::LINEAR_DEPTH));

        RGTextureDescription ao_desc = RGTextureDescription::Relative(1.0f, 1.0f, lux::rdesc::ETextureFormat::R8_UNORM);
        ao_desc.usage = static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::COLOR_ATTACHMENT) |
                        static_cast<ERGTextureUsageFlags>(ERGTextureUsageBits::SAMPLED);
        // persistent:跨帧单副本 —— b11 描述符在图外持有 view,不许被别名复用。
        ao_desc.allow_aliasing = false;
        auto ao = builder.createPersistentTexture("SceneSsao", ao_desc);

        auto tds = builder.createTransientDS(
            "SsaoInputDS",
            input_ds_layout_,
            {
                {0,
                 EDescriptorType::COMBINED_IMAGE_SAMPLER,
                 linear,
                 input_sampler_,
                 EImageLayout::SHADER_READ_ONLY_OPTIMAL},
            }
        );

        builder.addPass("SsaoResolve", ERGPassType::GRAPHICS)
            .read(linear, lux::render::ETextureRole::SAMPLED)
            .write(ao, lux::render::ETextureRole::COLOR_ATTACHMENT)
            .setPipeline(pipeline_)
            .bindTransientDS(1, tds) // set1:uLinearDepth(发射器自动 b0)
            .setKernelFn([](const PassRecordContext& rec) { vkCmdDraw(rec.cmd, 3, 1, 0, 0); })
            .setKernel("SsaoResolvePass")
            .stage(ERenderStage::Geometry); // LinearDepthResolve 之后:写后读依赖排序

        // 发布 pass:不录任何 GPU 命令。read(SAMPLED) 让图把 AO 转到
        // SHADER_READ_ONLY 并排 barrier(b11 的真实读者 deferred_lighting 在
        // 图外,图看不见那条边);record 期解析物理 view,变了就重指 b11。
        // TRANSFER 型:GRAPHICS 要 renderpass、COMPUTE 强制 compute 管线,
        // 只有它允许"纯 barrier + CPU 回调"的空 pass。
        builder.addPass("SsaoPublish", ERGPassType::TRANSFER)
            .read(ao, lux::render::ETextureRole::SAMPLED)
            .markSideEffect(true)
            .setKernelFn([this, ao](const PassRecordContext& rec) {
                const VkImageView view = rec.resolveTextureView(ao);
                if (view != VK_NULL_HANDLE && view != provided_view_ && light_res_ != nullptr)
                {
                    // UPDATE_AFTER_BIND:录制窗口内更新、提交前生效 —— 本帧的
                    // deferred_lighting 就能吃到新 view。
                    light_res_->provideShadingInput(EShadingInputSlot::AmbientOcclusion, view);
                    provided_view_ = view;
                }
            }
            )
            .setKernel("SsaoPublishPass")
            .stage(ERenderStage::Geometry);
    }

} // namespace lux::render
