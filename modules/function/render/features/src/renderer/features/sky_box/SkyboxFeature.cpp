#include <array>
#include <lux/engine/render/renderer/features/sky_box/SkyboxFeature.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/function/render/client/features/sky_box/SkyboxOperation.hpp>               // kSkyboxPassName
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/resources/TextureResources.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/StandardPipelineLayoutBuilder.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>
#include <lux/engine/render/graph/RGCompiledGraph.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>

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

lux::render::Expected<void> SkyboxFeature::initAndAttachTo(RenderScene& /*scene*/)
{
    return init(cfg_);
}

Expected<void> SkyboxFeature::init(const Config& cfg)
{
    auto& ctx = renderContext();
    auto& shaders = ctx.globalRegistry().must<ShaderResources>();

    // 空句柄回填内置默认。
    const std::array backfill{
        ShaderStageSlot{EBuiltinShader::SKYBOX_VERT,          &cfg_.vertex_shader},
        ShaderStageSlot{EBuiltinShader::SKYBOX_CUBEMAP_FRAG,  &cfg_.cubemap_fragment},
        ShaderStageSlot{EBuiltinShader::SKYBOX_EQUIRECT_FRAG, &cfg_.equirect_fragment}};
    if (auto filled = resolveShaderStages(shaders, backfill); !filled)
        return filled;

    // 顶点着色器是必需的 —— 上面的回填之后仍取不到,说明内置项本身有问题。
    if (shaders.get(cfg_.vertex_shader) == nullptr)
        return renderFailure<err::shader::HandleStale>();

    // ── Domain-merge switch: this feature is the first pipeline switched over ──
    //
    //  How it works: patch the shader's SPIR-V according to the engine's
    //  relocation table, move the reflection metadata along with it, then
    //  register as usual — everything downstream follows automatically:
    //    - Layout routing recognizes by contract that uTex belongs to Texture
    //      (canonical set2), and picks up the shared table's Texture layout,
    //      which has the same structure as the BINDLESS domain layout (that
    //      domain holds only the texture table);
    //    - resource_slot_map is derived from the post-relocation slot table, so
    //      bindImmutableDS(Texture, ...) automatically resolves to the new slot 1.
    //  So not a single line at the binding call site needs to change — that is
    //  exactly the payoff from having switched bindings to logical identity
    //  earlier.
    //
    //  Why only this one pipeline can be switched: both the layout and the
    //  shader module are per-pipeline independent, and the domain set is
    //  already dual-written with data identical to the old set — the switched
    //  pipeline reads the new location, the unswitched ones read the old
    //  location, and both paths read the same data, so they can coexist.
    //
    //  整条管线的全部 stage 一次过 preparePipelineStages:任一 stage 切换失败即整体
    //  报错(不会出现一半搬过去一半没搬的中间态),返回的模块与反射是拷贝,此后不受
    //  容器扩容影响。
    //
    //  这里原先是逐个调 mergedOrOriginal 再统一 get(),并在注释里叮嘱「句柄先行」——
    //  那条不变量踩过一次:持有顶点着色器指针、逐个切换片元着色器,中途一次扩容
    //  重分配就让那个指针悬垂。批量入口让人写不出错误的顺序。
    //
    //  两个片元 stage 都写成可选的(收集实际存在的那些再一起过),但要注意:
    //  上面的 resolveShaderStages 会把空句柄按内置回填,所以走到这里时两个
    //  片元句柄**都已经是有效的** —— 下面这两个标志今天恒为真,分支是为了让
    //  「配置里只给一种」这个未来形态不必重写这段。
    lux::cxx::SmallVector<ShaderHandle, 3> wanted{cfg_.vertex_shader};
    const bool want_equirect = cfg.equirect_fragment.isValid();
    const bool want_cubemap  = cfg.cubemap_fragment.isValid();
    if (want_equirect) wanted.push_back(cfg.equirect_fragment);
    if (want_cubemap)  wanted.push_back(cfg.cubemap_fragment);

    auto prepared = shaders.preparePipelineStages(std::span<const ShaderHandle>{wanted.data(), wanted.size()});
    if (!prepared)
        return lux::cxx::unexpected(prepared.error());

    // 下标随收集顺序:0 = 顶点,之后依次是存在的那些片元。
    const std::size_t equi_index = 1;
    const std::size_t cube_index = want_equirect ? 2 : 1;

    // Common pipeline state (no vertex input — fullscreen triangle)
    auto makeBaseTmpl = [&](VkShaderModule frag_mod)
    {
        GraphicsPipelineTemplate tmpl{};
        tmpl.vertex_shader     = prepared->module(0);
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

    // --- Equirectangular pipeline (optional; reflected layout) ---
    // Leaving layout empty makes registerGraphicsTemplate build it from
    // reflection + contract: set0 (uViews, GLOBAL) and set2 (uTex/uCubeTex,
    // BINDLESS) route to the engine's shared layout; set1 is a hole (the
    // shader doesn't use Instance) — an empty layout placeholder there is legal
    // since skybox's draw never accesses it.
    // 「可选」说的是**哪一种**天空盒由配置决定,不是「建不出来也没关系」——
    // 配置里写了 equirect 却建不出管线,天空就是黑的,而上层收到的是装配成功。
    if (want_equirect)
    {
        auto tmpl = makeBaseTmpl(prepared->module(equi_index));
        tmpl.debug_name = "SkyboxEquirect";
        tmpl.descriptor_set_count = 3;
        const std::array<const lux::rdesc::ShaderInfo*, 2> equi_infos{
            &prepared->info(0), &prepared->info(equi_index)};
        auto handle = ctx.pipelineManager().registerGraphicsTemplate(tmpl, equi_infos);
        if (!handle)
            return lux::cxx::unexpected(handle.error());
        equirect_handle_ = *handle;
    }

    // --- Cubemap pipeline (optional; reflected layout, same as above) ---
    if (want_cubemap)
    {
        auto tmpl = makeBaseTmpl(prepared->module(cube_index));
        tmpl.debug_name = "SkyboxCubemap";
        tmpl.descriptor_set_count = 3;
        const std::array<const lux::rdesc::ShaderInfo*, 2> cube_infos{
            &prepared->info(0), &prepared->info(cube_index)};
        auto handle = ctx.pipelineManager().registerGraphicsTemplate(tmpl, cube_infos);
        if (!handle)
            return lux::cxx::unexpected(handle.error());
        cubemap_handle_ = *handle;
    }

    return {};
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

    auto pass = builder.addPass(kSkyboxPassName, ERGPassType::GRAPHICS)
        .write(color_target, lux::common::ETextureRole::COLOR_ATTACHMENT)
        .write(builder.referenceTexture(cfg_.depth_target), lux::common::ETextureRole::DEPTH_STENCIL_ATTACHMENT)
        .setPipeline(base_pipeline)
        .stage(ERenderStage::Sky);   // after opaque, before overlays (Grid/Gizmo)

    if (has_equirect && has_cubemap)
        pass.addPipeline(cubemap_handle_);

    // No longer binding Instance: skybox's shader never uses it at all (the old
    // code bound it only because the unmerged layout had a hole at set1 that
    // got filled in by convention at record time). After the domain merge
    // reorders the slots, continuing to bind it would bind the Instance set
    // onto the texture table's slot — verified by testing to trigger
    // VUID-00358.
    pass.bindSceneDS()
        .bindImmutableDS(EDescriptorSetSlot::Texture, ctx.globalRegistry().descriptorSetOf<TextureResources>())
        .setKernelFn(
            [this, has_equirect, has_cubemap, equirect_variant, cubemap_variant]
            (const PassRecordContext& ctx)
            {
                ++pass_visits_;
                if (active_mode_ == ActiveMode::NONE)
                {
                    ++inactive_pass_visits_;
                    return;
                }

                uint32_t push_index = 0u;

                if (active_mode_ == ActiveMode::EQUIRECT)
                {
                    if (!has_equirect)
                        return;
                    const bool bound = (equirect_variant == 0u)
                        ? ctx.bindPassPipeline()
                        : ctx.bindPipelineVariant(equirect_variant);
                    if (!bound)
                    {
                        ++pipeline_bind_failures_;
                        return;
                    }
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
                    {
                        ++pipeline_bind_failures_;
                        return;
                    }
                    push_index = cubemap_bindless_index_;
                }

                struct PC
                {
                    uint32_t skybox_index;
                    float rotation_radians;
                    float intensity;
                } pc{push_index, rotation_radians_, intensity_};
                vkCmdPushConstants(
                    ctx.cmd,
                    ctx.pipeline_layout,
                    ctx.pc_stage_flags,
                    8,
                    sizeof(PC),
                    &pc);
                vkCmdDraw(ctx.cmd, 3, 1, 0, 0);
                ++draws_;
            })
        .setKernel("SkyboxDraw");
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

bool SkyboxFeature::applyEquirectangularHandle(
    RTextureHandle texture,
    float rotation_radians,
    float intensity)
{
    // A null handle is the DISABLE signal (the client's SkyboxProxy::setEquirect with a
    // null texture — e.g. the SkyboxSubsystem's teardown clear): drop to NONE so the pass
    // kernel early-outs, rather than binding a stale/garbage bindless index into a
    // sampler2D[] slot the texture pool may have since reused.
    if (texture.isNull())
    {
        active_mode_ = ActiveMode::NONE;
        return true;
    }
    equirect_bindless_index_ = texture.index;
    rotation_radians_ = rotation_radians;
    intensity_ = intensity;
    active_mode_             = ActiveMode::EQUIRECT;
    return true;
}

bool SkyboxFeature::applyCubemapHandles(
    RTextureHandle cube,
    float rotation_radians,
    float intensity)
{
    if (cube.isNull())   // disable (see applyEquirectangularHandle)
    {
        active_mode_ = ActiveMode::NONE;
        return true;
    }
    // The handle-based cubemap path expects a SINGLE cube-texture handle stored in cube.
    cubemap_bindless_index_ = cube.index;
    rotation_radians_ = rotation_radians;
    intensity_ = intensity;
    active_mode_            = ActiveMode::CUBEMAP;
    return true;
}

SkyboxStatsReply SkyboxFeature::stats() const noexcept
{
    const auto bindless_index = active_mode_ == ActiveMode::CUBEMAP
        ? cubemap_bindless_index_
        : equirect_bindless_index_;
    return SkyboxStatsReply{
        static_cast<std::uint32_t>(active_mode_),
        bindless_index,
        pass_visits_,
        draws_,
        inactive_pass_visits_,
        pipeline_bind_failures_,
        intensity_,
        rotation_radians_};
}

} // namespace lux::render
