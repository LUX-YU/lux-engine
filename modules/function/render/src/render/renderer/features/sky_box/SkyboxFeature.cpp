#include <lux/engine/render/renderer/features/sky_box/SkyboxFeature.hpp>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/pipeline/StandardPipelineLayoutBuilder.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/pipeline/GeneralDescriptorSetLayout.hpp>

#include <cstring>

namespace lux::render
{

// ==================================================================
// RenderFeature lifecycle
// ==================================================================

SkyboxFeature::SkyboxFeature(Config cfg)
    : cfg_(std::move(cfg))
{
}

void SkyboxFeature::initAndAttachTo(RenderScene& /*scene*/)
{
    init(cfg_);
}

void SkyboxFeature::init(const Config& cfg)
{
    auto& ctx = renderContext();
    auto* shaders = ctx.globalRegistry().find<ShaderResources>();

    // ---- Ensure builtin shader defaults ----
    cfg_.vertex_shader     = ensureBuiltinShader(shaders, cfg_.vertex_shader,     EBuiltinShader::SKYBOX_VERT);
    cfg_.cubemap_fragment   = ensureBuiltinShader(shaders, cfg_.cubemap_fragment,   EBuiltinShader::SKYBOX_CUBEMAP_FRAG);
    cfg_.equirect_fragment  = ensureBuiltinShader(shaders, cfg_.equirect_fragment,  EBuiltinShader::SKYBOX_EQUIRECT_FRAG);

    auto* vs_ptr = cfg_.vertex_shader.valid() ? shaders->get(cfg_.vertex_shader) : nullptr;
    if (!vs_ptr) return;  // vertex shader is mandatory
    auto& vs = *vs_ptr;

    // Common pipeline state (no vertex input — fullscreen triangle)
    auto makeBaseTmpl = [&](VkShaderModule frag_mod)
    {
        GraphicsPipelineTemplate tmpl{};
        tmpl.vertex_shader     = vs.module;
        tmpl.fragment_shader   = frag_mod;
        tmpl.vertex_entry      = "main";
        tmpl.fragment_entry    = "main";
        tmpl.vertex_bindings.clear();
        tmpl.vertex_attributes.clear();
        tmpl.topology          = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        tmpl.polygon_mode      = VK_POLYGON_MODE_FILL;
        tmpl.cull_mode         = VK_CULL_MODE_NONE;
        tmpl.front_face        = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        tmpl.depth_test_enable  = VK_TRUE;
        tmpl.depth_write_enable = VK_FALSE;
        tmpl.depth_compare_op   = VK_COMPARE_OP_LESS_OR_EQUAL;
        tmpl.blend_enable       = VK_FALSE;
        tmpl.color_write_mask   = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        tmpl.use_dynamic_viewport = true;
        tmpl.use_dynamic_scissor  = true;
        return tmpl;
    };

    // --- Equirectangular pipeline (optional) ---
    if (cfg.equirect_fragment.valid())
    {
        auto* equi_ptr = shaders->get(cfg.equirect_fragment);
        if (equi_ptr)
        {
            auto& equi_fs = *equi_ptr;
            auto tmpl = makeBaseTmpl(equi_fs.module);
            tmpl.descriptor_set_count = 3;
            std::vector<const lux::rdesc::ShaderInfo*> equi_infos = { &vs.info, &equi_fs.info };
            tmpl.pipeline_layout = buildStandardGraphicsPipelineLayout(
                ctx,
                tmpl.descriptor_set_count,
                equi_infos,
                "SkyboxEquirectLayout").value();
            equirect_handle_ = ctx.pipelineManager().registerGraphicsTemplate(tmpl, equi_infos).value();
        }
    }

    // --- Cubemap pipeline (optional) ---
    if (cfg.cubemap_fragment.valid())
    {
        auto* cube_ptr = shaders->get(cfg.cubemap_fragment);
        if (cube_ptr)
        {
            auto& cube_fs = *cube_ptr;
            auto tmpl = makeBaseTmpl(cube_fs.module);
            tmpl.descriptor_set_count = 3;
            std::vector<const lux::rdesc::ShaderInfo*> cube_infos = { &vs.info, &cube_fs.info };
            tmpl.pipeline_layout = buildStandardGraphicsPipelineLayout(
                ctx,
                tmpl.descriptor_set_count,
                cube_infos,
                "SkyboxCubemapLayout").value();
            cubemap_handle_ = ctx.pipelineManager().registerGraphicsTemplate(tmpl, cube_infos).value();
        }
    }
}

void SkyboxFeature::addPasses(RGBuilder& builder)
{
    auto& ctx = renderContext();

    // When the deferred pipeline is active, Skybox writes into the color
    // color target so that Tonemap can process it.  Forward-only path
    // falls back to the swapchain backbuffer.
    auto color = builder.findResource(cfg_.color_input);
    auto color_target = color ? color : builder.referenceTexture(cfg_.color_input);

    const bool has_equirect = (equirect_handle_ != kInvalidPipelineHandle);
    const bool has_cubemap = (cubemap_handle_ != kInvalidPipelineHandle);
    if (!has_equirect && !has_cubemap)
        return;

    const GraphicsPipelineHandle base_pipeline =
        has_equirect ? equirect_handle_ : cubemap_handle_;
    const uint32_t equirect_variant = has_equirect ? 0u : ~0u;
    const uint32_t cubemap_variant = has_cubemap
        ? (has_equirect ? 1u : 0u)
        : ~0u;

    auto pass = builder.addPass("SkyboxPass", ERGPassType::GRAPHICS)
        .write(color_target, lux::common::ETextureRole::COLOR_ATTACHMENT)
        .write(builder.referenceTexture(cfg_.depth_target), lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
        .setPipeline(base_pipeline)
        .stage(ERenderStage::Sky);   // after opaque, before overlays (Grid/Gizmo)

    if (has_equirect && has_cubemap)
        pass.addPipeline(cubemap_handle_);

    pass.bindSceneDS(0)
        .bindImmutableDS(1, renderScene().sceneRegistry().descriptorSetOf<InstanceResources>())
        .bindImmutableDS(2, ctx.globalRegistry().descriptorSetOf<TextureResources>())
        .setKernelFn(
            [this, has_equirect, has_cubemap, equirect_variant, cubemap_variant]
            (const PassRecordContext& ctx)
            {
                if (active_mode_ == ActiveMode::NONE)
                    return;

                uint32_t push_index = 0u;

                if (active_mode_ == ActiveMode::EQUIRECT)
                {
                    if (!has_equirect)
                        return;
                    const bool bound = (equirect_variant == 0u)
                        ? ctx.bindPassPipeline()
                        : ctx.bindPipelineVariant(equirect_variant);
                    if (!bound)
                        return;
                    push_index = equirect_bindless_index_;
                }
                else // ActiveMode::CUBEMAP
                {
                    if (!has_cubemap)
                        return;
                    const bool bound = (cubemap_variant == 0u)
                        ? ctx.bindPassPipeline()
                        : ctx.bindPipelineVariant(cubemap_variant);
                    if (!bound)
                        return;
                    push_index = cubemap_bindless_index_;
                }

                struct PC { uint32_t skybox_index; } pc{push_index};
                vkCmdPushConstants(
                    ctx.cmd,
                    ctx.pipeline_layout,
                    ctx.pc_stage_flags,
                    8,
                    sizeof(PC),
                    &pc);
                vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
            })
        .setKernel("SkyboxDraw");
}


// ==================================================================
// Per-frame command apply (render thread)
// ==================================================================

void SkyboxFeature::onFrameBegin(const FeatureFrameContext& /*ctx*/)
{
    // Nothing pending → nothing to do.
    if (!pending_cmd_) return;

    bool applied = std::visit([this](auto&& c) -> bool {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, SetEquirectHandleCmd>)
        {
            if (applyEquirectangularHandle(c.texture)) {
                return true;
            }
            return false;
        }
        else if constexpr (std::is_same_v<T, SetCubemapHandlesCmd>)
        {
            if (applyCubemapHandles(c.cube)) {
                return true;
            }
            return false;
        }
    }, *pending_cmd_);

    if (applied)
        pending_cmd_.reset();
}

void SkyboxFeature::onSetEquirectHandle(
    const SetEquirectHandleCmd& cmd,
    const FeatureFrameContext& /*ctx*/) noexcept
{
    pending_cmd_ = cmd;
}

void SkyboxFeature::onSetCubemapHandles(
    const SetCubemapHandlesCmd& cmd,
    const FeatureFrameContext& /*ctx*/) noexcept
{
    pending_cmd_ = cmd;
}

// ==================================================================
// Handle-based GPU operations
// ==================================================================

SkyboxFeature::~SkyboxFeature()
{
    // Both paths are managed by TextureResources — nothing to clean up.
}

// ==================================================================
// Handle-based GPU operations
// ==================================================================

bool SkyboxFeature::applyEquirectangularHandle(RTextureHandle texture)
{
    uint32_t bindless = 0;
    equirect_bindless_index_ = texture.index;
    active_mode_             = ActiveMode::EQUIRECT;
    return true;
}

bool SkyboxFeature::applyCubemapHandles(RTextureHandle cube)
{
    // The handle-based cubemap path expects a SINGLE cube-texture handle stored in cube.
    cubemap_bindless_index_ = cube.index;
    active_mode_            = ActiveMode::CUBEMAP;
    return true;
}

} // namespace lux::render
