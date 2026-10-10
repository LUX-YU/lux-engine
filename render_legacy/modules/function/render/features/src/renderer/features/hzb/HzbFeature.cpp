/**
 * @file HzbFeature.cpp
 * @brief Hi-Z occlusion pyramid build feature — see HzbFeature.hpp.
 */

#include <lux/engine/render/core/RenderErrorSink.hpp>

#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp> // sampler cache
#include <lux/engine/render/renderer/features/hzb/HzbFeature.hpp>

#include <array>
#include <cstring>
#include <mutex>
#include <span>

#include <lux/engine/function/render/features/deferred/DeferredGBufferOperation.hpp>
#include <lux/engine/function/render/graph/RGEnums.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/lifecycle/CommandBufferOwner.hpp>
#include <lux/engine/render/gpu/lifecycle/DeviceObject.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/graph/PassRecordContext.hpp>
#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraResource.hpp>
#include <lux/engine/render/resources/BuiltinShaderRegistry.hpp> // resolveShaderStage
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/scene/View.hpp>

namespace lux::render
{
    HzbFeature::HzbFeature(Config cfg) : RenderFeature(RenderFeature::Config{.name = "Hzb"}), cfg_(cfg) {}

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
            rb[0] = {0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            rb[1] = {1u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            DescriptorLayoutDesc rd{};
            rd.bindings = std::span<const VkDescriptorSetLayoutBinding>(rb.data(), rb.size());
            rd.debug_name = "HzbReadSet";
            const auto registered = ctx.descriptorService().registerLayout(rd);
            if (!registered)
            {
                return lux::cxx::unexpected(registered.error());
            }
            read_layout_id_ = *registered;
            read_layout_ = ctx.descriptorService().layout(read_layout_id_);
        }

        // --- 2. HZB sampler:共享缓存(最近邻 + clamp + 全 mip 采样,
        //     max-Z 不得插值)。原先困扰过的裸句柄泄漏
        //     (VUID-vkDestroyDevice-device-05137)由服务统一销毁根治。---
        auto hzb_sampler = ctx.descriptorService().sampler(SamplerDesc::nearestClampAllMips());
        if (!hzb_sampler)
        {
            return lux::cxx::unexpected(hzb_sampler.error());
        }
        hzb_sampler_ = *hzb_sampler;

        // The scene owns one complete resource; extents remain per-view capabilities.
        auto& sreg = renderScene().resources();
        hzb_res_ = sreg.find<HzbResources>();
        const bool fresh_hzb = hzb_res_ == nullptr;
        if (fresh_hzb)
        {
            auto resource = HzbResources::create(
                {.device = ctx.deviceContext(),
                 .retirement = ctx.deferredDestroyQueue(),
                 .arena = &renderScene().descriptorArena(),
                 .read_layout = read_layout_,
                 .sampler = hzb_sampler_}
            );
            if (!resource)
            {
                return lux::cxx::unexpected(resource.error());
            }
            hzb_res_ = sreg.insert(std::move(*resource)).get();
        }

        // --- 4. Compute pipeline (set0, set1-depth) + push constant ---
        {
            auto& shaders = ctx.globalRegistry().must<ShaderResources>();
            auto cs_h = resolveShaderStage(shaders, cfg_.compute_shader, EBuiltinShader::HZB_DOWNSAMPLE_COMP);
            if (!cs_h)
            {
                return lux::cxx::unexpected(cs_h.error());
            }
            const ShaderObject* cs = shaders.get(*cs_h);
            if (cs == nullptr)
            {
                return renderFailure<err::shader::HandleStale>();
            }

            // The build pipeline's layout is built via reflection (set0 =
            // src/dst per-mip, set1 = depth source; both are pass-local,
            // with no contract resources).
            auto pipeline =
                ctx.pipelineManager().registerComputePipelineReflected(cs->module.get(), cs->info, "HzbBuild");
            if (!pipeline)
            {
                return lux::cxx::unexpected(pipeline.error());
            }
            compute_pipeline_ = *pipeline;
            set0_layout_ = ctx.pipelineManager().computeSetLayout(compute_pipeline_, 0);
            set1_layout_ = ctx.pipelineManager().computeSetLayout(compute_pipeline_, 1);
            // 反射没给出这两套 set = 着色器与本 feature 对布局的预期不一致。
            // 继续下去会在录制期绑一个空布局。
            if (set0_layout_ == VK_NULL_HANDLE)
            {
                return renderFailure<err::pipeline::ReflectedSetLayoutMissing>(0u);
            }
            if (set1_layout_ == VK_NULL_HANDLE)
            {
                return renderFailure<err::pipeline::ReflectedSetLayoutMissing>(1u);
            }
        }
        return {};
    }

    Expected<void> HzbFeature::rebuildViewAt(uint32_t view_id, uint32_t width, uint32_t height) noexcept
    {
        const bool is_same_extent = hzb_res_->width(view_id) == width && hzb_res_->height(view_id) == height;
        if (is_same_extent && mip_sets_.contains(view_id))
        {
            return {};
        }
        auto& ctx = renderContext();
        auto& device = ctx.deviceContext();
        const auto idle_result = device.waitIdle();
        if (idle_result != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(idle_result));
        }
        auto candidate = hzb_res_->prepareView(width, height);
        if (!candidate)
        {
            return lux::cxx::unexpected(candidate.error());
        }
        const auto mip_count = (*candidate)->mip_count;
        ViewMipSets build_sets;
        for (auto& slot : build_sets.slot)
        {
            slot.reserve(mip_count);
            for (uint32_t mip = 0; mip < mip_count; ++mip)
            {
                auto descriptor = renderScene().descriptorArena().allocate(set0_layout_);
                if (!descriptor)
                {
                    return lux::cxx::unexpected(descriptor.error());
                }
                slot.push_back(*descriptor);
            }
        }

        // This is the existing synchronous resize boundary. All candidates stay
        // owned until initialization is complete, including a failed fence wait.
        auto command = CommandBufferOwner::create(device.logicalDevice(), ctx.resourceContext().commandPool());
        if (!command)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(command.error()));
        }
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        const auto begin_result = vkBeginCommandBuffer(command->get(), &begin);
        if (begin_result != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(begin_result));
        }
        HzbResources::recordViewInitialization(command->get(), **candidate);
        const auto end_result = vkEndCommandBuffer(command->get());
        if (end_result != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(end_result));
        }
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        auto fence = FenceOwner::create(device.logicalDevice(), fence_info);
        if (!fence)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(fence.error()));
        }
        const auto cmd = command->get();
        VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submission.commandBufferCount = 1;
        submission.pCommandBuffers = &cmd;
        {
            const std::scoped_lock queue_lock(device.graphicsQueueMutex());
            const auto submit_result = vkQueueSubmit(device.graphicsQueue(), 1, &submission, fence->get());
            if (submit_result != VK_SUCCESS)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(submit_result));
            }
        }
        const auto fence_handle = fence->get();
        const auto completed = vkWaitForFences(device.logicalDevice(), 1, &fence_handle, VK_TRUE, UINT64_MAX);
        if (completed != VK_SUCCESS)
        {
            const auto idle = device.waitIdle();
            const bool can_release = idle == VK_SUCCESS || idle == VK_ERROR_DEVICE_LOST;
            if (!can_release)
            {
                renderFatal("HZB initialization could not establish a safe resource release point");
            }
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(completed));
        }

        // No fallible backend work remains. Read images and build sets become
        // visible together; old native owners retire at the original watermark.
        hzb_res_->adoptView(view_id, std::move(*candidate));
        mip_sets_[view_id] = std::move(build_sets);
        const auto& accepted = mip_sets_[view_id];
        for (uint32_t slot = 0; slot < 2; ++slot)
        {
            hzb_res_
                ->writeBuildDescriptors(device.logicalDevice(), view_id, slot, accepted.slot[slot].data(), mip_count);
        }
        return {};
    }

    void HzbFeature::deallocateViewState(uint32_t view_id)
    {
        hzb_res_->evictView(view_id);
        mip_sets_.erase(view_id);
    }

    void HzbFeature::onFrameBegin(const FeatureFrameContext& /*ctx*/)
    {
        if (hzb_res_ == nullptr)
        {
            return;
        }

        // onFrameBegin fires once per frame, scene-wide → the feature-local counter
        // IS the absolute frame index; its parity picks the build (current) slot.
        // Bumped ONCE here, then applied to every view, so all of a scene's
        // pyramids stay on the same ping-pong phase.
        ++frame_counter_;

        auto* cam = resolveViewCameraOnce(cam_cache_, renderScene().resources());

        // EVERY active view gets its own pyramid, sized to its own extent.
        // (This used to take only the FIRST active view's extent and build one
        // scene-wide pair; with two views the build pass — which records once per
        // view — then overwrote the same images from each view's depth, so the
        // pyramid ended up holding the LAST view's depth at the FIRST view's size,
        // and both views culled against it.)
        renderScene().forEachActiveView(
            [&](const View& v)
            {
                const uint32_t view_id = v.handle.index;
                const uint32_t vw = v.current_extent.width;
                const uint32_t vh = v.current_extent.height;
                if (vw == 0u || vh == 0u)
                {
                    return;
                }

                // (Re)build this view's two images on first use / extent change.
                if (auto ready = rebuildViewAt(view_id, vw, vh); !ready)
                {
                    if (auto* sink = renderContext().errorSink())
                    {
                        sink->emit(ready.error(), RenderErrorEvent::kNoScene, 0);
                    }
                    return;
                }

                hzb_res_->setCurrent(view_id, frame_counter_);

                // Upload THIS frame's camera params into the CURRENT slot. Next frame
                // the cull reads the PREVIOUS slot = this frame's HZB + view_proj.
                HzbResources::ViewParams vp{};
                const auto* cam_fd = cam ? cam->find(view_id) : nullptr;
                if (cam_fd != nullptr)
                {
                    const Eigen::Matrix4f relative_vp = viewRelativeViewProjection(*cam_fd);
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
                vp.params[2] = static_cast<float>(hzb_res_->mipCount(view_id)); // >= 1 → ready
                vp.params[3] = 0.0f;
                hzb_res_->writeViewParams(view_id, hzb_res_->curIndex(view_id), vp);
            }
        );
    }

    void HzbFeature::addPasses(RGBuilder& builder)
    {
        // Add the build pass unconditionally if the pipeline exists — the HZB
        // images are created per-frame in onFrameBegin, so the kernel checks
        // readiness at record time (the graph is compiled only once).
        if (!compute_pipeline_.valid())
        {
            return;
        }

        // set 1 = SceneDepth (SAMPLED), bound by the graph. The mip-0 source.
        auto depth_tds = builder.createTransientDS(
            "HzbDepthDS",
            set1_layout_,
            {
                // image_layout is filled by the compiler (autoFillTransientDSLayouts)
                // from this pass's .read(SceneDepth, SAMPLED): a DEPTH image resolves
                // to DEPTH_STENCIL_READ_ONLY_OPTIMAL, always matching the barrier — no
                // hand-written layout to drift (this site was the original 00344/08114
                // bug source).
                {0u, EDescriptorType::SAMPLED_IMAGE, builder.referenceTexture("SceneDepth")},
            }
        );

        builder.addPass("HzbBuild", ERGPassType::COMPUTE)
            .setComputePipeline(compute_pipeline_)
            .markSideEffect() // HZB pyramid is consumed by the cull via a feature DS (bindResourceDS), not an RG .read
                              // → don't dead-prune this build
            .bindTransientDS(1, depth_tds)
            .read(builder.referenceTexture("SceneDepth"), lux::render::ETextureRole::SAMPLED)
            // Build the next-frame pyramid from this frame's completed opaque
            // depth.  Without the explicit edge, a late Terrain/Cluster
            // contribution can reverse the inferred RAW direction and make
            // HZB consume an UNDEFINED imported depth image.
            .after(kDeferredGBufferDrawPassName)
            .setKernelFn(
                [this](const PassRecordContext& pctx)
                {
                    // Recording runs once per active view, so this kernel builds THAT
                    // view's pyramid. (It used to build the one scene-wide pair, which
                    // meant N views overwrote each other's pyramid every frame.)
                    if (pctx.pipeline_layout == VK_NULL_HANDLE || hzb_res_ == nullptr || pctx.view == nullptr)
                    {
                        return;
                    }
                    const uint32_t view_id = pctx.view->handle.index;
                    if (!hzb_res_->viewReady(view_id))
                    {
                        return; // this view's images not built yet
                    }
                    const auto* ms = mip_sets_.tryGet(view_id);
                    if (ms == nullptr)
                    {
                        return;
                    }
                    const uint32_t slot = hzb_res_->curIndex(view_id);
                    if (ms->slot[slot].empty())
                    {
                        return;
                    }
                    hzb_res_->recordBuild(
                        pctx.cmd,
                        pctx.pipeline_layout,
                        view_id,
                        slot,
                        ms->slot[slot].data(),
                        static_cast<uint32_t>(ms->slot[slot].size())
                    );
                }
            );
    }

} // namespace lux::render
