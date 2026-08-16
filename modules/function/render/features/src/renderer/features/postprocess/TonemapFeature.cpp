#include <lux/engine/render/renderer/features/postprocess/TonemapFeature.hpp>

// 生成物:由 TonemapPassParams.hpp 注解生成(手写原型已退役)
#include <TonemapPassParams.pass.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/PipelinePresets.hpp>   // makeFullscreenTemplate
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/VulkanCheck.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

#include <array>
#include <vector>

namespace lux::render
{

// =========================================================================
//  Construction / destruction
// =========================================================================

TonemapFeature::TonemapFeature(Config cfg)
    : cfg_(std::move(cfg))
{
    // Seed the live param struct from the bring-up Config so runtime tuning and
    // the initial render agree. cfg_ keeps shaders/targets; pass_params_.scalars
    // owns the tunable trio (exposure/gamma/operator).
    pass_params_.scalars.exposure    = cfg_.exposure;
    pass_params_.scalars.gamma       = cfg_.gamma;
    pass_params_.scalars.tone_map_op = static_cast<std::uint32_t>(cfg_.tone_map_op);
}

TonemapFeature::~TonemapFeature()
{
    destroy();
}

// =========================================================================
//  Lifecycle
// =========================================================================

lux::render::Expected<void> TonemapFeature::initAndAttachTo(RenderScene& /*scene*/)
{
    return init();
}

void TonemapFeature::onDetachFromScene(RenderScene& /*scene*/)
{
    // hdr_sampler_ 来自 DescriptorService 采样器缓存——服务持有生命
    // 周期,特性侧零退休动作。§2.2: layout is owned by DescriptorService.
    hdr_ds_layout_ = VK_NULL_HANDLE;
}

// =========================================================================
//  Initialisation
// =========================================================================

lux::render::Expected<void> TonemapFeature::init()
{
    auto& ctx = renderContext();
    VkDevice device = ctx.deviceContext().logicalDevice();
    auto& shaders = ctx.globalRegistry().must<ShaderResources>();

    // 解析内置默认 + 域合并切换 + 取模块反射,一次做完。
    // 这条管线没有 FEATURE 域成员(uViews 是恒等位、uHDRColor 是私有的),所以切换实质
    // 是个空操作 —— 但标记本身是注册通过的前提。
    const std::array stage_requests{
        PipelineStageRequest{EBuiltinShader::TONEMAP_VERT, cfg_.vertex_shader},
        PipelineStageRequest{EBuiltinShader::TONEMAP_FRAG, cfg_.fragment_shader}};

    auto stages = preparePipelineStages(shaders, stage_requests);
    if (!stages)
        return lux::cxx::unexpected(stages.error());

    // ---- HDR 采样器:共享缓存(线性 clamp) ----
    hdr_sampler_ = ctx.descriptorService().sampler(SamplerDesc::linearClamp());

    // 不手写 DS 布局 / 管线布局:模板的 pipeline_layout 留空,由 registerGraphicsTemplate
    // 从着色器反射加 LayoutContract 建出来(set1 = uHDRColor 来自反射;set0 = Scene 片元
    // 着色器并不引用它,靠 resource_slot_map 的槽位语义补上)。set1 布局之后由
    // templateSetLayout() 取出,供 addPasses 分配 transient DS。
    //
    // 形状收敛进 makeFullscreenTemplate(与 Highlight 的 blur/composite 同预置);
    // PC 大小沿用生成物常量 —— 声明式布局的单一来源。
    GraphicsPipelineTemplate tmpl = makeFullscreenTemplate(
        "Tonemap", pass_gen::kTonemapPassParamsPCTotalSize, /*alpha_blend=*/false);
    tmpl.vertex_shader   = stages->module(0);
    tmpl.fragment_shader = stages->module(1);

    auto pipeline = ctx.pipelineManager().registerGraphicsTemplate(tmpl, stages->infos());
    if (!pipeline)
        return lux::cxx::unexpected(pipeline.error());

    tonemap_pipeline_ = *pipeline;
    hdr_ds_layout_    = ctx.pipelineManager().templateSetLayout(tonemap_pipeline_, 1);
    if (hdr_ds_layout_ == VK_NULL_HANDLE)
        return renderFailure<err::pipeline::ReflectedSetLayoutMissing>(1);
    return {};
}

void TonemapFeature::destroy() noexcept
{
    // hdr_sampler_ 是缓存句柄,不归本特性销毁。
    // §2.2: Layout owned by DescriptorService — no manual destroy.
    hdr_ds_layout_ = VK_NULL_HANDLE;
}

// =========================================================================
//  Per-frame: apply staged params (mirrors Grid3DPassFeature's pending pattern,
//  consumed from RenderScene::beginFrame's onFrameBegin loop).
// =========================================================================

void TonemapFeature::onFrameBegin(const FeatureFrameContext& /*ctx*/)
{
    if (pending_params_)
    {
        pass_params_.scalars = *pending_params_;
        pending_params_.reset();
    }
}

// =========================================================================
//  Render graph passes
// =========================================================================

void TonemapFeature::addPasses(RGBuilder& builder)
{
    namespace gen = pass_gen;   // 生成物(pass_gen/TonemapPassParams.pass.hpp)

    // ---- 作者侧仅剩的活:把名字解析成句柄,填进 PassParams 的资源段 ----
    // (设计稿 L3 的 ViewContext 会接走这一步;标量段由 comm/编辑器路径维护,
    //  这里不碰。)If DeferredLightingFeature is absent, this pass is pruned
    //  by dead-pass elimination.
    pass_params_.hdr_color   = builder.referenceTexture(cfg_.color_input);
    pass_params_.color_out   = builder.referenceTexture(cfg_.color_target);
    pass_params_.hdr_sampler = hdr_sampler_;

    // ---- 以下三处消费同一份 PassParams:描述符、图 I/O、推送常量 ----
    auto lit_tds = gen::createTransientDS(builder, hdr_ds_layout_, pass_params_);

    auto pass = builder.addPass("Tonemap", ERGPassType::GRAPHICS);
    gen::declareGraphIO(pass, pass_params_);
    pass.setPipeline(tonemap_pipeline_)
        // WaterColor is a forward-referenced final-HDR target.  Environment
        // contributions may be attached after the core post-process feature,
        // so declaration order cannot identify its producer.  Without this
        // edge the dependency analyzer quite legally treats Tonemap's early
        // read as preceding WaterComposite's later write and samples an
        // UNDEFINED image.  Missing named passes are ignored, preserving the
        // existing pruning behaviour for profiles without Water.
        .after("WaterComposite")
        .bindSceneDS()                // Set 0: Scene
        .bindTransientDS(1, lit_tds)  // Set 1: HDR sampler (per-view transient)
        .setKernelFn([this](const PassRecordContext& rec) {
            // 标量段整体 memcpy(读活实例 —— HOT 参数下一帧生效,语义不变)
            gen::pushScalars(rec, pass_params_.scalars);

            // Draw fullscreen triangle
            vkCmdDraw(rec.cmd, 3, 1, 0, 0);
        })
        .setKernel("TonemapPass")
        .stage(ERenderStage::PostProcess);   // HDR→LDR scene composite — runs BEFORE the
                                             // Overlay-stage grid/gizmos that draw on top
}

} // namespace lux::render
