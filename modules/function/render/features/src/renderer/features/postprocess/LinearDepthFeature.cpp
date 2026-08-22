#include <lux/engine/render/renderer/features/postprocess/LinearDepthFeature.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/PipelinePresets.hpp>   // makeFullscreenTemplate
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/function/render/client/features/deferred/DeferredGBufferOperation.hpp>

#include <cassert>
#include <vector>

namespace lux::render
{

lux::render::Expected<void> LinearDepthFeature::initAndAttachTo(RenderScene& /*scene*/)
{
    auto& ctx = renderContext();
    auto& shaders = ctx.globalRegistry().must<ShaderResources>();

    // 空句柄回填内置(替换阶梯第 2 级:客户端可换任一着色器)。
    const std::array stage_requests{
        PipelineStageRequest{EBuiltinShader::TONEMAP_VERT,      cfg_.vertex_shader},
        PipelineStageRequest{EBuiltinShader::LINEAR_DEPTH_FRAG, cfg_.fragment_shader}};

    auto stages = preparePipelineStages(shaders, stage_requests);
    if (!stages)
        return lux::cxx::unexpected(stages.error());

    // 深度采样:最近邻(深度不得插值)。缓存句柄,服务持有生命周期。
    depth_sampler_ = ctx.descriptorService().sampler(SamplerDesc::nearestClamp());

    // 全屏三角管线:预置。PC 仅共享头 8 字节(near/far 走 uViews)。
    GraphicsPipelineTemplate tmpl = makeFullscreenTemplate(
        "LinearDepth", /*push_constant_size=*/8, /*alpha_blend=*/false);
    tmpl.vertex_shader   = stages->module(0);
    tmpl.fragment_shader = stages->module(1);

    auto pipeline = ctx.pipelineManager().registerGraphicsTemplate(tmpl, stages->infos());
    if (!pipeline)
        return lux::cxx::unexpected(pipeline.error());

    pipeline_        = *pipeline;
    depth_ds_layout_ = ctx.pipelineManager().templateSetLayout(pipeline_, 1);
    if (depth_ds_layout_ == VK_NULL_HANDLE)
        return renderFailure<err::pipeline::ReflectedSetLayoutMissing>(1);
    return {};
}

void LinearDepthFeature::onDetachFromScene(RenderScene& /*scene*/)
{
    // 采样器是服务缓存句柄、布局归 DescriptorService —— 零退休动作。
    depth_ds_layout_ = VK_NULL_HANDLE;
}

void LinearDepthFeature::addPasses(RGBuilder& builder)
{
    // 两个槽都是引擎按本特性的 requiredTargetSlots() 声明导入的
    // slotted 纹理 —— 按稳定名引用,零魔法字符串。
    auto scene_depth = builder.referenceTexture(targetSlotName(TargetSlot::SCENE_DEPTH));
    auto linear      = builder.referenceTexture(targetSlotName(TargetSlot::LINEAR_DEPTH));

    auto tds = builder.createTransientDS("LinearDepthDS", depth_ds_layout_, {
        {0, EDescriptorType::COMBINED_IMAGE_SAMPLER, scene_depth, depth_sampler_,
         EImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL},
    });

    builder.addPass("LinearDepthResolve", ERGPassType::GRAPHICS)
        .read(scene_depth, lux::render::ETextureRole::SAMPLED)
        .write(linear, lux::render::ETextureRole::COLOR_ATTACHMENT)
        .setPipeline(pipeline_)
        .bindSceneDS()                // set0:uViews(near/far)
        .bindTransientDS(1, tds)      // set1:uSceneDepth(局部资源,发射器自动 b0)
        .setKernelFn([](const PassRecordContext& rec) {
            vkCmdDraw(rec.cmd, 3, 1, 0, 0);
        })
        .setKernel("LinearDepthPass")
        // SceneDepth has several writers and this feature can be contributed
        // before the deferred stack.  Name the terminal geometry producer so
        // this pass never samples the imported image in its initial state.
        .after(kDeferredGBufferDrawPassName)
        .stage(ERenderStage::Geometry);   // 深度写完即产;经共享导入资源的
                                          // 显式 producer edge 排在几何之后
}

} // namespace lux::render
