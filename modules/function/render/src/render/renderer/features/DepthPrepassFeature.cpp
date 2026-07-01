#include <lux/engine/render/renderer/features/DepthPrepassFeature.hpp>

#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/pipeline/StandardPipelineLayoutBuilder.hpp>
#include <lux/engine/render/pipeline/PipelinePresets.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>

#include <iostream>


namespace lux::render
{

DepthPrepassFeature::DepthPrepassFeature(Config cfg)
    : cfg_(std::move(cfg))
{
}

lux::render::Expected<void> DepthPrepassFeature::initAndAttachTo(RenderScene& /*scene*/){
    auto& ctx = renderContext();
    auto* shaders = ctx.globalRegistry().find<ShaderResources>();
    auto* vs = shaders ? shaders->get(cfg_.vertex_shader)   : nullptr;
    auto* fs = shaders ? shaders->get(cfg_.fragment_shader) : nullptr;
    if (vs == nullptr || fs == nullptr)
    {
        // Created via protocol with default (empty) shader handles —
        // ShaderResources::get returns nullptr for invalid handles, and the old
        // code dereferenced it unconditionally, crashing the render thread. There
        // is no DEPTH_PREPASS builtin to fall back on, so leave pipeline_handle_
        // invalid and skip construction; addPasses guards on validity, making the
        // feature inert rather than fatal. (medium)
        std::cerr << "[DepthPrepassFeature] missing vertex/fragment shader handle; "
                     "feature disabled (no pass will be emitted)\n";
        return {};
    }

    auto tmpl = makeDepthPrepassTemplate();
    tmpl.descriptor_set_count = 2;
    tmpl.vertex_shader   = vs->module;
    tmpl.fragment_shader = fs->module;

    std::vector<const lux::rdesc::ShaderInfo*> infos = {
        &vs->info, &fs->info };
    tmpl.pipeline_layout = buildStandardGraphicsPipelineLayout(
        ctx,
        tmpl.descriptor_set_count,
        infos,
        "DepthPrepassLayout").value();
    pipeline_handle_ = ctx.pipelineManager().registerGraphicsTemplate(tmpl, infos).value();
    return {};
    }

void DepthPrepassFeature::addPasses(RGBuilder& builder)
{
    // Inert if init() disabled the feature (missing shaders). (medium)
    if (!pipeline_handle_.valid())
        return;

    builder.addPass("DepthPrepass", ERGPassType::GRAPHICS)
        .write(builder.referenceTexture(cfg_.depth_target), lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
        .setPipeline(pipeline_handle_)
        .bindSceneDS(0)
        .bindImmutableDS(1, renderScene().sceneRegistry().descriptorSetOf<InstanceResources>())
        .setPhaseMask(1ULL << static_cast<uint8_t>(ECoreRenderPhase::ForwardOpaque))
        .setKernel("DepthPrepass")
        .forcePassPipeline();         // always use depth_only pipeline; ignore per-material handles
}



} // namespace lux::render
