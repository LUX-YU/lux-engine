/**
 * @file HzbFeature.cpp
 * @brief Hi-Z occlusion pyramid build feature — see HzbFeature.hpp.
 */

#include <lux/engine/render/renderer/features/hzb/HzbFeature.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>   // sampler cache

#include <array>
#include <cstring>
#include <mutex>
#include <span>

#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraResource.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>   // resolveShaderStage
#include <lux/engine/function/render/client/features/deferred/DeferredGBufferOperation.hpp>

namespace lux::render
{
    HzbFeature::HzbFeature(Config cfg)
        : RenderFeature(RenderFeature::Config{ .name = "Hzb" })
        , cfg_(cfg)
    {}

    lux::render::Expected<void> HzbFeature::initAndAttachTo(RenderScene& /*scene*/)
    {
        return init(cfg_);
    }

    Expected<void> HzbFeature::init(const Config& /*cfg*/)
    {
        auto& ctx = renderContext();
        VkDevice device = ctx.deviceContext().logicalDevice();

        // --- 1. Read (cull) descriptor set layout ---
        //   {0: COMBINED_IMAGE_SAMPLER hzb, 1: UNIFORM_BUFFER view}
        //   This set is consumed by the cull pipeline (set1 of
        //   mesh_cull_unified.comp), not by this feature's build pipeline —
        //   so it is still hand-built. The build pipeline's set0/set1 have
        //   already been switched to reflection-based construction (see
        //   step 4 below).
        {
            std::array<VkDescriptorSetLayoutBinding, 2> rb{};
            rb[0] = { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            rb[1] = { 1u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            DescriptorLayoutDesc rd{};
            rd.bindings   = std::span<const VkDescriptorSetLayoutBinding>(rb.data(), rb.size());
            rd.debug_name = "HzbReadSet";
            read_layout_id_ = ctx.descriptorService().registerLayout(rd);
            read_layout_    = ctx.descriptorService().layout(read_layout_id_);
        }

        // --- 2. HZB sampler:共享缓存(最近邻 + clamp + 全 mip 采样,
        //     max-Z 不得插值)。原先困扰过的裸句柄泄漏
        //     (VUID-vkDestroyDevice-device-05137)由服务统一销毁根治。---
        hzb_sampler_ = ctx.descriptorService().sampler(SamplerDesc::nearestClampAllMips());

        // --- 3. HzbResources in the scene registry so the cull feature can
        //         find<HzbResources>() and bind its read DS (mirror SkinningResources).
        //         init() takes the DEVICE-level wiring only; the images are per
        //         view and per extent, created in onFrameBegin → rebuildViewAt. ---
        // ensure<T>(init_args):构造 + init + 只在成功时发布。原先同一个对象查了
        // 三次(find 判空 → emplace → 再 find),且 init 失败时对象已经进了注册表。
        auto& sreg = renderScene().sceneRegistry();
        const bool fresh_hzb = (sreg.find<HzbResources>() == nullptr);

        HzbResources::InitInfo ri{};
        ri.device      = ctx.deviceContext().logicalDevice();
        ri.allocator   = ctx.deviceContext().vmaAllocator();
        // 金字塔的释放必须延迟到 GPU 过水位:下面登记的视图销毁钩子跑在
        // RenderScene::removeView 里,那条路径不等设备空闲。
        ri.deferred_queue = &ctx.deferredDestroyQueue();
        ri.arena       = &renderScene().descriptorArena();
        ri.read_layout = read_layout_;
        ri.sampler     = hzb_sampler_;
        auto hzb_r = sreg.ensure<HzbResources>(ri);
        if (!hzb_r)
            return lux::cxx::unexpected<RenderError>(hzb_r.error());
        hzb_res_ = *hzb_r;

        if (fresh_hzb)
        {
            // 视图销毁时释放该视图的金字塔 —— **不是可选项**:view id 会被回收
            // (SlotKeyAutoSparseSet 把 index 带着 +1 的 generation 放回池子,而
            // feature 侧的 per-view 键是裸 index),留下的陈旧条目会被下一个拿到
            // 同一个 index 的视图静默捡走。由**安装点**登记,只在首次创建时登记。
            auto* res  = hzb_res_;
            auto* sets = &mip_sets_;
            sreg.addViewDestroyedHook(
                [res, sets](uint32_t /*scene_key*/, uint32_t view_id)
                {
                    res->evictView(view_id);
                    sets->erase(view_id);   // 描述符集由场景 arena 拥有,这里只丢句柄
                });
        }

        // --- 4. Compute pipeline (set0, set1-depth) + push constant ---
        {
            auto& shaders = ctx.globalRegistry().must<ShaderResources>();
            auto cs_h = resolveShaderStage(shaders, cfg_.compute_shader,
                                           EBuiltinShader::HZB_DOWNSAMPLE_COMP);
            if (!cs_h)
                return lux::cxx::unexpected(cs_h.error());
            const ShaderObject* cs = shaders.get(*cs_h);
            if (cs == nullptr)
                return renderFailure<err::shader::HandleStale>();

            // The build pipeline's layout is built via reflection (set0 =
            // src/dst per-mip, set1 = depth source; both are pass-local,
            // with no contract resources).
            auto pipeline = ctx.pipelineManager().registerComputePipelineReflected(
                cs->module, cs->info, "HzbBuild");
            if (!pipeline)
                return lux::cxx::unexpected(pipeline.error());
            compute_pipeline_ = *pipeline;
            set0_layout_ = ctx.pipelineManager().computeSetLayout(compute_pipeline_, 0);
            set1_layout_ = ctx.pipelineManager().computeSetLayout(compute_pipeline_, 1);
            // 反射没给出这两套 set = 着色器与本 feature 对布局的预期不一致。
            // 继续下去会在录制期绑一个空布局。
            if (set0_layout_ == VK_NULL_HANDLE)
                return renderFailure<err::pipeline::ReflectedSetLayoutMissing>(0u);
            if (set1_layout_ == VK_NULL_HANDLE)
                return renderFailure<err::pipeline::ReflectedSetLayoutMissing>(1u);
        }
        return {};
    }

    void HzbFeature::rebuildViewAt(uint32_t view_id, uint32_t width, uint32_t height)
    {
        if (hzb_res_ == nullptr || width == 0u || height == 0u)
            return;
        // Nothing to do when this view already has a pyramid at this extent.
        if (hzb_res_->viewReady(view_id)
            && hzb_res_->width(view_id) == width && hzb_res_->height(view_id) == height)
            return;

        auto& ctx = renderContext();
        VkDevice device = ctx.deviceContext().logicalDevice();
        if (ctx.deviceContext().waitIdle() != VK_SUCCESS)
            return;

        if (!hzb_res_->ensureView(view_id, width, height))
            return;

        // Re-point THIS VIEW's per-mip build descriptors at the new images.
        const uint32_t n = hzb_res_->mipCount(view_id);
        const VkDescriptorSetLayout l0 = set0_layout_;   // product of reflection-based construction (retrieved during init)
        auto& ms = mip_sets_[view_id];
        for (uint32_t slot = 0u; slot < 2u; ++slot)
        {
            ms.slot[slot].assign(n, VK_NULL_HANDLE);
            for (uint32_t i = 0u; i < n; ++i)
                ms.slot[slot][i] = renderScene().descriptorArena().allocate(l0);
            hzb_res_->writeBuildDescriptors(device, view_id, slot, ms.slot[slot].data(), n);
        }

        // Mark BOTH slots not-ready (mip_count = 0): until a slot has actually
        // been built, the cull shader's `params.z < 1` guard keeps everything
        // (no over-cull on the first frame after a (re)build).
        HzbResources::ViewParams nr{};
        nr.params[0] = static_cast<float>(width);
        nr.params[1] = static_cast<float>(height);
        nr.params[2] = 0.0f;   // mip_count = 0 → not ready
        hzb_res_->writeViewParams(view_id, 0u, nr);
        hzb_res_->writeViewParams(view_id, 1u, nr);

        // First-use / post-resize: transition BOTH slots UNDEFINED→GENERAL via a
        // one-shot submit, BEFORE any RenderGraph pass runs this frame. The cull
        // samples the PREVIOUS slot — which won't be built until a later frame —
        // so without this its first-frame layout stays UNDEFINED (VUID-09600).
        // DeviceContext::waitIdle above guarantees no concurrent GPU work, so a
        // blocking one-shot here is safe (resize is rare).
        {
            VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
            pci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pci.queueFamilyIndex = ctx.deviceContext().graphicsQueueFamilyIndex();
            VkCommandPool pool = VK_NULL_HANDLE;
            if (vkCreateCommandPool(device, &pci, nullptr, &pool) == VK_SUCCESS)
            {
                VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                ai.commandPool        = pool;
                ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                ai.commandBufferCount = 1u;
                VkCommandBuffer cmd = VK_NULL_HANDLE;
                vkAllocateCommandBuffers(device, &ai, &cmd);
                VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(cmd, &bi);
                hzb_res_->recordInitToGeneral(cmd, view_id);
                vkEndCommandBuffer(cmd);
                VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
                si.commandBufferCount = 1u;
                si.pCommandBuffers    = &cmd;
                {
                    const std::scoped_lock queue_lock(
                        ctx.deviceContext().graphicsQueueMutex());
                    vkQueueSubmit(
                        ctx.deviceContext().graphicsQueue(),
                        1u,
                        &si,
                        VK_NULL_HANDLE
                    );
                    vkQueueWaitIdle(ctx.deviceContext().graphicsQueue());
                }
                vkDestroyCommandPool(device, pool, nullptr);
            }
        }
    }

    void HzbFeature::onFrameBegin(const FeatureFrameContext& /*ctx*/)
    {
        if (hzb_res_ == nullptr)
            return;

        // onFrameBegin fires once per frame, scene-wide → the feature-local counter
        // IS the absolute frame index; its parity picks the build (current) slot.
        // Bumped ONCE here, then applied to every view, so all of a scene's
        // pyramids stay on the same ping-pong phase.
        ++frame_counter_;

        auto* cam = resolveViewCameraOnce(cam_cache_, renderScene().sceneRegistry());

        // EVERY active view gets its own pyramid, sized to its own extent.
        // (This used to take only the FIRST active view's extent and build one
        // scene-wide pair; with two views the build pass — which records once per
        // view — then overwrote the same images from each view's depth, so the
        // pyramid ended up holding the LAST view's depth at the FIRST view's size,
        // and both views culled against it.)
        renderScene().forEachActiveView([&](const View& v)
        {
            const uint32_t view_id = v.handle.index;
            const uint32_t vw = v.current_extent.width;
            const uint32_t vh = v.current_extent.height;
            if (vw == 0u || vh == 0u)
                return;

            // (Re)build this view's two images on first use / extent change.
            rebuildViewAt(view_id, vw, vh);
            if (!hzb_res_->viewReady(view_id))
                return;

            hzb_res_->setCurrent(view_id, frame_counter_);

            // Upload THIS frame's camera params into the CURRENT slot. Next frame
            // the cull reads the PREVIOUS slot = this frame's HZB + view_proj.
            HzbResources::ViewParams vp{};
            const auto* cam_fd = cam ? cam->find(view_id) : nullptr;
            if (cam_fd != nullptr)
            {
                const Eigen::Matrix4f relative_vp =
                    viewRelativeViewProjection(*cam_fd);
                std::memcpy(vp.view_proj, relative_vp.data(), sizeof(vp.view_proj));
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    vp.origin_page[axis] = cam_fd->render_origin.page_delta[axis];
                    vp.origin_local_page_size[axis] = cam_fd->render_origin.local[axis];
                }
                vp.origin_local_page_size[3] = cam_fd->coordinate_page_size;
            }
            vp.params[0] = static_cast<float>(hzb_res_->width(view_id));
            vp.params[1] = static_cast<float>(hzb_res_->height(view_id));
            vp.params[2] = static_cast<float>(hzb_res_->mipCount(view_id));   // >= 1 → ready
            vp.params[3] = 0.0f;
            hzb_res_->writeViewParams(view_id, hzb_res_->curIndex(view_id), vp);
        });
    }

    void HzbFeature::addPasses(RGBuilder& builder)
    {
        // Add the build pass unconditionally if the pipeline exists — the HZB
        // images are created per-frame in onFrameBegin, so the kernel checks
        // readiness at record time (the graph is compiled only once).
        if (!compute_pipeline_.valid())
            return;

        // set 1 = SceneDepth (SAMPLED), bound by the graph. The mip-0 source.
        auto depth_tds = builder.createTransientDS(
            "HzbDepthDS", set1_layout_,
            {
                // image_layout is filled by the compiler (autoFillTransientDSLayouts)
                // from this pass's .read(SceneDepth, SAMPLED): a DEPTH image resolves
                // to DEPTH_STENCIL_READ_ONLY_OPTIMAL, always matching the barrier — no
                // hand-written layout to drift (this site was the original 00344/08114
                // bug source).
                { 0u, EDescriptorType::SAMPLED_IMAGE,
                  builder.referenceTexture("SceneDepth") },
            });

        builder.addPass("HzbBuild", ERGPassType::COMPUTE)
            .setComputePipeline(compute_pipeline_)
            .markSideEffect()   // HZB pyramid is consumed by the cull via a feature DS (bindResourceDS), not an RG .read → don't dead-prune this build
            .bindTransientDS(1, depth_tds)
            .read(builder.referenceTexture("SceneDepth"), lux::common::ETextureRole::SAMPLED)
            // Build the next-frame pyramid from this frame's completed opaque
            // depth.  Without the explicit edge, a late Terrain/Cluster
            // contribution can reverse the inferred RAW direction and make
            // HZB consume an UNDEFINED imported depth image.
            .after(kDeferredGBufferDrawPassName)
            .setKernelFn([this](const PassRecordContext& pctx)
            {
                // Recording runs once per active view, so this kernel builds THAT
                // view's pyramid. (It used to build the one scene-wide pair, which
                // meant N views overwrote each other's pyramid every frame.)
                if (pctx.pipeline_layout == VK_NULL_HANDLE
                    || hzb_res_ == nullptr || pctx.view == nullptr)
                    return;
                const uint32_t view_id = pctx.view->handle.index;
                if (!hzb_res_->viewReady(view_id))
                    return;   // this view's images not built yet
                const auto* ms = mip_sets_.tryGet(view_id);
                if (ms == nullptr) return;
                const uint32_t slot = hzb_res_->curIndex(view_id);
                if (ms->slot[slot].empty()) return;
                hzb_res_->recordBuild(pctx.cmd, pctx.pipeline_layout, view_id, slot,
                                      ms->slot[slot].data(),
                                      static_cast<uint32_t>(ms->slot[slot].size()));
            });
    }

} // namespace lux::render
