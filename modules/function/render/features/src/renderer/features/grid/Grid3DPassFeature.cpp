#include <array>
#include <lux/engine/render/renderer/features/grid/Grid3DPassFeature.hpp>

#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/gpu/pipeline/PipelinePresets.hpp>  // makeGridTemplate (+ GraphicsPipelineTemplate, vulkan.h)

namespace lux::render
{
    Grid3DPassFeature::Grid3DPassFeature(Config cfg)
        : cfg_(std::move(cfg))
    {
    }

    lux::render::Expected<void> Grid3DPassFeature::initAndAttachTo(RenderScene & /*scene*/){
        // Self-contained feature: only the narrow RenderContextView SDK surface —
        // no engine-internal RenderContext / PipelineManager / ShaderResources.
        auto cv = contextView();
    // 解析内置默认 + 域合并切换 + 取模块反射,一次做完。引用引擎契约资源(本 vert
    // 用 uViews)的管线必须带域合并标记,否则注册被拒。
    const std::array stage_requests{
        RenderContextView::PipelineStageDesc{EBuiltinShader::GRID_VERT, cfg_.vertex_shader},
        RenderContextView::PipelineStageDesc{EBuiltinShader::GRID_FRAG, cfg_.fragment_shader}};

    auto stages = cv.preparePipelineStages(stage_requests);
    if (!stages)
        return lux::cxx::unexpected(stages.error());
        auto tmpl = makeGridTemplate();
        tmpl.descriptor_set_count = 1;
        tmpl.vertex_shader   = stages->module(0);
        tmpl.fragment_shader = stages->module(1);
        // Layout left empty here; it's built via reflection instead — this
        // pipeline only uses the Scene set, and the contract routes it back
        // to the same shared engine layout, equivalent to building it by hand.
        tmpl.debug_name = "Grid3DPass";
        grid_handle_ = cv.registerGraphics(tmpl, stages->infos());
        return {};
    }

    void Grid3DPassFeature::addPasses(RGBuilder &builder)
    {
        auto &pass =
            builder.addPass("Grid3DPass", ERGPassType::GRAPHICS)
            .write(builder.referenceTexture(cfg_.color_target), lux::common::ETextureRole::COLOR_ATTACHMENT)
            .write(builder.referenceTexture(cfg_.depth_target), lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
            .setPipeline(grid_handle_)
            .bindSceneDS()
            .setKernelFn(
                [this](const PassRecordContext &ctx)
                {
                    vkCmdPushConstants(
                        ctx.cmd,
                        ctx.pipeline_layout,
                        ctx.pc_stage_flags,
                        8,
                        sizeof(Grid3DParams),
                        &grid_params_
                    );
                    vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
                }
            ).setKernel("Grid3DDraw")
            // Painter order via stage, not .after(kSkyboxPassName): the grid overlay
            // must draw after opaque + sky regardless of feature REGISTRATION order.
            // Overlay sorts after Sky/Opaque in the write-after-write tie-break, and
            // unlike .after(kSkyboxPassName) it still works when no Skybox is present
            // (the old name reference silently did nothing then).
            .stage(ERenderStage::Overlay);
    }

    void Grid3DPassFeature::onFrameBegin(const FeatureFrameContext & /*ctx*/)
    {
        if (pending_grid_params_)
        {
            grid_params_ = *pending_grid_params_;
            pending_grid_params_.reset();
        }
    }

} // namespace lux::render
