#include <lux/engine/render/renderer/features/grid/GridPassFeature.hpp>
#include <lux/engine/render/renderer/features/grid/GridSyncCommands.hpp>

#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/pipeline/PipelinePresets.hpp>  // makeGridTemplate (+ GraphicsPipelineTemplate, vulkan.h)

namespace lux::render
{
    GridPassFeature::GridPassFeature(Config cfg)
        : cfg_(std::move(cfg))
    {
    }

    void GridPassFeature::initAndAttachTo(RenderScene & /*scene*/)
    {
        // Self-contained feature: only the narrow RenderContextView SDK surface —
        // no engine-internal RenderContext / PipelineManager / ShaderResources.
        auto cv = contextView();
        cfg_.vertex_shader   = cv.loadBuiltinShader(EBuiltinShader::GRID_VERT,   cfg_.vertex_shader);
        cfg_.fragment_shader = cv.loadBuiltinShader(EBuiltinShader::GRID_FRAG,   cfg_.fragment_shader);

        auto tmpl = makeGridTemplate();
        tmpl.descriptor_set_count = 1;
        tmpl.vertex_shader   = cv.shaderModule(cfg_.vertex_shader);
        tmpl.fragment_shader = cv.shaderModule(cfg_.fragment_shader);

        std::vector<const lux::rdesc::ShaderInfo *> infos = {
            cv.shaderInfo(cfg_.vertex_shader),
            cv.shaderInfo(cfg_.fragment_shader)
        };
        tmpl.pipeline_layout = cv.buildStandardGraphicsLayout(
            tmpl.descriptor_set_count,
            infos,
            "GridPassLayout"
        );
        grid_handle_ = cv.registerGraphics(tmpl, infos);
    }

    void GridPassFeature::addPasses(RGBuilder &builder)
    {
        auto &pass = 
            builder.addPass("GridPass", ERGPassType::GRAPHICS)
            .write(builder.referenceTexture(cfg_.color_target), lux::common::ETextureRole::COLOR_ATTACHMENT)
            .write(builder.referenceTexture(cfg_.depth_target), lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
            .setPipeline(grid_handle_)
            .bindSceneDS(0)
            .setKernelFn(
                [this](const PassRecordContext &ctx)
                {
                    vkCmdPushConstants(
                        ctx.cmd, 
                        ctx.pipeline_layout,
                        ctx.pc_stage_flags, 
                        8, 
                        sizeof(GridParams), 
                        &grid_params_
                    );
                    vkCmdDraw(ctx.cmd, 3, 1, 0, 0); 
                }
            ).setKernel("GridDraw")
            // Painter order via stage, not .after("SkyboxPass"): the grid overlay
            // must draw after opaque + sky regardless of feature REGISTRATION order.
            // Overlay sorts after Sky/Opaque in the write-after-write tie-break, and
            // unlike .after("SkyboxPass") it still works when no Skybox is present
            // (the old name reference silently did nothing then).
            .stage(ERenderStage::Overlay);
    }

    void GridPassFeature::onFrameBegin(const FeatureFrameContext & /*ctx*/)
    {
        if (pending_grid_params_)
        {
            grid_params_ = *pending_grid_params_;
            pending_grid_params_.reset();
        }
    }

    void GridPassFeature::onSyncSetGridParams(const SetGridParamsCmd &cmd, const FeatureFrameContext & /*ctx*/)
    {
        pending_grid_params_ = cmd.params;
    }

} // namespace lux::render
