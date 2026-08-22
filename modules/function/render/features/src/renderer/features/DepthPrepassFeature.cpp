#include <lux/engine/render/renderer/features/DepthPrepassFeature.hpp>

#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/StandardPipelineLayoutBuilder.hpp>
#include <lux/engine/render/gpu/pipeline/PipelinePresets.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>



namespace lux::render
{

DepthPrepassFeature::DepthPrepassFeature(Config cfg)
    : cfg_(std::move(cfg))
{
}

lux::render::Expected<void> DepthPrepassFeature::initAndAttachTo(RenderScene& /*scene*/){
    auto& ctx = renderContext();
    auto& shaders = ctx.globalRegistry().must<ShaderResources>();
    auto* vs = shaders.get(cfg_.vertex_shader);
    auto* fs = shaders.get(cfg_.fragment_shader);
    // 这个 feature 没有可回填的内置着色器 —— 句柄必须由客户端提供。缺了它建不出管线,
    // 装上一个不产 pass 的 DepthPrepass 等于让上层以为深度预通道在跑。
    if (vs == nullptr || fs == nullptr)
        return renderFailure<err::shader::HandleStale>();

    auto tmpl = makeDepthPrepassTemplate();
    tmpl.descriptor_set_count = 2;
    tmpl.vertex_shader   = vs->module;
    tmpl.fragment_shader = fs->module;

    // Reflected layout (both set 0 Scene and set 1 Instance are engine_set,
    // routed to the shared table).
    tmpl.debug_name = "DepthPrepass";
    const std::array<const lux::rdesc::ShaderInfo*, 2> infos{&vs->info, &fs->info};
    auto pipeline = ctx.pipelineManager().registerGraphicsTemplate(tmpl, infos);
    if (!pipeline)
        return lux::cxx::unexpected(pipeline.error());
    pipeline_handle_ = *pipeline;
    return {};
    }

void DepthPrepassFeature::addPasses(RGBuilder& builder)
{
    // Inert if init() disabled the feature (missing shaders). (medium)
    if (!pipeline_handle_.valid())
        return;

    builder.addPass("DepthPrepass", ERGPassType::GRAPHICS)
        .write(builder.referenceTexture(cfg_.depth_target), lux::render::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
        .setPipeline(pipeline_handle_)
        .bindSceneDS()
        .useEngineSet(EDescriptorSetSlot::Instance)
        .setPhaseMask(1ULL << static_cast<uint8_t>(ECoreRenderPhase::ForwardOpaque))
        .setKernel("DepthPrepass");
}



} // namespace lux::render
