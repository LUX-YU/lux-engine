/**
 * @file SkinningFeature.cpp
 * @brief GPU pre-skinning compute feature.
 */

#include <lux/engine/render/renderer/features/skinning/SkinningFeature.hpp>

#include <array>
#include <cstdint>

#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/RGResourceTypes.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp> // resolveShaderStage
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/resources/vertex/SkinningResources.hpp>
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>
#include <lux/engine/render/resources/vertex/VertexProduction.hpp> // producer registry + typed RG name
#include <lux/engine/render/gpu/pipeline/VertexLayoutRegistry.hpp> // vertex-layout SSOT
#include <lux/engine/render/gpu/pipeline/VertexLayoutSpec.hpp>     // appendVertexLayoutSpecs

#include <span>

#include <lux/engine/description/Vertex.hpp> // sizeof(rdesc::Vertex)

namespace lux::render
{
    namespace
    {
        // Batched skinning push constant. Per-instance fields moved
        // into the dispatch-params SSBO (set 0 binding 2); the shader uses
        // `gl_WorkGroupID.x` to binary-search its dispatch entry. Only the
        // total dispatch count remains, so the shader knows where to stop.
        struct SkinPC
        {
            std::uint32_t dispatch_count;
        };
        static_assert(sizeof(SkinPC) == 4);
    }

    SkinningFeature::SkinningFeature(Config cfg) : RenderFeature(RenderFeature::Config{.name = "Skinning"}), cfg_(cfg)
    {
    }

    SkinningResources* SkinningFeature::skinningResources() noexcept
    {
        return renderScene().sceneRegistry().find<SkinningResources>();
    }

    lux::render::Expected<void> SkinningFeature::initAndAttachTo(RenderScene& /*scene*/)
    {
        return init(cfg_);
    }

    Expected<void> SkinningFeature::init(const Config& cfg)
    {
        auto& ctx = renderContext();

        // Vertex-layout registry (SSOT). Layout 0 = full (skin INPUT read),
        // layout 1 = lean geometry-only (skin OUTPUT write / what _vp shaders read).
        auto& vlr = ctx.globalRegistry().must<VertexLayoutRegistry>();

        // --- 1. SkinningResources (per-scene): bone palette + output pool ---
        // 蒙皮的输出要挂进无绑定顶点池才画得出来。池不在 = 装配顺序错了
        // (StandardMeshStack 必须先装),不是「蒙皮可选」。
        auto* vpr = renderScene().sceneRegistry().find<VertexPoolRegistry>();
        if (vpr == nullptr)
            return renderFailure<err::feature::DependencyMissing>(
                encodeFeatureType(::lux::render::featureId("lux.render.skinning.v1")),
                encodeFeatureType(::lux::render::featureId("lux.render.mesh_stack.v1"))
            );

        if (!renderScene().sceneRegistry().find<SkinningResources>())
            renderScene().sceneRegistry().emplace<SkinningResources>();

        auto* skin = skinningResources();
        {
            SkinningResources::InitInfo si{};
            si.device_context = &ctx.deviceContext();
            si.vertex_pool_registry = vpr;
            // The skinning INPUT pool (global VBO) is owned by the per-scene
            // StaticVertexPoolSet; SkinningResources only owns the OUTPUT pool +
            // bone palettes. The dispatch caller passes the input pool id
            // explicitly.
            // The transient OUTPUT pool uses the FULL layout 0 (22 floats /
            // 88 B, same as global VBO). The kernel still writes only the
            // geometry prefix; the bone tail is zero-padded. This unifies the
            // stride between skinned output and static VBO so that all
            // _vp.vert pipelines can share ONE spec set.
            // TransientVertexSource sizes its allocation (and the per-draw
            // vertex_base it emits) by this stride, so it MUST match the
            // OUTPUT stride written by skin_compute.comp.
            si.layout_id = kDefaultVertexLayoutId;
            si.vertex_stride = (vlr.hasLayout(kDefaultVertexLayoutId))
                                   ? vlr.fetchLayout(kDefaultVertexLayoutId).stride
                                   : makeDefaultVertexLayout().stride; // 88 B fallback
            si.max_bones = cfg.max_bones;
            si.output_pool_bytes = cfg.output_pool_bytes;
            if (!skin->init(si))
                return renderFailure<err::feature::ResourceInitFailed>();
            // 批装不下时的自发上报去处(没有调用方可以处置这类每帧仲裁结果)。
            skin->setErrorSink(renderContext().errorSink());
        }

        // --- 2+3. Compute pipeline (reflected layout) ---
        // set0 (the skin trio: palette/out-pool/dispatch, pass-local) is built via
        // reflection; set1 = the bindless vertex pool, routed to the engine's
        // shared layout via its contract canonical slot (here it lands at set1
        // through a macro override, canonical=7 — the routing rule absorbs the
        // drift). The skin set's per-instance allocation uses the layout object
        // retrieved from computeSetLayout(handle, 0).
        {
            auto& shaders = ctx.globalRegistry().must<ShaderResources>();
            // Self-load the GPU pre-skinning kernel from the builtin registry
            // if the caller didn't supply one (the feature factory adds
            // SkinningFeature without wiring shaders explicitly).
            const auto resolved = resolveShaderStage(shaders, cfg.compute_shader, EBuiltinShader::SKIN_COMPUTE_COMP);
            if (!resolved)
                return lux::cxx::unexpected(resolved.error());
            // Compute pipeline switch: the vertex pool moves from the macro-
            // overridden set1 (canonical 7) into the [+23, +39) range of FEATURE
            // domain slot 2; set0's skin data stays private and untouched. At
            // record time, the VertexPool logical binding collapses into a
            // domain-set binding through the compute path.
            const std::array skin_stages{*resolved};
            auto prepared = shaders.preparePipelineStages(skin_stages);
            if (!prepared)
                return lux::cxx::unexpected(prepared.error());
            const ShaderHandle compute = prepared->handle(0);

            // Feed the skin kernel its vertex layouts as COMPUTE spec
            // constants. Two blocks: INPUT (110-block) = layout 0 (the kernel
            // reads un-skinned verts incl. bone ids/weights); OUTPUT
            // (120-block) ALSO = layout 0 — the kernel writes the geometry
            // prefix and zero-pads the bone tail so the skinned pool's stride
            // matches the global VBO's, enabling single-pipeline-per-family.
            lux::cxx::SmallVector<GraphicsPipelineTemplate::ShaderSpecializationValue, 4> skin_specs;
            if (vlr.hasLayout(kDefaultVertexLayoutId))
            {
                const auto& full = vlr.fetchLayout(kDefaultVertexLayoutId);
                appendVertexLayoutSpecs(skin_specs, full, VK_SHADER_STAGE_COMPUTE_BIT, kVtxSpecInputBase);
                appendVertexLayoutSpecs(skin_specs, full, VK_SHADER_STAGE_COMPUTE_BIT, kVtxSpecOutputBase);
            }
            const std::span<const GraphicsPipelineTemplate::ShaderSpecializationValue> specs{
                skin_specs.data(),
                skin_specs.size()};
            auto pipeline = ctx.pipelineManager().registerComputePipelineReflected(
                prepared->module(0),
                prepared->info(0),
                "SkinningCompute",
                specs
            );
            if (!pipeline)
                return lux::cxx::unexpected(pipeline.error());
            compute_pipeline_ = *pipeline;

            const VkDescriptorSetLayout skin_layout = ctx.pipelineManager().computeSetLayout(compute_pipeline_, 0);
            if (skin_layout == VK_NULL_HANDLE)
                return renderFailure<err::pipeline::ReflectedSetLayoutMissing>(0u);
            for (auto& ds : skin_ds_)
                ds = renderScene().descriptorArena().allocate(skin_layout);
        }

        // --- 4. Write each per-FIF skin descriptor set:
        //         binding 0 = bone palette (per-FIF ring slot),
        //         binding 1 = skinned-vertex output pool (stable),
        //         binding 2 = dispatch-params SSBO (per-FIF ring slot).
        //         The INPUT is read from the bindless pool (set 1), so it is
        //         NOT in this set anymore. ---
        skin_res_ = skin; // cache for resolveSkinDS
        {
            const VkBuffer out_pool = skin->outputPool().buffer();
            if (out_pool == VK_NULL_HANDLE)
                return renderFailure<err::feature::ResourceInitFailed>();

            for (std::uint32_t fi = 0; fi < kMaxFramesInFlight; ++fi)
            {
                const VkBuffer palette = skin->bonePaletteBuffer(fi);
                const VkBuffer disp_buf = skin->dispatchParamsBuffer(fi);
                if (palette == VK_NULL_HANDLE || disp_buf == VK_NULL_HANDLE || skin_ds_[fi] == VK_NULL_HANDLE)
                    return renderFailure<err::feature::ResourceInitFailed>();

                std::array<VkDescriptorBufferInfo, 3> bi{};
                bi[0] = {palette, 0, VK_WHOLE_SIZE};
                bi[1] = {out_pool, 0, VK_WHOLE_SIZE};
                bi[2] = {disp_buf, 0, VK_WHOLE_SIZE};

                std::array<VkWriteDescriptorSet, 3> w{};
                for (std::uint32_t i = 0; i < 3; ++i)
                {
                    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    w[i].dstSet = skin_ds_[fi];
                    w[i].dstBinding = i;
                    w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    w[i].descriptorCount = 1;
                    w[i].pBufferInfo = &bi[i];
                }
                vkUpdateDescriptorSets(
                    ctx.deviceContext().logicalDevice(),
                    static_cast<uint32_t>(w.size()),
                    w.data(),
                    0,
                    nullptr
                );
            }
        }

        // --- 5. Announce this producer to the per-scene registry so mesh-draw
        //         consumers order their passes after the skinning compute pass
        //         (replaces the magic-string cross-feature link). The
        //         producer's lifetime parity with SkinningResources (both owned
        //         by the scene) means no unpublish is needed here. ---
        if (auto* production = renderScene().sceneRegistry().find<VertexProductionRegistry>())
            production->publish(EVertexProductKind::Skinning);

        return {};
    }

    void SkinningFeature::addPasses(RGBuilder& builder)
    {
        auto* skin = skinningResources();
        if (!skin || !skin->initialized() || skin_ds_[0] == VK_NULL_HANDLE)
            return;

        // set 1 = bindless vertex pool: the compute reads its INPUT vertices from
        // it. Required by the compute pipeline layout, so bail if absent.
        auto* vpr = renderScene().sceneRegistry().find<VertexPoolRegistry>();
        if (!vpr || !vpr->isInitialized())
            return;

        // IMPORTANT: addPasses runs only at graph-COMPILE time (the graph is
        // cached + recompiled on invalidation), NOT per frame. So the compute
        // pass must be added UNCONDITIONALLY here — the per-instance dispatch
        // list is populated by the bone-palette command each frame and read
        // LIVE in the record callback below. (Gating on dispatches().empty()
        // at compile time would bake an empty/absent pass into the graph.)

        // Import the skinned-output pool under the producer's typed RG name.
        // Mesh-draw consumers discover it via VertexProductionRegistry and do a
        // matching referenceBuffer(vertexProductRgName(Skinning)) + read() so the
        // graph orders Skinning → mesh draw and inserts the compute→vertex
        // barrier (and doesn't dead-prune this pass).
        RGBufferDescription desc{};
        desc.size = static_cast<uint64_t>(cfg_.output_pool_bytes);

        RGImportedBufferInfo imp{};
        imp.buffer_getter = [this](VkBuffer* out, uint32_t cap) -> uint32_t {
            if (out == nullptr || cap == 0)
                return 0u;
            auto* s = skinningResources();
            if (!s || !s->initialized())
                return 0u;
            out[0] = s->outputPool().buffer();
            return 1u;
        };
        out_pool_rg_ = builder.importBuffer(vertexProductRgName(EVertexProductKind::Skinning), desc, imp);

        builder.addPass("Skinning", ERGPassType::COMPUTE)
            .setComputePipeline(compute_pipeline_)
            .write(out_pool_rg_, ERGBufferRole::STORAGE)
            .bindResourceDS(
                0,
                this,
                &SkinningFeature::resolveSkinDS,
                EDSBindMode::PER_FIF,
                builder.trackExternalBuffer("ext.SkinPalette"),
                ERGResourceType::BUFFER
            )
            .useEngineSet(EDescriptorSetSlot::VertexPool) // a macro override places this at set1 (canonical 7); the
                                                          // slot is resolved at compile time
            .setKernelFn([this](const PassRecordContext& ctx) {
                // Single batched dispatch. Upload this frame's per-instance
                // SkinDispatchParams + running workgroup prefix sum into the
                // current ring slot of the dispatch-params SSBO, then issue
                // ONE vkCmdDispatch covering all skinned vertices. The shader
                // reads `gl_WorkGroupID.x`, binary-searches the prefix sum to
                // recover its dispatch index, and early-outs past the per-
                // instance vertex_count tail.
                auto* s = skinningResources();
                if (!s || ctx.pipeline_layout == VK_NULL_HANDLE)
                    return;
                const std::uint32_t total_wg = s->uploadDispatches();
                if (total_wg == 0u)
                    return; // no dispatches / capacity exceeded
                SkinPC pc{static_cast<std::uint32_t>(s->dispatches().size())};
                vkCmdPushConstants(ctx.cmd, ctx.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
                vkCmdDispatch(ctx.cmd, total_wg, 1, 1);
            }
            );
    }

    // The bound skin set must reference the SAME ring slot the upload command
    // wrote this frame. Both derive from serial % kMaxFramesInFlight, exposed by
    // SkinningResources::currentFrameIndex() — so we use it as the single source
    // of truth and ignore the framework's frame_slot (kept only as a fallback).
    VkDescriptorSet
    SkinningFeature::resolveSkinDS(const void* self, std::uint32_t frame_slot, std::uint32_t /*view_id*/) noexcept
    {
        const auto* f = static_cast<const SkinningFeature*>(self);
        const std::uint32_t fi = f->skin_res_ ? f->skin_res_->currentFrameIndex() : (frame_slot % kMaxFramesInFlight);
        return f->skin_ds_[fi % kMaxFramesInFlight];
    }

} // namespace lux::render
