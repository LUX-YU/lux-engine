#include <lux/engine/function/render/client/RenderControlSession.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

namespace lux::render
{
    RenderControlSession::RenderControlSession(
        std::shared_ptr<TRenderControlChannel<>> channel,
        std::shared_ptr<RenderChannelSync> sync
    )
        : channel_(std::move(channel)), sync_(std::move(sync))
    {}

    RenderControlSession::~RenderControlSession() = default;

    std::size_t RenderControlSession::pumpReplies(std::size_t budget)
    {
        const auto consumed = detail::pumpReplyEnvelopes(*channel_, *sync_, callbacks_, budget);
        return consumed;
    }

    bool RenderControlSession::waitAndPumpReplies()
    {
        if (channel_->responses.tryAcquireRead())
        {
            callbacks_.dispatchAll(channel_->responses.currentRead());
            sync_->notifyReplyConsumed();
            (void)pumpReplies();
            return true;
        }

        const auto observed = sync_->reply_epoch.load(std::memory_order_acquire);
        if (channel_->responses.tryAcquireRead())
        {
            callbacks_.dispatchAll(channel_->responses.currentRead());
            sync_->notifyReplyConsumed();
            (void)pumpReplies();
            return true;
        }
        if (sync_->isStopping())
        {
            return false;
        }
        sync_->reply_epoch.wait(observed, std::memory_order_acquire);
        if (sync_->isStopping())
        {
            return false;
        }
        (void)pumpReplies();
        return true;
    }

    bool RenderControlSession::publishPacket(TOperationPacket<>&& packet, bool blocking)
    {
        for (;;)
        {
            if (channel_->requests.tryPush(std::move(packet)) == lux::cxx::EQueuePushResult::ACCEPTED)
            {
                sync_->notifyRequestStateChanged();
                return true;
            }
            if (!blocking || sync_->isStopping())
            {
                return false;
            }

            (void)pumpReplies();
            const auto observed = sync_->work_epoch.load(std::memory_order_acquire);
            if (sync_->isStopping())
            {
                return false;
            }
            sync_->work_epoch.wait(observed, std::memory_order_acquire);
        }
    }

    TRenderRequest<SceneCreatedReply> RenderControlSession::createScene(const CreateSceneConfig& config)
    {
        return recordReply<SceneCreatedReply>([&](Builder& builder, auto callback) {
            CreateScenePayload payload{};
            if (config.name)
            {
                std::strncpy(payload.name, config.name, sizeof(payload.name) - 1);
            }
            payload.flags = config.flags;
            payload.lit_color_format = config.lit_color_format;
            payload.coordinate_page_size = config.coordinate_page_size;
            for (std::size_t i = 0; i < 3; ++i)
            {
                payload.scene_origin_page[i] = config.scene_origin_page[i];
            }
            builder.pushWithReply(opcodes::CommandOp, type_ids::CreateScene, payload, std::move(callback));
        });
    }

    TRenderRequest<SceneCreatedReply> RenderControlSession::createScene(const char* name, std::uint32_t flags)
    {
        return createScene(CreateSceneConfig{.name = name, .flags = flags});
    }

    TRenderRequest<GenericOkReply> RenderControlSession::destroyScene(RenderSceneId scene)
    {
        return recordReply<GenericOkReply>([&](Builder& builder, auto callback) {
            builder.pushWithReply(
                opcodes::CommandOp,
                type_ids::DestroyScene,
                DestroyScenePayload{scene},
                std::move(callback)
            );
        });
    }

    TRenderRequest<GenericOkReply> RenderControlSession::setActiveScene(RenderSceneId scene, bool enabled)
    {
        return recordReply<GenericOkReply>([&](Builder& builder, auto callback) {
            SetActiveScenePayload payload{};
            payload.scene_id = scene;
            payload.enabled = enabled;
            builder.pushWithReply(opcodes::CommandOp, type_ids::SetActiveScene, payload, std::move(callback));
        });
    }

    TRenderRequest<ViewCreatedReply> RenderControlSession::addView(
        RenderSceneId scene,
        lux::math::Extent2u extent,
        const char* name
    )
    {
        return recordReply<ViewCreatedReply>([&](Builder& builder, auto callback) {
            AddViewPayload payload{};
            payload.scene_id = scene;
            payload.extent = extent;
            if (name)
            {
                std::strncpy(payload.name, name, sizeof(payload.name) - 1);
            }
            builder.pushWithReply(opcodes::CommandOp, type_ids::AddView, payload, std::move(callback));
        });
    }

    TRenderRequest<GenericOkReply> RenderControlSession::removeView(RenderSceneId scene, ViewHandle view)
    {
        return recordReply<GenericOkReply>([&](Builder& builder, auto callback) {
            RemoveViewPayload payload{};
            payload.scene_id = scene;
            payload.view = view;
            builder.pushWithReply(opcodes::CommandOp, type_ids::RemoveView, payload, std::move(callback));
        });
    }

    TRenderRequest<TargetReadyReply> RenderControlSession::createOffscreenRenderTarget(
        lux::math::Extent2u extent,
        std::uint32_t flags
    )
    {
        return recordReply<TargetReadyReply>([&](Builder& builder, auto callback) {
            CreateOffscreenTargetPayload payload{};
            payload.extent = extent;
            payload.flags = flags;
            builder.pushWithReply(opcodes::CommandOp, type_ids::CreateOffscreenTarget, payload, std::move(callback));
        });
    }

    TRenderRequest<TargetReadyReply> RenderControlSession::createSurfaceRenderTarget(
        std::uint64_t native_window_handle,
        lux::math::Extent2u extent
    )
    {
        return recordReply<TargetReadyReply>([&](Builder& builder, auto callback) {
            CreateSurfaceTargetPayload payload{};
            payload.native_window_handle = native_window_handle;
            payload.extent = extent;
            builder.pushWithReply(opcodes::CommandOp, type_ids::CreateSurfaceTarget, payload, std::move(callback));
        });
    }

    TRenderRequest<TargetReleasedReply> RenderControlSession::destroyRenderTarget(RenderTargetId target)
    {
        return recordReply<TargetReleasedReply>([&](Builder& builder, auto callback) {
            builder.pushWithReply(
                opcodes::CommandOp,
                type_ids::DestroyTarget,
                DestroyTargetPayload{target},
                std::move(callback)
            );
        });
    }

    TRenderRequest<GenericOkReply> RenderControlSession::setLayer(
        RenderTargetId target,
        std::uint32_t order,
        RenderSceneId scene,
        ViewHandle view
    )
    {
        return recordReply<GenericOkReply>([&](Builder& builder, auto callback) {
            SetLayerPayload payload{};
            payload.target = target;
            payload.order = order;
            payload.scene_id = scene;
            payload.view = view;
            builder.pushWithReply(opcodes::CommandOp, type_ids::SetLayer, payload, std::move(callback));
        });
    }

    TRenderRequest<GenericOkReply> RenderControlSession::switchViewTarget(
        RenderTargetId previous,
        RenderTargetId next,
        RenderSceneId scene,
        ViewHandle view,
        RenderSubmissionState production
    )
    {
        return recordReply<GenericOkReply>([&](Builder& builder, auto callback) {
            SwitchViewTargetPayload payload{previous, next, scene, view};
            if (production)
                payload.production_attachment = builder.emplaceAttachment<RenderSubmissionState>(
                    attachment_types::SubmissionState,
                    std::move(production)
                );
            builder.pushWithReply(opcodes::CommandOp, type_ids::SwitchViewTarget, payload, std::move(callback));
        });
    }

    void RenderControlSession::removeLayer(RenderTargetId target, std::uint32_t order)
    {
        (void)record([&](Builder& builder) {
            RemoveLayerPayload payload{};
            payload.target = target;
            payload.order = order;
            builder.push(opcodes::CommandOp, type_ids::RemoveLayer, payload);
        });
    }

    void RenderControlSession::resizeTarget(RenderTargetId target, lux::math::Extent2u extent)
    {
        static_cast<void>(requestResizeTarget(target, extent));
    }

    void RenderControlSession::bindSwapchain(RenderSceneId scene, ViewHandle view)
    {
        (void)record([&](Builder& builder) {
            BindSwapchainPayload payload{};
            payload.scene_id = scene;
            payload.view = view;
            builder.push(opcodes::CommandOp, type_ids::BindSwapchain, payload);
        });
    }

    TRenderRequest<TargetResizedReply> RenderControlSession::requestResizeTarget(
        RenderTargetId target,
        lux::math::Extent2u extent
    )
    {
        return recordReply<TargetResizedReply>([&](Builder& builder, auto callback) {
            ResizeTargetPayload payload{target, extent};
            builder.pushWithReply(opcodes::CommandOp, type_ids::ResizeTarget, payload, std::move(callback));
        });
    }

    TRenderRequest<ReadbackTargetReply> RenderControlSession::readbackTarget(
        RenderTargetId target,
        void* dst,
        std::size_t capacity,
        ETargetSlot slot
    )
    {
        return recordReply<ReadbackTargetReply>([&](Builder& builder, auto callback) {
            ReadbackTargetPayload payload{};
            payload.target = target;
            payload.dst_ptr = reinterpret_cast<std::uint64_t>(dst);
            payload.dst_capacity = static_cast<std::uint64_t>(capacity);
            payload.slot = static_cast<std::uint8_t>(slot);
            builder.pushWithReply(opcodes::CommandOp, type_ids::ReadbackTarget, payload, std::move(callback));
        });
    }

    TRenderRequest<ReadbackTargetReply> RenderControlSession::readbackTargetAsync(
        RenderTargetId target,
        void* dst,
        std::size_t capacity,
        std::uint32_t settle_frames,
        ETargetSlot slot
    )
    {
        return recordReply<ReadbackTargetReply>([&](Builder& builder, auto callback) {
            ReadbackTargetAsyncPayload payload{};
            payload.target = target;
            payload.dst_ptr = reinterpret_cast<std::uint64_t>(dst);
            payload.dst_capacity = static_cast<std::uint64_t>(capacity);
            payload.settle_frames = settle_frames;
            payload.slot = static_cast<std::uint8_t>(slot);
            builder.pushWithReply(opcodes::CommandOp, type_ids::ReadbackTargetAsync, payload, std::move(callback));
        });
    }

    TRenderRequest<RenderGraphDumpReply> RenderControlSession::dumpRenderGraph(
        RenderSceneId scene,
        void* dst,
        std::size_t capacity
    )
    {
        return recordReply<RenderGraphDumpReply>([&](Builder& builder, auto callback) {
            DumpRenderGraphPayload payload{};
            payload.scene_id = scene;
            payload.dst_ptr = reinterpret_cast<std::uint64_t>(dst);
            payload.dst_capacity = static_cast<std::uint64_t>(capacity);
            builder.pushWithReply(opcodes::CommandOp, type_ids::DumpRenderGraph, payload, std::move(callback));
        });
    }

    TRenderRequest<GpuTimingReply> RenderControlSession::queryGpuTiming(
        RenderSceneId scene,
        void* dst,
        std::size_t capacity
    )
    {
        return recordReply<GpuTimingReply>([&](Builder& builder, auto callback) {
            QueryGpuTimingPayload payload{};
            payload.scene_id = scene;
            payload.dst_ptr = reinterpret_cast<std::uint64_t>(dst);
            payload.dst_capacity = static_cast<std::uint64_t>(capacity);
            builder.pushWithReply(opcodes::CommandOp, type_ids::QueryGpuTiming, payload, std::move(callback));
        });
    }

    TRenderRequest<QueryFeatureParamsReply> RenderControlSession::queryFeatureParams(
        RenderSceneId scene,
        void* dst,
        std::size_t capacity
    )
    {
        return recordReply<QueryFeatureParamsReply>([&](Builder& builder, auto callback) {
            QueryFeatureParamsPayload payload{};
            payload.scene_id = scene;
            payload.dst_ptr = reinterpret_cast<std::uint64_t>(dst);
            payload.dst_capacity = static_cast<std::uint64_t>(capacity);
            builder.pushWithReply(opcodes::CommandOp, type_ids::QueryFeatureParams, payload, std::move(callback));
        });
    }

    TRenderRequest<DeviceCapsReply> RenderControlSession::queryDeviceCaps(DeviceCaps& output)
    {
        return recordReply<DeviceCapsReply>([&](Builder& builder, auto callback) {
            QueryDeviceCapsPayload payload{};
            payload.dst_ptr = reinterpret_cast<std::uint64_t>(&output);
            payload.dst_capacity = sizeof(DeviceCaps);
            builder.pushWithReply(opcodes::CommandOp, type_ids::QueryDeviceCaps, payload, std::move(callback));
        });
    }

    TRenderRequest<ShaderCompiledReply> RenderControlSession::compileShader(
        std::span<const std::byte> spirv,
        std::span<const std::byte> shader_info
    )
    {
        return recordReply<ShaderCompiledReply>([&](Builder& builder, auto callback) {
            CompileShaderPayload payload{};
            payload.spirv_data = builder.pushOwnedBytesCopy(spirv.data(), static_cast<std::uint32_t>(spirv.size()));
            if (!shader_info.empty())
            {
                payload.shader_info_data =
                    builder.pushOwnedBytesCopy(shader_info.data(), static_cast<std::uint32_t>(shader_info.size()));
            }
            builder.pushResource(type_ids::CompileShader, payload, std::move(callback));
        });
    }

    TRenderRequest<ShaderCompiledReply> RenderControlSession::compileShader(
        std::shared_ptr<const std::vector<std::byte>> spirv,
        std::shared_ptr<const std::vector<std::byte>> shader_info
    )
    {
        return recordReply<ShaderCompiledReply>([&](Builder& builder, auto callback) mutable {
            CompileShaderPayload payload{};
            const auto* spirv_data = spirv ? spirv->data() : nullptr;
            const auto spirv_size = spirv ? static_cast<std::uint32_t>(spirv->size()) : 0u;
            payload.spirv_data =
                builder.pushSharedBytes(std::static_pointer_cast<const void>(spirv), spirv_data, spirv_size);
            if (shader_info && !shader_info->empty())
            {
                payload.shader_info_data = builder.pushSharedBytes(
                    std::static_pointer_cast<const void>(shader_info),
                    shader_info->data(),
                    static_cast<std::uint32_t>(shader_info->size())
                );
            }
            builder.pushResource(type_ids::CompileShader, payload, std::move(callback));
        });
    }

    void RenderControlSession::destroyShader(ShaderHandle shader)
    {
        (void)record([&](Builder& builder) {
            builder.push(opcodes::ResourceOp, type_ids::DestroyShader, DestroyShaderPayload{shader});
        });
    }

    TRenderRequest<FeatureTypeRegisteredReply> RenderControlSession::registerFeatureType(
        const FeatureFactory& factory,
        std::shared_ptr<const void> module_lease
    )
    {
        return recordReply<FeatureTypeRegisteredReply>([&](Builder& builder, auto callback) {
            RegisterFeatureTypePayload payload{};
            payload.factory = factory;
            if (module_lease)
            {
                payload.module_lease_attachment = builder.template emplaceAttachment<std::shared_ptr<const void>>(
                    attachment_types::LifetimeLease,
                    std::move(module_lease)
                );
            }
            builder.pushWithReply(opcodes::CommandOp, type_ids::RegisterFeatureType, payload, std::move(callback));
        });
    }

    TRenderRequest<GenericOkReply> RenderControlSession::unregisterFeatureType(std::uint32_t feature_type_id)
    {
        return recordReply<GenericOkReply>([&](Builder& builder, auto callback) {
            builder.pushWithReply(
                opcodes::CommandOp,
                type_ids::UnregisterFeatureType,
                UnregisterFeatureTypePayload{feature_type_id},
                std::move(callback)
            );
        });
    }

    TRenderRequest<QueryTypeIdReply> RenderControlSession::queryTypeId(const char* name)
    {
        return recordReply<QueryTypeIdReply>([&](Builder& builder, auto callback) {
            QueryTypeIdPayload payload{};
            if (name)
            {
                std::strncpy(payload.name, name, sizeof(payload.name) - 1);
            }
            builder.pushWithReply(opcodes::CommandOp, type_ids::QueryTypeId, payload, std::move(callback));
        });
    }

    TRenderRequest<FeatureAddedReply> RenderControlSession::addFeatureRaw(
        RenderSceneId scene,
        std::uint32_t feature_type_id,
        std::span<const std::byte> config
    )
    {
        return addFeatureRaw(scene, feature_type_id, lux::cxx::SharedBytes<>::copyOf(config));
    }

    TRenderRequest<FeatureAddedReply> RenderControlSession::addFeatureRaw(
        RenderSceneId scene,
        std::uint32_t feature_type_id,
        lux::cxx::SharedBytes<> config
    )
    {
        return recordReply<FeatureAddedReply>([&](Builder& builder, auto callback) {
            const auto attachment = builder.pushSharedBytes(config).attachment_index;
            AddFeaturePayload payload{};
            payload.scene_id = scene;
            payload.feature_type_id = feature_type_id;
            payload.attachment_index = attachment;
            builder.pushWithReply(opcodes::CommandOp, type_ids::AddFeature, payload, std::move(callback));
        });
    }

    TRenderRequest<GenericOkReply> RenderControlSession::removeFeature(RenderSceneId scene, FeatureHandle feature)
    {
        return recordReply<GenericOkReply>([&](Builder& builder, auto callback) {
            RemoveFeaturePayload payload{};
            payload.scene_id = scene;
            payload.feature = feature;
            builder.pushWithReply(opcodes::CommandOp, type_ids::RemoveFeature, payload, std::move(callback));
        });
    }

    TRenderRequest<GenericOkReply> RenderControlSession::setFeatureEnabled(
        RenderSceneId scene,
        FeatureHandle feature,
        bool enabled
    )
    {
        return recordReply<GenericOkReply>([&](Builder& builder, auto callback) {
            SetFeatureEnabledPayload payload{};
            payload.scene_id = scene;
            payload.feature = feature;
            payload.enabled = enabled;
            builder.pushWithReply(opcodes::CommandOp, type_ids::SetFeatureEnabled, payload, std::move(callback));
        });
    }

    void RenderControlSession::destroyTexture(RTextureHandle handle)
    {
        send(opcodes::ResourceOp, type_ids::DestroyTexture, DestroyTexturePayload{handle});
    }

    void RenderControlSession::destroyCubeTexture(RTextureHandle handle)
    {
        send(opcodes::ResourceOp, type_ids::DestroyCubeTexture, DestroyCubeTexturePayload{handle});
    }

    bool RenderControlSession::canSubmit(std::size_t commands) const noexcept
    {
        return !sync_->isStopping() && commands <= channel_->requests.capacity() - channel_->requests.size();
    }

    void RenderControlSession::requestStop() noexcept
    {
        sync_->requestStop();
    }
} // namespace lux::render
