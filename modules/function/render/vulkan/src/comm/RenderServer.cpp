#include <lux/engine/render/comm/server/RenderServerImpl.hpp>
// (InitialViewCamera.hpp retired — initial camera is a StandardViewCamera op now.)
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/render/comm/RenderTickPipeline.hpp>

// VMA — readback staging buffer uses raw vmaCreateBuffer/vmaInvalidateAllocation
// directly (previously pulled in transitively via SkinningResources.hpp, which
// moved to a feature; include what we use).
#include <vk_mem_alloc.h>

#include <mutex>

// Vulkan infrastructure
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/gpu/transfer/TransferContributor.hpp>
#include <lux/engine/render/renderer/Renderer.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/core/DescriptorSetLayoutContract.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>

// Targets
#include <lux/engine/render/targets/OffscreenImagePool.hpp>
#include <lux/engine/render/gpu/RenderSurface.hpp>
#include <lux/engine/render/targets/SwapchainProvider.hpp>
#include <lux/engine/render/renderer/FrameDriver.hpp>

// Window
#include <lux/engine/window/LuxWindow.hpp>

// Resources
#include <lux/engine/render/resources/TextureResources.hpp>
// (LightResources include removed — light is feature-owned now; LightFeature
//  emplaces the per-scene LightResources, not the core server.)
#include <lux/engine/render/resources/material/MaterialResources.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/gpu/pipeline/VertexLayoutRegistry.hpp>   // vertex-layout SSOT
#include <lux/engine/render/gpu/pipeline/VertexLayoutSpec.hpp>       // make*VertexLayout()
#include <lux/engine/render/resources/lighting/ShadowResources.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/resources/vertex/StaticVertexPoolSet.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/descriptor/BindlessCombinedSet.hpp>
#include <lux/engine/render/resources/lifecycle/GpuTransferPipeline.hpp>
#include <lux/engine/render/gpu/lifecycle/VRAMBudgetGuard.hpp>

// Resource descriptions (rdesc)
#include <lux/engine/description/MaterialEnums.hpp>   // rdesc::EAlphaMode (graph render-state)
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Texture.hpp>

// Shader compilation
#include <lux/engine/render/gpu/ShaderObject.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/description/Shader.hpp>

// Scene / View
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/function/render/client/core/RenderViewTypes.hpp>
#include <lux/engine/render/resources/material/MaterialFamily.hpp>
// (LightDescriptor include removed — light commands are feature-scoped now.)

#include <lux/cxx/container/SparseSet.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <sstream>      // DumpRenderGraph → in-memory text capture
#include <limits>
#include <unordered_map>
#include <lux/engine/render/gpu/lifecycle/VRAMBudgetGuard.hpp>

namespace lux::render
{

    void registerServerHandlers(GeneralRenderServer::Dispatcher& dispatcher);

    GeneralRenderServer::GeneralRenderServer(
        std::shared_ptr<Channel> frame_channel,
        std::shared_ptr<RenderControlChannel<>> control_channel,
        std::shared_ptr<RenderUploadChannel<>> upload_channel,
        std::shared_ptr<RenderChannelSync> sync)
        : GeneralRenderServer(
              std::move(frame_channel),
              std::move(control_channel),
              std::move(upload_channel),
              std::move(sync),
              std::make_unique<Impl>())
    {
    }

    GeneralRenderServer::GeneralRenderServer(
        std::shared_ptr<Channel> frame_channel,
        std::shared_ptr<RenderControlChannel<>> control_channel,
        std::shared_ptr<RenderUploadChannel<>> upload_channel,
        std::shared_ptr<RenderChannelSync> sync,
        std::unique_ptr<Impl> impl)
        : RenderServer<>(frame_channel, sync, impl->dispatcher)
        , impl_(std::move(impl))
        , control_server_(std::make_unique<RenderControlServer>(
              std::move(control_channel), sync, impl_->dispatcher))
        , upload_server_(std::make_unique<RenderUploadServer>(
              std::move(upload_channel), std::move(sync), impl_->dispatcher))
    {
        impl_->server_ = this;
    }

    GeneralRenderServer::~GeneralRenderServer() = default;

    Expected<void> GeneralRenderServer::init(ServerConfig config)
    {
        auto result = impl_->init(std::move(config));
        if (!result)
        {
            return result;
        }
        registerServerHandlers(impl_->dispatcher);

        // 分发失败接进自发错误通道。CommandFailedReply 只负责解除等回复请求的
        // 阻塞;即发即忘命令(removeView/destroyScene/setLayer/destroy*)的分发
        // 失败此前是彻底的黑洞 —— 这里是它们唯一的出口。
        setDispatchFailureSink(+[](void* user_state, EDispatchFailure,
                                   const RenderError& error) noexcept
        {
            auto& im = *static_cast<GeneralRenderServer::Impl*>(user_state);
            im.error_sink_.emit(error, RenderErrorEvent::kNoScene,
                                im.current_stamp_.serial);
        });
        control_server_->setDispatchFailureSink(
            +[](void* user_state, EDispatchFailure,
                const RenderError& error) noexcept
            {
                auto& im = *static_cast<GeneralRenderServer::Impl*>(user_state);
                im.error_sink_.emit(error, RenderErrorEvent::kNoScene,
                                    im.current_stamp_.serial);
            });
        upload_server_->setDispatchFailureSink(
            +[](void* user_state, EDispatchFailure,
                const RenderError& error) noexcept
            {
                auto& im = *static_cast<GeneralRenderServer::Impl*>(user_state);
                im.error_sink_.emit(error, RenderErrorEvent::kNoScene,
                                    im.current_stamp_.serial);
            });
        return {};
    }

    bool GeneralRenderServer::drainRequest()
    {
        if (!acquireAndExecute(/*blocking=*/false, impl_.get()))
        {
            return false;
        }
        (void)impl_->processUploadCompletions();
        return finalizeReplies(/*blocking=*/false);
    }

    bool GeneralRenderServer::drainRequestBlocking()
    {
        if (!acquireAndExecute(/*blocking=*/true, impl_.get()))
        {
            return false;
        }
        (void)impl_->processUploadCompletions();
        return finalizeReplies(/*blocking=*/true);
    }

    // ── Per-kind completion finalization ──────────────────────────────────

    void GeneralRenderServer::Impl::transitionUpload(
        std::uint32_t request_id,
        TransferCompletion::Kind kind,
        std::uint32_t resource_index,
        std::uint32_t resource_gen,
        EUploadLifecycleState state
    ) noexcept
    {
        if (request_id == UINT32_MAX)
        {
            return;
        }

        auto it = std::find_if(
            active_uploads_.begin(), active_uploads_.end(),
            [request_id](const ActiveUpload& upload)
            { return upload.request_id == request_id; }
        );
        if (state == EUploadLifecycleState::Accepted)
        {
            if (it != active_uploads_.end())
            {
                ++upload_lifecycle_.duplicate_terminal;
                error_sink_.emit(
                    renderError<err::upload::StateTransitionInvalid>(
                        request_id,
                        static_cast<std::uint32_t>(it->state),
                        static_cast<std::uint32_t>(state)
                    ),
                    RenderErrorEvent::kNoScene,
                    current_stamp_.serial
                );
                return;
            }
            active_uploads_.push_back(ActiveUpload{
                request_id,
                kind,
                resource_index,
                resource_gen,
                state,
            });
            ++upload_lifecycle_.accepted;
            upload_lifecycle_.active = active_uploads_.size();
            return;
        }

        if (it == active_uploads_.end() || it->kind != kind ||
            it->resource_index != resource_index ||
            it->resource_gen != resource_gen)
        {
            ++upload_lifecycle_.stale_result;
            error_sink_.emit(
                renderError<err::upload::ResultIdentityMismatch>(
                    request_id,
                    resource_index,
                    resource_gen
                ),
                RenderErrorEvent::kNoScene,
                current_stamp_.serial
            );
            return;
        }

        if (!isValidUploadLifecycleTransition(it->state, state))
        {
            if (isUploadLifecycleTerminal(it->state))
                ++upload_lifecycle_.duplicate_terminal;
            else
                ++upload_lifecycle_.stale_result;
            error_sink_.emit(
                renderError<err::upload::StateTransitionInvalid>(
                    request_id,
                    static_cast<std::uint32_t>(it->state),
                    static_cast<std::uint32_t>(state)
                ),
                RenderErrorEvent::kNoScene,
                current_stamp_.serial
            );
            return;
        }

        it->state = state;
        if (!isUploadLifecycleTerminal(state))
        {
            return;
        }

        if (state == EUploadLifecycleState::Ready)
            ++upload_lifecycle_.terminal_ready;
        else
            ++upload_lifecycle_.terminal_failed;
        recent_upload_terminals_[recent_upload_terminal_cursor_] = request_id;
        recent_upload_terminal_cursor_ =
            (recent_upload_terminal_cursor_ + 1u) % kRecentUploadTerminals;
        active_uploads_.erase(it);
        upload_lifecycle_.active = active_uploads_.size();
    }

    bool GeneralRenderServer::Impl::observeTransferResult(
        const TransferCompletion& completion) noexcept
    {
        if (completion.request_id == UINT32_MAX)
        {
            return true;
        }

        const auto it = std::find_if(
            active_uploads_.begin(), active_uploads_.end(),
            [&](const ActiveUpload& upload)
            { return upload.request_id == completion.request_id; }
        );
        if (it == active_uploads_.end())
        {
            const bool terminal = std::find(
                recent_upload_terminals_.begin(),
                recent_upload_terminals_.end(),
                completion.request_id
            ) != recent_upload_terminals_.end();
            if (terminal)
                ++upload_lifecycle_.duplicate_terminal;
            else
                ++upload_lifecycle_.stale_result;
            error_sink_.emit(
                renderError<err::upload::ResultIdentityMismatch>(
                    completion.request_id,
                    completion.resource_handle,
                    completion.resource_gen
                ),
                RenderErrorEvent::kNoScene,
                current_stamp_.serial
            );
            return false;
        }
        if (it->kind != completion.kind ||
            it->resource_index != completion.resource_handle ||
            it->resource_gen != completion.resource_gen ||
            it->state != EUploadLifecycleState::TransferQueued)
        {
            ++upload_lifecycle_.stale_result;
            error_sink_.emit(
                renderError<err::upload::ResultIdentityMismatch>(
                    completion.request_id,
                    completion.resource_handle,
                    completion.resource_gen
                ),
                RenderErrorEvent::kNoScene,
                current_stamp_.serial
            );
            return false;
        }

        transitionUpload(
            completion.request_id,
            completion.kind,
            completion.resource_handle,
            completion.resource_gen,
            EUploadLifecycleState::RecordedOrTransferComplete
        );
        return true;
    }

    void GeneralRenderServer::Impl::settleUploadReply(
        const DeferredReplyEntry& reply) noexcept
    {
        transitionUpload(
            reply.request_id,
            reply.kind,
            reply.resource_index,
            reply.resource_gen,
            reply.status == 0
                ? EUploadLifecycleState::Ready
                : EUploadLifecycleState::Failed
        );
    }

    UploadLifecycleSnapshot
    GeneralRenderServer::Impl::uploadLifecycle() const noexcept
    {
        auto snapshot = upload_lifecycle_;
        snapshot.active = active_uploads_.size();
        if (transfer_pipeline_)
        {
            snapshot.staging_copied_bytes =
                transfer_pipeline_->stagingCopiedBytes();
        }
        return snapshot;
    }

    UploadLifecycleSnapshot
    GeneralRenderServer::uploadLifecycle() const noexcept
    {
        return impl_->uploadLifecycle();
    }

    void GeneralRenderServer::Impl::failActiveUploadsForShutdown() noexcept
    {
        // A residual entry here means the transfer owner terminated without a
        // result. Preserve exactly-once reply semantics and reclaim only the
        // generation recorded by that entry. Generation checks in the
        // resource stores prevent a late request from touching a reused slot.
        for (const auto& upload : active_uploads_)
        {
            const bool already_has_reply = std::any_of(
                pending_deferred_replies_.begin(),
                pending_deferred_replies_.end(),
                [&](const DeferredReplyEntry& reply)
                { return reply.request_id == upload.request_id; }
            );
            if (already_has_reply)
            {
                continue;
            }

            TransferCompletion completion{};
            completion.kind = upload.kind;
            completion.request_id = upload.request_id;
            completion.resource_handle = upload.resource_index;
            completion.resource_gen = upload.resource_gen;
            reclaimReservedSlot(completion);
            pending_deferred_replies_.push_back(DeferredReplyEntry{
                upload.request_id,
                upload.kind,
                upload.resource_index,
                upload.resource_gen,
                0u,
                1u,
            });
        }
    }

    UploadLifecycleSnapshot
    GeneralRenderServer::closeAcceptedUploads() noexcept
    {
        auto& im = *impl_;

        // Stop has already closed producer admission. Drain every packet that
        // crossed a channel boundary before that point. Reply-space progress
        // comes from the host's concurrent pump loop and wakes work_epoch.
        for (;;)
        {
            bool progressed = false;
            while (control_server_->drainAndDispatch(impl_.get()))
            {
                progressed = true;
            }
            while (upload_server_->drainAndDispatch(impl_.get()))
            {
                progressed = true;
            }

            const bool ingress_empty =
                control_server_->endpoint().requests.empty() &&
                upload_server_->endpoint().requests.empty();
            if (ingress_empty)
            {
                break;
            }
            if (progressed)
            {
                continue;
            }

            const auto observed = channelSync().work_epoch.load(
                std::memory_order_acquire
            );
            if (control_server_->drainAndDispatch(impl_.get()) ||
                upload_server_->drainAndDispatch(impl_.get()))
                continue;
            channelSync().work_epoch.wait(
                observed,
                std::memory_order_acquire
            );
        }

        if (im.transfer_pipeline_)
        {
            im.transfer_pipeline_->shutdown();
            (void)im.processUploadCompletions();
            const auto upload_idle =
                im.dev_ctx_->logicalDevice().waitIdle();
            if (upload_idle == VK_ERROR_DEVICE_LOST)
            {
                // The terminal render error was already published by the
                // frame owner. Device loss is not a close invariant failure:
                // no GPU work can retire, so make processUploadCompletions()
                // take its explicit device-lost path and fail accepted uploads
                // exactly once. A second fatal here used to hide the original
                // fault and prevented crash-diagnostic artifacts from landing.
                (void)im.processUploadCompletions();
            }
            else if (upload_idle != VK_SUCCESS)
            {
                renderFatal("upload close wait-idle failed");
            }
            // The first pass may have submitted the graphics-finalize batch;
            // wait for that queue work and then move its replies to the
            // publish list.
            (void)im.processUploadCompletions();
            if (!im.pending_graphics_finalizes_.empty())
            {
                const auto finalize_idle =
                    im.dev_ctx_->logicalDevice().waitIdle();
                if (finalize_idle == VK_ERROR_DEVICE_LOST)
                    (void)im.processUploadCompletions();
                else if (finalize_idle != VK_SUCCESS)
                {
                    renderFatal("graphics-finalize close wait-idle failed");
                }
                (void)im.processUploadCompletions();
            }
        }

        im.failActiveUploadsForShutdown();
        while (!im.active_uploads_.empty())
        {
            const auto before = im.active_uploads_.size();
            flushDeferredRepliesOnly();
            if (im.active_uploads_.size() < before)
            {
                continue;
            }

            const auto observed = channelSync().work_epoch.load(
                std::memory_order_acquire
            );
            flushDeferredRepliesOnly();
            if (im.active_uploads_.size() < before)
            {
                continue;
            }
            channelSync().work_epoch.wait(
                observed,
                std::memory_order_acquire
            );
        }
        return im.uploadLifecycle();
    }

    GeneralRenderServer::Impl::FinalizeDisposition GeneralRenderServer::Impl::finalizeMeshCompletion(
        TransferCompletion& c, bool needs_qfot,
        uint32_t src_family, uint32_t dst_family)
    {
        auto* mr = render_ctx_->globalRegistry().find<MeshResources>();
        if (!mr)
        {
            // MeshResources gone (teardown) — can't finalize. Free the staging so it is
            // not leaked, and report Failed so the caller neither claims success nor
            // retires the staging again.
            if (c.stg_buf != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(dev_ctx_->vmaAllocator(), c.stg_buf, c.stg_alloc);
            }
            return FinalizeDisposition::Failed;
        }

        if (!c.gpu_copy_recorded)
        {
            MeshResources::PendingStagingCopy sc{};
            sc.stg_buf        = c.stg_buf;
            sc.vbo_dst        = c.mesh.vbo_buf;
            sc.vbo_stg_offset = 0;
            sc.vbo_dst_offset = c.mesh.vbo_offset;
            sc.vbo_size       = c.mesh.vbo_size;
            sc.ibo_dst        = c.mesh.ibo_buf;
            sc.ibo_stg_offset = c.mesh.vbo_size;
            sc.ibo_dst_offset = c.mesh.ibo_offset;
            sc.ibo_size       = c.mesh.ibo_size;
            sc.mesh_index     = c.mesh.mesh_index;
            mr->pushStagingCopy(sc);
        }
        else
        {
            if (needs_qfot)
            {
                mr->pushAcquireBarrier(c.mesh.vbo_buf, c.mesh.vbo_offset,
                                       c.mesh.vbo_size, src_family, dst_family);
                if (c.mesh.ibo_size > 0)
                    mr->pushAcquireBarrier(c.mesh.ibo_buf, c.mesh.ibo_offset,
                                           c.mesh.ibo_size, src_family, dst_family);
            }
            mr->markReady(c.mesh.mesh_index);
        }
        return FinalizeDisposition::Succeeded;
    }

    GeneralRenderServer::Impl::FinalizeDisposition GeneralRenderServer::Impl::finalizeTexture2DCompletion(
        TransferCompletion& c, bool needs_qfot,
        uint32_t src_family, uint32_t dst_family)
    {
        auto& tr = render_ctx_->globalRegistry().must<TextureResources>();

        auto& bcs = tr.bindlessSet2D();
        const bool replacement =
            c.kind == TransferCompletion::Kind::Texture2DReplacement;
        const TextureHandle handle{
            c.texture.slot_index, c.resource_gen};
        // The reserved slot must still be ours: if it was recycled/removed before this
        // async completion retired, finalizing would write into a now-foreign slot.
        // Free the worker's GPU objects (incl. staging) and drop it.
        if (!bcs.isTextureAlive(SlotHandle{c.texture.slot_index, c.resource_gen}))
        {
            if (replacement)
            {
                tr.endMipReplacement(handle);
            }
            freeCompletionTextureGpu(c);
            return FinalizeDisposition::Failed;
        }
        const auto install = [&]
        {
            if (replacement)
            {
                bcs.replaceTransferredTexture(
                    c.texture.slot_index,
                    c.texture.image,
                    c.texture.image_alloc,
                    c.texture.view,
                    c.texture.sampler,
                    c.texture.format,
                    c.texture.mip_levels,
                    c.texture.array_layers,
                    c.texture.width,
                    c.texture.height);
            }
            else
            {
                bcs.finalizeTransferredTexture(
                    c.texture.slot_index,
                    c.texture.image,
                    c.texture.image_alloc,
                    c.texture.view,
                    c.texture.sampler,
                    c.texture.format,
                    c.texture.mip_levels,
                    c.texture.array_layers,
                    c.texture.width,
                    c.texture.height);
            }
        };
        if (!c.gpu_copy_recorded)
        {
            install();
            BindlessCombinedSet::PendingStagingTexture st{};
            st.stg_buf     = c.stg_buf;
            st.stg_size    = c.stg_size;
            st.slot_index  = c.texture.slot_index;
            st.do_mips     = c.texture.needs_mip_gen;
            st.is_cube     = false;
            st.face_stride = 0;
            st.mip_copy_count = std::clamp<uint32_t>(
                c.texture.uploaded_mip_count,
                1u,
                rdesc::kTextureMaxMipCount);
            for (uint32_t i = 0; i < st.mip_copy_count; ++i)
            {
                st.mip_copies[i].buffer_offset = c.texture.uploaded_mips[i].buffer_offset;
                st.mip_copies[i].mip_level = c.texture.uploaded_mips[i].mip_level;
                st.mip_copies[i].width = c.texture.uploaded_mips[i].width;
                st.mip_copies[i].height = c.texture.uploaded_mips[i].height;
            }
            bcs.pushStagingTextureCopy(st);
        }
        else
        {
            if (needs_qfot)
            {
                bcs.pushImageAcquireBarrier(
                    c.texture.image, c.texture.mip_levels, c.texture.array_layers,
                    c.texture.needs_mip_gen
                        ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    src_family, dst_family);
            }
            if (c.texture.needs_mip_gen)
            {
                bcs.pushDeferredMipGen(c.texture.slot_index);
            }
            install();
        }
        tr.noteTextureResident(
            c.texture.slot_index,
            replacement ? c.logical_base_mip : 0u);
        return FinalizeDisposition::Succeeded;
    }

    GeneralRenderServer::Impl::FinalizeDisposition GeneralRenderServer::Impl::finalizeTextureCubeCompletion(
        TransferCompletion& c, bool needs_qfot,
        uint32_t src_family, uint32_t dst_family)
    {
        auto& tr = render_ctx_->globalRegistry().must<TextureResources>();

        auto& bcs = tr.bindlessSetCube();
        // Same generation guard as the 2D path — don't finalize into a recycled slot,
        // freeing GPU objects + staging on drop.
        if (!bcs.isTextureAlive(SlotHandle{c.texture.slot_index, c.resource_gen}))
        {
            freeCompletionTextureGpu(c);
            return FinalizeDisposition::Failed;
        }
        if (!c.gpu_copy_recorded)
        {
            bcs.finalizeTransferredTexture(
                c.texture.slot_index, c.texture.image, c.texture.image_alloc,
                c.texture.view, c.texture.sampler, c.texture.format,
                c.texture.mip_levels, c.texture.array_layers,
                c.texture.width, c.texture.height);
            BindlessCombinedSet::PendingStagingTexture st{};
            st.stg_buf     = c.stg_buf;
            st.stg_size    = c.stg_size;
            st.slot_index  = c.texture.slot_index;
            st.do_mips     = false;
            st.is_cube     = true;
            // Use the worker's validated per-face byte size: the old
            // width*height*4 assumed RGBA8 and mis-strided every non-RGBA8 / BC cube
            // (faces 2..6 read from the wrong offset → corruption / OOB).
            st.face_stride = c.texture.face_stride;
            bcs.pushStagingTextureCopy(st);
        }
        else
        {
            if (needs_qfot)
            {
                bcs.pushImageAcquireBarrier(
                    c.texture.image, c.texture.mip_levels, c.texture.array_layers,
                    c.texture.needs_mip_gen
                        ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    src_family, dst_family);
            }
            bcs.finalizeTransferredTexture(
                c.texture.slot_index, c.texture.image, c.texture.image_alloc,
                c.texture.view, c.texture.sampler, c.texture.format,
                c.texture.mip_levels, c.texture.array_layers,
                c.texture.width, c.texture.height);
        }
        return FinalizeDisposition::Succeeded;
    }

    // ── Dispatch table for completion finalization ────────────────────────

    void GeneralRenderServer::Impl::finalizeCompletion(
        TransferCompletion& c, bool needs_qfot,
        uint32_t src_family, uint32_t dst_family)
    {
        using Kind = TransferCompletion::Kind;

        // FAILURE terminal state: an async upload bailed out (cancel /
        // invalid input / unknown format / Vulkan/VMA/staging/submit failure). It
        // carries no GPU objects to finalize — settle the client request with a
        // non-zero status and reclaim the reserved bindless slot, so it is neither
        // leaked nor left hanging forever.
        if (c.failed)
        {
            if (c.kind == Kind::Texture2DReplacement)
            {
                auto& tr = render_ctx_->globalRegistry().must<TextureResources>();
                tr.endMipReplacement(TextureHandle{
                    c.resource_handle, c.resource_gen});
            }
            if (c.request_id != UINT32_MAX)
                pending_deferred_replies_.push_back(
                    {c.request_id, c.kind, c.resource_handle, c.resource_gen,
                     c.logical_base_mip, /*status=*/1u});
            reclaimReservedSlot(c);
            return;
        }

        using FinalizeFn = FinalizeDisposition (Impl::*)(TransferCompletion&, bool, uint32_t, uint32_t);

        static constexpr FinalizeFn kDispatch[] = {
            &Impl::finalizeMeshCompletion,       // Kind::MeshBuffer  = 0
            &Impl::finalizeTexture2DCompletion,   // Kind::Texture2D   = 1
            &Impl::finalizeTextureCubeCompletion,  // Kind::TextureCube = 2
            &Impl::finalizeTexture2DCompletion,   // Texture2DReplacement = 3
        };
        static_assert(static_cast<int>(Kind::MeshBuffer)  == 0);
        static_assert(static_cast<int>(Kind::Texture2D)   == 1);
        static_assert(static_cast<int>(Kind::TextureCube)  == 2);
        static_assert(static_cast<int>(Kind::Texture2DReplacement) == 3);

        const bool needs_graphics_finalize =
            !c.gpu_copy_recorded || needs_qfot ||
            ((c.kind == Kind::Texture2D ||
              c.kind == Kind::Texture2DReplacement) &&
             c.texture.needs_mip_gen);

        const FinalizeDisposition disp =
            (this->*kDispatch[static_cast<int>(c.kind)])(c, needs_qfot, src_family, dst_family);

        if (disp == FinalizeDisposition::Succeeded)
        {
            if (c.request_id != UINT32_MAX)
            {
                DeferredReplyEntry reply{
                    c.request_id,
                    c.kind,
                    c.resource_handle,
                    c.resource_gen,
                    c.logical_base_mip,
                    0u};
                if (needs_graphics_finalize)
                {
                    graphics_finalize_reply_batch_.push_back(reply);
                }
                else
                    pending_deferred_replies_.push_back(reply);
            }
            if (c.stg_buf != VK_NULL_HANDLE)
            {
                if (needs_graphics_finalize)
                    graphics_finalize_staging_batch_.emplace_back(
                        dev_ctx_->vmaAllocator(), c.stg_buf, c.stg_alloc);
                else
                    vmaDestroyBuffer(
                        dev_ctx_->vmaAllocator(), c.stg_buf, c.stg_alloc);
            }
            graphics_finalize_required_ =
                graphics_finalize_required_ || needs_graphics_finalize;
            if (c.retained_batch_slot != UINT32_MAX)
            {
                if (needs_graphics_finalize)
                    graphics_finalize_slot_batch_.push_back(
                        c.retained_batch_slot
                    );
                else
                    transfer_pipeline_->releaseAfterGraphicsAcquire(
                        c.retained_batch_slot
                    );
                c.retained_batch_slot = UINT32_MAX;
            }
        }
        else
        {
            // Failed: the finalizer DROPPED the completion (stale / recycled slot) and
            // already freed both its GPU objects and its staging buffer. Send a
            // null-handle failure reply; do NOT retire the (already-destroyed) staging
            // again — that was the double-free — and do NOT reclaim the slot (it was
            // recycled by whoever removed it).
            if (c.request_id != UINT32_MAX)
                pending_deferred_replies_.push_back({
                    c.request_id, c.kind, c.resource_handle, c.resource_gen,
                    c.logical_base_mip, /*status=*/1u});
            if (c.retained_batch_slot != UINT32_MAX)
            {
                transfer_pipeline_->releaseAfterGraphicsAcquire(
                    c.retained_batch_slot
                );
                c.retained_batch_slot = UINT32_MAX;
            }
        }
    }

    void GeneralRenderServer::Impl::reclaimReservedSlot(const TransferCompletion& c)
    {
        // Return a reserved-but-never-finalized bindless slot to its free list.
        // removeTexture() is the existing recycle path and is safe here: the slot was
        // only reserved (allocateSlotDeferred wrote just the null descriptor), so its
        // CombinedSlot is still {} and removeTexture retires null GPU objects. Meshes
        // carry no bindless slot — their arena range is owned by the mesh resource.
        if (c.kind == TransferCompletion::Kind::MeshBuffer)
        {
            // Roll back the reserved mesh arena range (allocateOnly) that a failed async
            // upload never finalized — otherwise repeated failures exhaust mesh handles
            // and the VBO/IBO arena. destroy() defers the range's return past
            // frames-in-flight, exactly like a normal removal.
            if (auto* mr = render_ctx_->globalRegistry().find<MeshResources>())
                mr->destroy(MeshHandle{c.resource_handle, c.resource_gen});
            return;
        }
        auto& tr = render_ctx_->globalRegistry().must<TextureResources>();
        const SlotHandle h{c.resource_handle, c.resource_gen};
        switch (c.kind)
        {
        case TransferCompletion::Kind::Texture2D:   tr.bindlessSet2D().removeTexture(h);   break;
        case TransferCompletion::Kind::TextureCube: tr.bindlessSetCube().removeTexture(h); break;
        case TransferCompletion::Kind::Texture2DReplacement:
            tr.endMipReplacement(TextureHandle{h.index, h.gen});
            break;
        case TransferCompletion::Kind::MeshBuffer:  break; // handled above
        }
    }

    void GeneralRenderServer::Impl::freeCompletionTextureGpu(const TransferCompletion& c)
    {
        auto     vma = dev_ctx_->vmaAllocator();
        VkDevice dev = dev_ctx_->logicalDevice();
        vkDestroyImageView(dev, c.texture.view, nullptr);
        vkDestroySampler(dev, c.texture.sampler, nullptr);
        if (c.texture.image != VK_NULL_HANDLE)
        {
            vmaDestroyImage(vma, c.texture.image, c.texture.image_alloc);
        }
        if (c.stg_buf != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(vma, c.stg_buf, c.stg_alloc);
        }
    }

    void GeneralRenderServer::Impl::destroyUnfinalizedCompletion(const TransferCompletion& c)
    {
        // MUST branch on kind: reading c.texture.* on a mesh completion mis-reads the
        // union. Mesh holds only staging; texture/cube hold image/view/sampler + staging.
        if (c.kind == TransferCompletion::Kind::MeshBuffer)
        {
            if (c.stg_buf != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(dev_ctx_->vmaAllocator(), c.stg_buf, c.stg_alloc);
            }
        }
        else
        {
            freeCompletionTextureGpu(c);
        }
    }

    bool GeneralRenderServer::Impl::pollGraphicsFinalizes(
        std::uint64_t gpu_value)
    {
        const std::size_t before = pending_graphics_finalizes_.size();
        const VkDevice device = dev_ctx_->logicalDevice().handle();
        const VkCommandPool pool = res_ctx_->commandPool();
        std::erase_if(
            pending_graphics_finalizes_,
            [&](PendingGraphicsFinalize& pending)
            {
            if (pending.timeline_value > gpu_value)
            {
                return false;
            }
                if (pending.command_buffer != VK_NULL_HANDLE)
                    vkFreeCommandBuffers(
                        device, pool, 1, &pending.command_buffer);
                pending_deferred_replies_.insert(
                    pending_deferred_replies_.end(),
                    std::make_move_iterator(pending.replies.begin()),
                    std::make_move_iterator(pending.replies.end()));
                return true;
            });
        return pending_graphics_finalizes_.size() != before;
    }

    bool GeneralRenderServer::Impl::submitGraphicsFinalizeBatch()
    {
        if (!graphics_finalize_required_)
        {
            return false;
        }

        graphics_finalize_required_ = false;
        const VkDevice device = dev_ctx_->logicalDevice().handle();
        const VkCommandPool pool = res_ctx_->commandPool();

        VkCommandBufferAllocateInfo allocate{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;

        const auto fail_batch = [&]()
        {
            if (command_buffer != VK_NULL_HANDLE)
            {
                vkFreeCommandBuffers(device, pool, 1, &command_buffer);
            }
            for (auto& reply : graphics_finalize_reply_batch_)
            {
                TransferCompletion completion{};
                completion.kind = reply.kind;
                completion.resource_handle = reply.resource_index;
                completion.resource_gen = reply.resource_gen;
                reclaimReservedSlot(completion);
                reply.status = 1u;
                pending_deferred_replies_.push_back(reply);
            }
            graphics_finalize_reply_batch_.clear();
            graphics_finalize_staging_batch_.clear();
            for (const auto slot : graphics_finalize_slot_batch_)
            {
                transfer_pipeline_->releaseAfterGraphicsAcquire(slot);
            }
            graphics_finalize_slot_batch_.clear();
            error_sink_.emit(
                renderError<err::device::VulkanObjectCreationFailed>(),
                RenderErrorEvent::kNoScene,
                current_stamp_.serial);
        };

        if (vkAllocateCommandBuffers(
                device, &allocate, &command_buffer) != VK_SUCCESS)
        {
            fail_batch();
            return true;
        }

        VkCommandBufferBeginInfo begin{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(command_buffer, &begin) != VK_SUCCESS)
        {
            fail_batch();
            return true;
        }

        if (auto* mesh = render_ctx_->globalRegistry().find<MeshResources>())
        {
            mesh->recordStagingCopies(command_buffer);
            mesh->recordAcquireBarriers(command_buffer);
        }
        auto& textures =
            render_ctx_->globalRegistry().must<TextureResources>();
        for (BindlessCombinedSet* set : {
                 &textures.bindlessSet2D(),
                 &textures.bindlessSetCube()})
        {
            set->recordStagingTextureCopies(command_buffer);
            set->recordAcquireBarriers(command_buffer);
            set->recordDeferredMipGens(command_buffer);
        }

        if (vkEndCommandBuffer(command_buffer) != VK_SUCCESS)
        {
            fail_batch();
            return true;
        }

        const auto timeline =
            transfer_pipeline_->submitGraphicsFinalize(command_buffer);
        if (!timeline)
        {
            fail_batch();
            return true;
        }

        // vkQueueSubmit2 has now queued every matching graphics acquire. Only
        // at this point may the transfer worker reset/reuse the command pools
        // that still contain their release barriers.
        for (const auto slot : graphics_finalize_slot_batch_)
        {
            transfer_pipeline_->releaseAfterGraphicsAcquire(slot);
        }
        graphics_finalize_slot_batch_.clear();

        for (const auto& reply : graphics_finalize_reply_batch_)
            transitionUpload(
                reply.request_id,
                reply.kind,
                reply.resource_index,
                reply.resource_gen,
                EUploadLifecycleState::GraphicsFinalizeSubmitted
            );

        pending_graphics_finalizes_.push_back(PendingGraphicsFinalize{
            .timeline_value = *timeline,
            .command_buffer = command_buffer,
            .replies = std::move(graphics_finalize_reply_batch_),
            .staging = std::move(graphics_finalize_staging_batch_),
        });
        graphics_finalize_reply_batch_.clear();
        graphics_finalize_staging_batch_.clear();
        return true;
    }

    // ── processUploadCompletions ──────────────────────────────────────────

    bool GeneralRenderServer::Impl::processUploadCompletions()
    {
        bool made_progress = false;
        // 0. Drain results produced since the last tick. In RECORD_ONLY mode
        //    drainResults also submits recorded command buffers; the render
        //    thread is the sole graphics-queue submitter, so this collides with
        //    nothing — frame submission runs on this same thread. This removes the
        //    cross-thread vkQueueSubmit2 race without any queue lock. The just-
        //    submitted completions haven't retired yet, so defer them into
        //    pending_completions_ exactly like freshly drained ones; step 3
        //    re-checks them against the timeline and finalizes once retired.
        if (transfer_pipeline_)
        {
            const uint32_t fn = transfer_pipeline_->drainResults(
                completion_buf_, kMaxDrainBatch);
            for (uint32_t i = 0; i < fn; ++i)
            {
                auto& completion = completion_buf_[i];
                if (observeTransferResult(completion))
                {
                    pending_completions_.push_back(completion);
                }
                else
                    destroyUnfinalizedCompletion(completion);
            }
            made_progress = fn != 0;
        }

        if (!transfer_pipeline_)
        {
            return made_progress;
        }

        // 2. Query GPU timeline value (non-blocking).
        uint64_t gpu_value = 0;
        const VkResult sem_res = vkGetSemaphoreCounterValue(
            dev_ctx_->logicalDevice().handle(),
            transfer_pipeline_->timelineSemaphore(),
            &gpu_value
        );
        if (sem_res != VK_SUCCESS)
        {
            // Don't silently treat the result as gpu_value==0 — that re-defers every
            // pending completion forever and never surfaces the failure, so client
            // upload futures hang. Report, then: on a transient error skip
            // finalization this tick and retry next tick; on device loss the
            // completions can never finalize, so fail all pending ones with an error
            // reply (status!=0) to unblock the waiting clients.
            //
            // 走自发上报而不是原先的 log-once:上报通道按同键合并计数,持续查不动
            // 会带着累加的 occurrences 一直出现,而 log-once 只说第一次。
            error_sink_.emit(renderError<err::frame::TimelineQueryFailed>(encodeVkResult(sem_res)),
                             RenderErrorEvent::kNoScene, current_stamp_.serial);
            if (sem_res == VK_ERROR_DEVICE_LOST)
            {
                // Device lost: no completion can ever finalize. For EVERY completion —
                // pending AND still queued in the ring, with or without a client request
                // — destroy its GPU objects (so nothing leaks), reclaim its reserved slot
                // / mesh range, and send a NULL-handle failure reply if a client waits.
                const auto abort_completion = [&](TransferCompletion& c)
                {
                    destroyUnfinalizedCompletion(c);
                    reclaimReservedSlot(c);
                    if (c.request_id == UINT32_MAX)
                    {
                        return;
                    }
                    pending_deferred_replies_.push_back(
                        {c.request_id, c.kind, c.resource_handle,
                         c.resource_gen, c.logical_base_mip, 1u});
                };
                for (auto& c : pending_completions_)
                {
                    abort_completion(c);
                }
                pending_completions_.clear();
                // Also drain the transfer→render SPSC — those requests would otherwise
                // never get a reply and their resources would leak.
                uint32_t dn;
                do {
                    dn = transfer_pipeline_->drainResults(
                        completion_buf_, kMaxDrainBatch);
                    for (uint32_t i = 0; i < dn; ++i)
                    {
                        auto& completion = completion_buf_[i];
                        if (observeTransferResult(completion))
                        {
                            abort_completion(completion);
                        }
                        else
                            destroyUnfinalizedCompletion(completion);
                    }
                } while (dn > 0);

                // Resources in these lists were already adopted by their
                // resource manager, but their success reply is gated by the
                // graphics-finalize timeline. Device loss makes that terminal
                // success impossible: retire CPU/VMA owners, return the resource
                // handle through the normal destroy path, and fail each request
                // exactly once.
                const VkDevice device =
                    dev_ctx_->logicalDevice().handle();
                const VkCommandPool pool = res_ctx_->commandPool();
                for (auto& pending : pending_graphics_finalizes_)
                {
                    if (pending.command_buffer != VK_NULL_HANDLE)
                        vkFreeCommandBuffers(
                            device, pool, 1, &pending.command_buffer);
                    for (auto& reply : pending.replies)
                    {
                        TransferCompletion completion{};
                        completion.kind = reply.kind;
                        completion.resource_handle = reply.resource_index;
                        completion.resource_gen = reply.resource_gen;
                        reclaimReservedSlot(completion);
                        reply.status = 1u;
                        pending_deferred_replies_.push_back(reply);
                    }
                }
                pending_graphics_finalizes_.clear();

                for (auto& reply : graphics_finalize_reply_batch_)
                {
                    TransferCompletion completion{};
                    completion.kind = reply.kind;
                    completion.resource_handle = reply.resource_index;
                    completion.resource_gen = reply.resource_gen;
                    reclaimReservedSlot(completion);
                    reply.status = 1u;
                    pending_deferred_replies_.push_back(reply);
                }
                graphics_finalize_reply_batch_.clear();
                graphics_finalize_staging_batch_.clear();
                for (const auto slot : graphics_finalize_slot_batch_)
                {
                    transfer_pipeline_->releaseAfterGraphicsAcquire(slot);
                }
                graphics_finalize_slot_batch_.clear();
                graphics_finalize_required_ = false;
            }
            return made_progress;
        }

        const bool needs_qfot =
            transfer_pipeline_->needsQueueFamilyOwnershipTransfer();
        const uint32_t src_family = transfer_pipeline_->transferFamily();
        const uint32_t dst_family = transfer_pipeline_->graphicsFamily();
        made_progress = pollGraphicsFinalizes(gpu_value) || made_progress;

        // 3. Re-check previously deferred completions.
        auto new_end = std::remove_if(
            pending_completions_.begin(), pending_completions_.end(),
            [&](TransferCompletion& c)
            {
                if (c.timeline_value <= gpu_value)
                {
                    finalizeCompletion(
                        c,
                        needs_qfot &&
                            c.requires_queue_family_ownership_transfer,
                        src_family,
                        dst_family
                    );
                    made_progress = true;
                    return true; // remove from pending
                }
                return false;
            }
        );
        pending_completions_.erase(new_end, pending_completions_.end());

        // 4. Drain new completions from the transfer→render SPSC.
        const uint32_t n = transfer_pipeline_->drainResults(
            completion_buf_, kMaxDrainBatch);
        made_progress = made_progress || n != 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            auto& c = completion_buf_[i];
            if (!observeTransferResult(c))
            {
                destroyUnfinalizedCompletion(c);
                continue;
            }
            if (c.timeline_value == 0 || c.timeline_value <= gpu_value)
            {
                finalizeCompletion(
                    c,
                    needs_qfot &&
                        c.requires_queue_family_ownership_transfer,
                    src_family,
                    dst_family
                );
            }
            else
                pending_completions_.push_back(c);
        }
        made_progress = submitGraphicsFinalizeBatch() || made_progress;
        return made_progress;
    }

    Expected<RenderTargetId> GeneralRenderServer::Impl::createSurfaceTargetInternal(
        RenderSurface&& surface, VkExtent2D extent)
    {
        // surface → swapchain → sem 环整体收进 PresentContext,由
        // Surface entry 拥有(per-target;主窗与副窗同构)。过渡期
        // surface_target_ 仍单指主窗(多窗化尚未做)。
        if (surfacePresent())
        {
            // 单主窗过渡约束:重复 attach 先拆旧(极少路径;泛化)。
            // (surface 由本函数按值接收,直接返回就会析构;reset() 只是把释放
            //  点说清楚 —— 它是我们拒绝了这次 attach,不是忘了处理。)
            surface.reset();
            return renderFailure<err::internal::Unspecified>();
        }

        auto pc = PresentContext::create(*res_ctx_, std::move(surface),
                                         extent, enable_vsync_);
        if (!pc)
        {
            return lux::cxx::unexpected(pc.error());
        }

        // Surface target entry(空合成链;绑定经 SetLayer/bindSwapchain 入链)。
        RenderTargetEntry e{};
        e.kind    = RenderTargetEntry::EKind::Surface;
        e.layout  = (*pc)->provider()->layout();
        e.present = std::move(*pc);
        targets_registry_.setSurfaceTarget(targets_registry_.insert(std::move(e)));
        return targets_registry_.surfaceTargetId();
    }

    Expected<void> GeneralRenderServer::attachToWindow(lux::window::LuxWindow& window)
    {
        // 宿主侧封装——与 CreateSurfaceTarget 命令共用同一条内部创建
        // 路径(surface 创建仍只发生在渲染线程:本函数约定在 tick 循环启动
        // 前于渲染线程调用;运行期动态开窗走命令面)。
        RenderSurface surface;
        if (!surface.init(window, impl_->inst_ctx_->instance()))
        {
            return renderFailure<err::internal::Unspecified>();
        }

        const auto fb = window.framebufferSize();
        auto r = impl_->createSurfaceTargetInternal(
            std::move(surface), VkExtent2D{fb.width, fb.height});
        if (!r)
        {
            return lux::cxx::unexpected(r.error());
        }
        return {};
    }

    // ── Standalone deferred-reply flush ────────────────────────────────
    void GeneralRenderServer::flushDeferredRepliesOnly()
    {
        auto& im = *impl_;
        const auto flush_lane = [&]<class ReplyRing>(
            ERequestLane lane,
            ReplyRing& responses,
            bool primary_publication_pending)
        {
            const bool has_lane = std::any_of(
                im.pending_deferred_replies_.begin(),
                im.pending_deferred_replies_.end(),
                [lane](const auto& reply)
                { return requestLane(reply.request_id) == lane; });
            if (!has_lane)
            {
                return;
            }

            // BoundedSpscFrameRing::tryBeginWrite() is idempotent for its
            // producer. Entering it here while the primary server owns an
            // unpublished slot would reuse and overwrite that slot, then leave
            // the primary server retrying a publication it no longer owns.
            if (primary_publication_pending)
            {
                return;
            }

            auto* slot = responses.tryBeginWrite();
            if (slot == nullptr)
            {
                return;
            }

            FrameReplyBuilder<64> builder(*slot);
            builder.begin();
            for (const auto& dr : im.pending_deferred_replies_)
            {
                if (requestLane(dr.request_id) != lane)
                {
                    continue;
                }
                using Kind = TransferCompletion::Kind;
                switch (dr.kind)
                {
                case Kind::MeshBuffer:
                    builder.push<MeshUploadedReply>(
                        type_ids::ReplyMeshUploaded,
                        MeshUploadedReply{
                            dr.status ? RMeshHandle{} : RMeshHandle{
                                dr.resource_index, dr.resource_gen},
                            dr.status},
                        0, dr.request_id);
                    break;
                case Kind::Texture2D:
                    builder.push<Texture2DCreatedReply>(
                        type_ids::ReplyTexture2DCreated,
                        Texture2DCreatedReply{
                            dr.status ? RTextureHandle{} : RTextureHandle{
                                dr.resource_index, dr.resource_gen},
                            dr.status},
                        0, dr.request_id);
                    break;
                case Kind::Texture2DReplacement:
                    builder.push<TextureMipRangeReplacedReply>(
                        type_ids::ReplyTextureMipRangeReplaced,
                        TextureMipRangeReplacedReply{
                            RTextureHandle{
                                dr.resource_index, dr.resource_gen},
                            dr.logical_base_mip,
                            dr.status},
                        0,
                        dr.request_id);
                    break;
                case Kind::TextureCube:
                    builder.push<CubeTextureCreatedReply>(
                        type_ids::ReplyCubeTextureCreated,
                        CubeTextureCreatedReply{
                            dr.status ? RTextureHandle{} : RTextureHandle{
                                dr.resource_index, dr.resource_gen},
                            dr.status},
                        0, dr.request_id);
                    break;
                }
            }

            if (!responses.publishWrite())
            {
                return;
            }
            for (const auto& reply : im.pending_deferred_replies_)
                if (requestLane(reply.request_id) == lane)
                {
                    im.settleUploadReply(reply);
                }
            std::erase_if(
                im.pending_deferred_replies_,
                [lane](const auto& reply)
                { return requestLane(reply.request_id) == lane; });
            channelSync().notifyReplyProduced();
        };

        flush_lane(
            ERequestLane::FRAME,
            channel().responses,
            hasPendingReplyPublication()
        );
        flush_lane(
            ERequestLane::UPLOAD,
            upload_server_->endpoint().responses,
            upload_server_->hasPendingReplyPublication()
        );
    }

    void GeneralRenderServer::reportError(const RenderError& error, std::uint32_t scene_index) noexcept
    {
        impl_->error_sink_.emit(error, scene_index, impl_->current_stamp_.serial);
    }

    void GeneralRenderServer::flushErrorEvents()
    {
        auto& sink = impl_->error_sink_;

        // 先把其它线程投进来的校验层消息折进 sink。排空发生在渲染线程,所以
        // sink 自始至终只有一个写者 —— 合并计数的线性扫描因此是安全的。
        auto& ring = impl_->validation_ring_;
        ring.drain([&sink, serial = impl_->current_stamp_.serial](const ValidationEvent& e) {
            sink.emit(renderError<err::validation::LayerReport>(e.severity, e.fingerprint),
                      RenderErrorEvent::kNoScene, serial);
        });
        if (const std::uint32_t lost = ring.dropped(); lost != 0)
        {
            // 环满/槽位耗尽丢掉的那些。它们没有各自的类型与实参可言,只报一条
            // 「丢了多少」—— 比默默吞掉强,也不假装还知道丢的是什么。
            sink.emit(renderError<err::validation::EventsDropped>(lost),
                      RenderErrorEvent::kNoScene, impl_->current_stamp_.serial);
            ring.clearDropped();
        }

        if (sink.empty() && sink.dropped() == 0)
        {
            return;
        }

        if (hasPendingReplyPublication())
        {
            return;
        }

        auto* slot = channel().responses.tryBeginWrite();
        if (slot == nullptr)
        {
            return;   // 回复环满:事件留在 sink 里,下 tick 重试
        }

        const auto events = sink.pending();

        FrameReplyBuilder<64> builder(*slot);
        builder.begin();   // completion-only frame (no matching request)
        builder.push<ErrorEventBatchReply>(
            type_ids::ReplyErrorEventBatch,
            ErrorEventBatchReply{
                .count   = static_cast<std::uint32_t>(events.size()),
                .dropped = sink.dropped(),
            });
        for (const RenderErrorEvent& event : events)
        {
            builder.push<RenderErrorEvent>(type_ids::ReplyErrorEvent, event);
        }

        if (!channel().responses.publishWrite())
        {
            return;   // 同上:没推出去就不清,信息不丢
        }

        sink.clear();
        channelSync().notifyReplyProduced();
    }

    // Advance + flush in-flight async readbacks (ReadbackTargetAsync). The GPU
    // state machine (settle/submit/poll/finish) runs in advancePendingReadbacks;
    // here we send the deferred replies (by request_id) for resolved entries and
    // drop them. Ring-full / closed reply channel → retry next tick.
    bool GeneralRenderServer::stepPendingSurfaceReleases()
    {
        auto& im = *impl_;
        if (im.pending_surface_releases_.empty())
        {
            return true;
        }

        // 阶段一:fence 水位越过退休阈值 → 在飞帧全部走完,拆派生链
        // (sems → swapchain → surface,PresentContext 析构一体完成;呈现
        // 早在受理时已停——surface_target_ 已清,后续 tick 不再 acquire)。
        //
        // 水位不足时主动排干,而不是等下个 tick:阈值是"受理时已提交的最后
        // 一帧",waitAllFences 等的全是已提交帧的 fence,故等待有界(≤ fif 帧)
        // 且等完必然越过阈值。惰性等待依赖后续流量推水位——宿主关窗后 await
        // 回执、不再提交任何帧,服务端等请求、客户端等回执,两端互等死锁
        // (桌面 player 优雅退出实测挂死;Android TERM_WINDOW 同构)。
        // Surface 销毁是窗口生命周期事件,频率极低,阻塞几毫秒可接受。
        uint64_t gpu_completed = im.frame_driver_
            ? im.frame_driver_->gpuCompletedSerial() : im.current_stamp_.serial;
        if (im.frame_driver_)
        {
            const bool watermark_short = std::any_of(
                im.pending_surface_releases_.begin(),
                im.pending_surface_releases_.end(),
                [&](const Impl::PendingSurfaceRelease& r)
                { return !r.torn_down && gpu_completed < r.retire_serial; });
            if (watermark_short)
            {
                auto waited = im.frame_driver_->waitAllFences();
                if (!waited)
                {
                    return stopAfterFrameError(waited.error(), 2u);
                }
                gpu_completed = im.frame_driver_->gpuCompletedSerial();
            }
        }
        for (auto& r : im.pending_surface_releases_)
        {
            if (r.torn_down || gpu_completed < r.retire_serial)
            {
                continue;
            }
            if (r.ctx)
            {
                auto closed = r.ctx->close();
                if (!closed)
                {
                    return stopAfterFrameError(closed.error(), 3u);
                }
            }
            r.ctx.reset();
            if (r.on_teardown)
            {
                r.on_teardown();          // 副视口顶点环/vd 等随拆收尾
                r.on_teardown = nullptr;
            }
            r.torn_down = true;
        }

        // 阶段二:按 request_id 送 TargetReleased 延迟回执(环满下帧重试;
        // request_id == 0 的内部释放不发回执,拆完即除名)。
        bool any_reply = false;
        for (const auto& r : im.pending_surface_releases_)
            if (r.torn_down && r.request_id != 0) { any_reply = true; break; }

        if (any_reply)
        {
            if (control_server_->hasPendingReplyPublication())
            {
                return true;
            }

            auto& responses = control_server_->endpoint().responses;
            auto* slot = responses.tryBeginWrite();
            if (!slot)
            {
                return true;   // 环满——含内部释放在内统一下帧再清
            }

            FrameReplyBuilder<64> builder(*slot);
            builder.begin();
            for (const auto& r : im.pending_surface_releases_)
                if (r.torn_down && r.request_id != 0)
                    builder.push<TargetReleasedReply>(
                        type_ids::ReplyTargetReleased,
                        TargetReleasedReply{r.target, 0u}, 0, r.request_id);

            if (!responses.publishWrite())
            {
                return true;
            }
            channelSync().notifyReplyProduced();
        }

        im.pending_surface_releases_.erase(
            std::remove_if(im.pending_surface_releases_.begin(),
                           im.pending_surface_releases_.end(),
                           [](const Impl::PendingSurfaceRelease& r) { return r.torn_down; }),
            im.pending_surface_releases_.end());
        return true;
    }

    // ── tick ─────────────────────────────────────────────────────────────

    void GeneralRenderServer::Impl::beginTickFrame()
    {
        // FrameDriver::beginFrame() has already waited this slot's fence — feed
        // the fence-proven completion watermark to the renderer BEFORE its
        // beginFrame collects deferred destroys, then free our own tagged
        // deferred lists against the same watermark. (Serial arithmetic is NOT
        // sound here: serials advance on non-submitting ticks.)
        const uint64_t gpu_completed = frame_driver_
            ? frame_driver_->gpuCompletedSerial()
            : current_stamp_.serial;   // driverless ⇒ no GPU work ever submitted
        renderer_->setGpuCompletedSerial(gpu_completed);
        frame_orchestrator_.beginFrame(*renderer_);
        std::erase_if(async_deferred_staging_,
                      [&](const auto& e) { return e.first <= gpu_completed; });
        targets_registry_.collectRetiredPools(gpu_completed);

        VRAMBudgetGuard budget(dev_ctx_->vmaAllocator());
        auto snap = budget.snapshot();
        if (snap.nearFull(0.95f))
        {
            expansion_suppressed_ = true;
        }
        else if (!snap.nearFull(0.85f))
        {
            expansion_suppressed_ = false;
        }
    }

    void GeneralRenderServer::Impl::endTickFrame(bool uploads_recorded)
    {
        // Move staging buffers accumulated during drain into the FIF ring now,
        // after runUploadPhase() has recorded all copies that read them.
        // beginTickFrame()'s .clear() fires only at next reuse of this slot
        // (after the slot fence is waited), so the GPU has finished by then.
        // Skip on upload-less ticks: StagingOnly copy records are still queued,
        // so the buffers must outlive the future tick that records them.
        if (uploads_recorded)
        {
            for (auto& sb : staging_pending_this_tick_)
            {
                async_deferred_staging_.emplace_back(current_stamp_.serial, std::move(sb));
            }
            staging_pending_this_tick_.clear();
        }

        frame_orchestrator_.endFrame(*renderer_);
    }

    // ─────────────────────────────────────────────────────────────────────
    //  一 tick 的三段(派生服务器的扩展面)
    // ─────────────────────────────────────────────────────────────────────
    //
    // 拆三段而不是一次调用,是因为记账有硬时序:staging 退休必须在
    // beginRenderFrame 等到栅栏之后、渲染开始之前。阶段边界正落在这些约束上,
    // 所以不需要发明任何回调钩子 —— 派生类按同样顺序调用,在段间插自己的事。

    bool GeneralRenderServer::drainTick()
    {
        auto& im = *impl_;

        // 帧戳必须先于排水生成,处理器才能看到正确的 FIF 槽位。
        // image_index 此刻还不知道(acquire 在后面),beginRenderFrame 里补。
        if (!RenderTickPipeline::runTick(
                im.frame_orchestrator_, im.current_stamp_,
                [&]()
                {
                    for (;;)
                    {
                        bool handled_non_frame_work = false;
                        while (control_server_->drainAndDispatch(impl_.get()))
                        {
                            handled_non_frame_work = true;
                        }
                        while (upload_server_->drainAndDispatch(impl_.get()))
                        {
                            handled_non_frame_work = true;
                        }
                        if (im.processUploadCompletions())
                        {
                            handled_non_frame_work = true;
                        }
                        // DestroyTarget is a control operation, while surface
                        // teardown used to advance only after a frame request.
                        // A closing host submits no further frame and waits for
                        // TargetReleased, leaving both sides asleep forever.
                        // Advance the fence-proven release state machine in the
                        // same drain turn that accepted the control request.
                        if (!stepPendingSurfaceReleases())
                        {
                            return false;
                        }
                        if (handled_non_frame_work)
                        {
                            flushDeferredRepliesOnly();
                        }
                        if (drainRequest())
                        {
                            return true;
                        }
                        if (channelSync().isStopping())
                        {
                            return false;
                        }

                        const auto observed = channelSync().work_epoch.load(
                            std::memory_order_acquire);

                        // Close the publication-before-wait race for every
                        // source. A producer that wins after this check changes
                        // the epoch, so atomic::wait returns immediately.
                        if (control_server_->drainAndDispatch(impl_.get()))
                        {
                            continue;
                        }
                        if (upload_server_->drainAndDispatch(impl_.get()))
                        {
                            continue;
                        }
                        if (im.processUploadCompletions())
                        {
                            flushDeferredRepliesOnly();
                            continue;
                        }
                        if (drainRequest())
                        {
                            return true;
                        }
                        if (channelSync().isStopping())
                        {
                            return false;
                        }
                        channelSync().work_epoch.wait(
                            observed, std::memory_order_acquire);
                    }
                }, 0
            ))
        {
            return false;
        }

        // 与新请求无关地冲刷待发的延迟回复,以及本 tick 汇集到的自发上报。
        flushDeferredRepliesOnly();
        flushErrorEvents();
        return true;
    }

    GeneralRenderServer::ETickStage
    GeneralRenderServer::beginRenderTick(FrameTickState& fs)
    {
        auto& im = *impl_;
        auto& orch = im.frame_orchestrator_;
        auto start_result = orch.beginRenderFrame(
            im.targets(),
            im.frame_driver_.get(),
            fs
        );
        if (!start_result)
        {
            // A frame-slot Vulkan failure is not a retryable swapchain skip. In
            // particular, reset-fence may already have made the slot fence
            // unsignalled with no future submit able to signal it. Publish the
            // structured error once, then stop both General and UI server loops.
            (void)stopAfterFrameError(start_result.error(), 0u);
            return ETickStage::Failed;
        }
        const auto start = *start_result;
        if (start == FrameOrchestrator::EFrameStart::NoTarget)
        {
            return ETickStage::NoTarget;   // 未开帧,无需收尾记账
        }

        im.current_stamp_ = orch.stamp();   // beginRenderFrame 里已 patch image_index

        // 栅栏已等到(FrameDriver::beginFrame 内),此刻才可退休本槽的
        // staging/上传记账并推进场景帧态。
        im.beginTickFrame();

        if (start == FrameOrchestrator::EFrameStart::Skip)
        {
            im.endTickFrame(false);
            return ETickStage::Skipped;    // 最小化 / 重建失败 —— 跳过本帧
        }

        return ETickStage::Ready;
    }

    void GeneralRenderServer::renderRenderTick(FrameTickState& fs)
    {
        auto& im = *impl_;
        im.frame_orchestrator_.renderTargets(
            im.targets(), *im.renderer_, im.scene_view_batch_, fs);
    }

    bool GeneralRenderServer::endRenderTick(FrameTickState& fs)
    {
        auto& im = *impl_;
        auto ended = im.frame_orchestrator_.endRenderFrame(
            im.targets(),
            im.frame_driver_.get(),
            fs
        );
        if (!ended)
        {
            return stopAfterFrameError(ended.error(), 1u);
        }

        // 两阶段销毁后半程:Surface 拆除步进 + TargetReleased 延迟回执。
        if (!stepPendingSurfaceReleases())
        {
            return false;
        }

        // 异步回读:结算 / 提交 / 轮询 + 发延迟回复。放在结帧之后,好让刚结算
        // 的拷贝在图形队列上排在本 tick 的渲染之后(与同步处理器同理)。
        pollPendingReadbacks();

        im.endTickFrame(fs.rt.primary_cmd != VK_NULL_HANDLE);
        return true;
    }

    bool GeneralRenderServer::stopAfterFrameError(
        const RenderError& error,
        std::uint32_t phase)
    {
        const auto terminal =
            isError<err::device::FrameLifecycleCallFailed>(error)
            ? error
            : isError<err::device::VulkanCallFailed>(error)
            ? renderError<err::device::FrameLifecycleCallFailed>(
                  phase,
                  error.args[0])
            : error;
        channelSync().publishTerminalError(terminal);
        reportError(terminal);
        flushErrorEvents();
        requestStop();
        return false;
    }

    bool GeneralRenderServer::tick()
    {
        if (!drainTick())
        {
            return false;
        }

        FrameTickState fs{};
        switch (beginRenderTick(fs))
        {
        case ETickStage::NoTarget:
            // 关掉最后一个 target 后所有后续 tick 都走这里,endRenderTick
            // 永不再跑——两阶段销毁的后半程必须在此步进,否则受理进来的
            // TargetReleased 永远发不出去(宿主关窗 await 即死等)。
            return stepPendingSurfaceReleases();
        case ETickStage::Skipped:
            // 最小化/重建窗口可以持续任意久；不能把 Surface release 的
            // fence 推进与“本帧是否拿到命令缓冲”绑在一起，否则关闭窗口的
            // TargetReleased 会在最小化期间永久饿死。
            return stepPendingSurfaceReleases();
        case ETickStage::Failed:   return false;
        case ETickStage::Ready:    break;
        }

        renderRenderTick(fs);
        return endRenderTick(fs);
    }

} // namespace lux::render
