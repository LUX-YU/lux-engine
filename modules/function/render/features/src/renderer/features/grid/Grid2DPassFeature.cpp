#include <array>
#include <lux/engine/render/renderer/features/grid/Grid2DPassFeature.hpp>

#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/gpu/pipeline/PipelinePresets.hpp>  // makeGridTemplate

namespace lux::render
{
    Grid2DPassFeature::Grid2DPassFeature(Config cfg)
        : cfg_(std::move(cfg))
    {
    }

    lux::render::Expected<void> Grid2DPassFeature::initAndAttachTo(RenderScene& /*scene*/)
    {
        auto cv = contextView();

        // 解析内置默认 + 域合并切换 + 取模块反射,一次做完。引用 uViews 的管线
        //(grid2d.frag 用它)必须带域合并标记,否则注册被拒。
        const std::array stage_requests{
            RenderContextView::PipelineStageDesc{EBuiltinShader::GRID_VERT,   cfg_.vertex_shader},
            RenderContextView::PipelineStageDesc{EBuiltinShader::GRID2D_FRAG, cfg_.fragment_shader}};

        auto stages = cv.preparePipelineStages(stage_requests);
        if (!stages)
            return lux::cxx::unexpected(stages.error());

        // 3D 网格模板去掉深度 —— 2D 路径没有深度附件。
        auto tmpl = makeGridTemplate();
        tmpl.depth_test_enable  = VK_FALSE;
        tmpl.depth_write_enable = VK_FALSE;
        tmpl.descriptor_set_count = 1;
        tmpl.vertex_shader   = stages->module(0);
        tmpl.fragment_shader = stages->module(1);

        // 布局留空,由反射建出来 —— 这条管线只用 Scene set,契约会把它路由回同一份
        // 引擎共享布局,与手写等价。
        tmpl.debug_name = "Grid2DPass";
        pipeline_ = cv.registerGraphics(tmpl, stages->infos());
        return {};
    }

    void Grid2DPassFeature::addPasses(RGBuilder& builder)
    {
        // TWO passes are registered, but only one of them actually draws each
        // frame (which one is selectable via display priority), keeping the
        // graph topology stable regardless of the choice.
        const auto record = [this](bool for_on_top)
        {
            return [this, for_on_top](const PassRecordContext& ctx)
            {
                if ((params_.onTop != 0) != for_on_top)
                    return;   // the other pass owns this frame's draw
                vkCmdPushConstants(
                    ctx.cmd, ctx.pipeline_layout, ctx.pc_stage_flags,
                    8, sizeof(Grid2DParams), &params_);
                vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
            };
        };

        // Under: Transparent (5000) — before the Canvas2D content pass
        // (Overlay, 7000), so the grid sits beneath sprites/tilemaps/fields.
        builder.addPass("Grid2DUnder", ERGPassType::GRAPHICS)
            .write(builder.referenceTexture(cfg_.color_target),
                   lux::common::ETextureRole::COLOR_ATTACHMENT)
            .setPipeline(pipeline_)
            .bindSceneDS()
            .setKernelFn(record(false))
            .setKernel("Grid2DUnderDraw")
            .stage(ERenderStage::Transparent);

        // Over: Overlay — the plan registers Grid2D AFTER Canvas2D, so the
        // write-after-write tie-break orders this pass after the content.
        builder.addPass("Grid2DOver", ERGPassType::GRAPHICS)
            .write(builder.referenceTexture(cfg_.color_target),
                   lux::common::ETextureRole::COLOR_ATTACHMENT)
            .setPipeline(pipeline_)
            .bindSceneDS()
            .setKernelFn(record(true))
            .setKernel("Grid2DOverDraw")
            .stage(ERenderStage::Overlay);
    }

    void Grid2DPassFeature::onFrameBegin(const FeatureFrameContext& /*ctx*/)
    {
        if (pending_params_)
        {
            params_ = *pending_params_;
            pending_params_.reset();
        }
    }

} // namespace lux::render
