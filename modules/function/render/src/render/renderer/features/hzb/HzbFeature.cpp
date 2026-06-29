/**
 * @file HzbFeature.cpp
 * @brief Hi-Z occlusion pyramid build feature — see HzbFeature.hpp.
 */

#include <lux/engine/render/renderer/features/hzb/HzbFeature.hpp>

#include <array>
#include <cstring>
#include <span>

#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraResource.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGEnums.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp>   // ensureBuiltinShader

namespace lux::render
{
    HzbFeature::HzbFeature(Config cfg)
        : RenderFeature(RenderFeature::Config{ .name = "Hzb" })
        , cfg_(cfg)
    {}

    void HzbFeature::initAndAttachTo(RenderScene& /*scene*/)
    {
        init(cfg_);
    }

    void HzbFeature::init(const Config& /*cfg*/)
    {
        auto& ctx = renderContext();
        VkDevice device = ctx.deviceContext().logicalDevice();

        // --- 1. Descriptor set layouts ---
        //   set 0 build = {0: SAMPLED_IMAGE src, 1: STORAGE_IMAGE dst} (per mip)
        //   set 1 depth = {0: SAMPLED_IMAGE depth}  (the build's mip-0 source)
        //   read (cull) = {0: COMBINED_IMAGE_SAMPLER hzb, 1: UNIFORM_BUFFER view}
        {
            std::array<VkDescriptorSetLayoutBinding, 2> b0{};
            b0[0] = { 0u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            b0[1] = { 1u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            DescriptorLayoutDesc d0{};
            d0.bindings   = std::span<const VkDescriptorSetLayoutBinding>(b0.data(), b0.size());
            d0.debug_name = "HzbBuildSet0";
            set0_layout_id_ = ctx.descriptorService().registerLayout(d0);

            std::array<VkDescriptorSetLayoutBinding, 1> b1{};
            b1[0] = { 0u, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            DescriptorLayoutDesc d1{};
            d1.bindings   = std::span<const VkDescriptorSetLayoutBinding>(b1.data(), b1.size());
            d1.debug_name = "HzbBuildSet1Depth";
            set1_layout_id_ = ctx.descriptorService().registerLayout(d1);
            set1_layout_    = ctx.descriptorService().layout(set1_layout_id_);

            std::array<VkDescriptorSetLayoutBinding, 2> rb{};
            rb[0] = { 0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            rb[1] = { 1u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
            DescriptorLayoutDesc rd{};
            rd.bindings   = std::span<const VkDescriptorSetLayoutBinding>(rb.data(), rb.size());
            rd.debug_name = "HzbReadSet";
            read_layout_id_ = ctx.descriptorService().registerLayout(rd);
            read_layout_    = ctx.descriptorService().layout(read_layout_id_);
        }

        // --- 2. HZB sampler (nearest + clamp; max-Z must not be interpolated) ---
        {
            VkSamplerCreateInfo si{};
            si.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            si.magFilter    = VK_FILTER_NEAREST;
            si.minFilter    = VK_FILTER_NEAREST;
            si.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            si.minLod       = 0.0f;
            si.maxLod       = VK_LOD_CLAMP_NONE;   // sample any mip via textureLod
            VkSampler sampler = VK_NULL_HANDLE;
            vkCreateSampler(device, &si, nullptr, &sampler);
            // Own it through the FIF deferred-destroy queue so it is reclaimed at
            // feature teardown (the bare-VkSampler member used to leak it past
            // vkDestroyDevice — VUID-vkDestroyDevice-device-05137).
            hzb_sampler_ = FifOwned<VkSampler>{&ctx.deferredDestroyQueue(), sampler};
        }

        // --- 3. HzbResources in the scene registry so the cull feature can
        //         find<HzbResources>() and bind its read DS (mirror SkinningResources).
        //         The images themselves are (re)created per the live view extent in
        //         onFrameBegin — init knows no extent. ---
        auto& reg = renderScene().sceneRegistry();
        if (reg.find<HzbResources>() == nullptr)
            reg.emplace<HzbResources>();
        hzb_res_ = reg.find<HzbResources>();

        // --- 4. Compute pipeline (set0, set1-depth) + push constant ---
        {
            auto* shaders = ctx.globalRegistry().find<ShaderResources>();
            ShaderHandle cs_h = cfg_.compute_shader;
            if (shaders)
                cs_h = ensureBuiltinShader(shaders, cs_h, EBuiltinShader::HZB_DOWNSAMPLE_COMP);
            auto* cs = shaders ? shaders->get(cs_h) : nullptr;
            if (cs == nullptr)
                return;

            const std::array<VkDescriptorSetLayout, 2> sls{
                ctx.descriptorService().layout(set0_layout_id_),
                set1_layout_,
            };
            const VkPushConstantRange pc{
                VK_SHADER_STAGE_COMPUTE_BIT, 0u,
                static_cast<uint32_t>(sizeof(HzbResources::BuildPushConstants)) };
            const std::array<VkPushConstantRange, 1> pcs{ pc };
            pipeline_layout_ = ctx.pipelineLayoutService().getOrCreate({
                .set_layouts    = sls,
                .push_constants = pcs,
                .debug_name     = "HzbBuildLayout",
            }).value();
            compute_pipeline_ = ctx.pipelineManager().registerComputePipeline(
                cs->module, pipeline_layout_);
        }
    }

    void HzbFeature::rebuildAt(uint32_t width, uint32_t height)
    {
        if (hzb_res_ == nullptr || width == 0u || height == 0u)
            return;

        auto& ctx = renderContext();
        VkDevice device = ctx.deviceContext().logicalDevice();
        vkDeviceWaitIdle(device);   // resize is rare → plain destroy/create is safe

        HzbResources::InitInfo ri{};
        ri.device      = device;
        ri.allocator   = ctx.deviceContext().vmaAllocator();
        ri.width       = width;
        ri.height      = height;
        ri.arena       = &renderScene().descriptorArena();
        ri.read_layout = read_layout_;
        ri.sampler     = hzb_sampler_.get();
        if (!hzb_res_->init(ri))
            return;

        // Re-point the per-mip build descriptors at the new images.
        const uint32_t n = hzb_res_->mipCount();
        const VkDescriptorSetLayout l0 = ctx.descriptorService().layout(set0_layout_id_);
        for (uint32_t slot = 0u; slot < 2u; ++slot)
        {
            mip_sets_[slot].assign(n, VK_NULL_HANDLE);
            for (uint32_t i = 0u; i < n; ++i)
                mip_sets_[slot][i] = renderScene().descriptorArena().allocate(l0);
            hzb_res_->writeBuildDescriptors(device, slot, mip_sets_[slot].data(), n);
        }

        // Mark BOTH slots not-ready (mip_count = 0): until a slot has actually
        // been built, the cull shader's `params.z < 1` guard keeps everything
        // (no over-cull on the first frame after a (re)build).
        HzbResources::ViewParams nr{};
        nr.params[0] = static_cast<float>(width);
        nr.params[1] = static_cast<float>(height);
        nr.params[2] = 0.0f;   // mip_count = 0 → not ready
        hzb_res_->writeViewParams(0u, nr);
        hzb_res_->writeViewParams(1u, nr);

        // First-use / post-resize: transition BOTH slots UNDEFINED→GENERAL via a
        // one-shot submit, BEFORE any RenderGraph pass runs this frame. The cull
        // samples the PREVIOUS slot — which won't be built until a later frame —
        // so without this its first-frame layout stays UNDEFINED (VUID-09600).
        // The vkDeviceWaitIdle above guarantees no concurrent GPU work, so a
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
                hzb_res_->recordInitToGeneral(cmd);
                vkEndCommandBuffer(cmd);
                VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
                si.commandBufferCount = 1u;
                si.pCommandBuffers    = &cmd;
                vkQueueSubmit(ctx.deviceContext().graphicsQueue(), 1u, &si, VK_NULL_HANDLE);
                vkQueueWaitIdle(ctx.deviceContext().graphicsQueue());
                vkDestroyCommandPool(device, pool, nullptr);
            }
        }
    }

    void HzbFeature::onFrameBegin(const FeatureFrameContext& /*ctx*/)
    {
        if (hzb_res_ == nullptr)
            return;

        // Main active view's extent + camera matrix (single editor main view; the
        // MVP only feeds HZB to the first active view).
        uint32_t     vw = 0u, vh = 0u;
        const float* view_proj = nullptr;
        auto* cam = renderScene().sceneRegistry().find<ViewCameraResource>();
        renderScene().forEachActiveView([&](const View& v)
        {
            if (vw == 0u)
            {
                vw = v.current_extent.width;
                vh = v.current_extent.height;
                auto* cam_fd = cam ? cam->find(v.handle.index) : nullptr;
                if (cam_fd)
                    view_proj = cam_fd->camera_view.view_proj.data();
            }
        });
        if (vw == 0u || vh == 0u)
            return;

        // (Re)build both images on first use / extent change.
        if (!hzb_res_->initialized() || vw != hzb_res_->width() || vh != hzb_res_->height())
            rebuildAt(vw, vh);
        if (!hzb_res_->initialized())
            return;

        // onFrameBegin fires once per frame, scene-wide → the feature-local counter
        // IS the absolute frame index; its parity picks the build (current) slot.
        ++frame_counter_;
        hzb_res_->setCurrent(frame_counter_);

        // Upload THIS frame's camera params into the CURRENT slot. Next frame the
        // cull reads the PREVIOUS slot = this frame's HZB + this frame's view_proj.
        HzbResources::ViewParams vp{};
        if (view_proj != nullptr)
            std::memcpy(vp.view_proj, view_proj, sizeof(vp.view_proj));
        vp.params[0] = static_cast<float>(hzb_res_->width());
        vp.params[1] = static_cast<float>(hzb_res_->height());
        vp.params[2] = static_cast<float>(hzb_res_->mipCount());   // >= 1 → ready
        vp.params[3] = 0.0f;
        hzb_res_->writeViewParams(hzb_res_->curIndex(), vp);
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
                // bug source). (M3 layer 3)
                { 0u, EDescriptorType::SAMPLED_IMAGE,
                  builder.referenceTexture("SceneDepth") },
            });

        builder.addPass("HzbBuild", ERGPassType::COMPUTE)
            .setComputePipeline(compute_pipeline_)
            .markSideEffect()   // HZB pyramid is consumed by the cull via a feature DS (bindResourceDS), not an RG .read → don't dead-prune this build
            .bindTransientDS(1, depth_tds)
            .read(builder.referenceTexture("SceneDepth"), lux::common::ETextureRole::SAMPLED)
            .setKernelFn([this](const PassRecordContext& pctx)
            {
                if (pctx.pipeline_layout == VK_NULL_HANDLE
                    || hzb_res_ == nullptr || !hzb_res_->initialized()
                    || mip_sets_[0].empty())
                    return;   // images not built yet this run
                const uint32_t slot = hzb_res_->curIndex();
                hzb_res_->recordBuild(pctx.cmd, pctx.pipeline_layout, slot,
                                      mip_sets_[slot].data(),
                                      static_cast<uint32_t>(mip_sets_[slot].size()));
            });
    }

} // namespace lux::render
