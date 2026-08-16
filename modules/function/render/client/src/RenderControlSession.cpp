#include <lux/engine/function/render/client/RenderControlSession.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

namespace lux::render
{
    RenderControlSession::RenderControlSession(
        std::shared_ptr<RenderControlChannel<>> channel,
        std::shared_ptr<RenderChannelSync> sync)
        : channel_(std::move(channel)), sync_(std::move(sync))
    {
    }

    std::size_t RenderControlSession::pumpReplies()
    {
        std::size_t acquired = 0u;
        while (channel_->responses.tryAcquireRead())
        {
            callbacks_.dispatchAll(channel_->responses.currentRead());
            sync_->notifyReplyConsumed();
            ++acquired;
        }
        return acquired;
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

        const auto observed =
            sync_->reply_epoch.load(std::memory_order_acquire);
        if (channel_->responses.tryAcquireRead())
        {
            callbacks_.dispatchAll(channel_->responses.currentRead());
            sync_->notifyReplyConsumed();
            (void)pumpReplies();
            return true;
        }
        if (sync_->isStopping())
            return false;
        sync_->reply_epoch.wait(observed, std::memory_order_acquire);
        if (sync_->isStopping())
            return false;
        (void)pumpReplies();
        return true;
    }

    bool RenderControlSession::publishPacket(
        OperationPacket<>&& packet,
        bool blocking
    )
    {
        for (;;)
        {
            if (channel_->requests.tryPush(std::move(packet)) ==
                lux::cxx::EQueuePushResult::ACCEPTED)
            {
                sync_->notifyRequestStateChanged();
                return true;
            }
            if (!blocking || sync_->isStopping())
                return false;

            (void)pumpReplies();
            const auto observed =
                sync_->work_epoch.load(std::memory_order_acquire);
            if (sync_->isStopping())
                return false;
            sync_->work_epoch.wait(observed, std::memory_order_acquire);
        }
    }

    RenderRequest<SceneCreatedReply> RenderControlSession::createScene(
        const CreateSceneConfig& config)
    {
        return recordReply<SceneCreatedReply>(
            [&](Builder& builder, auto callback)
            {
                CreateScenePayload payload{};
                if (config.name)
                    std::strncpy(payload.name, config.name,
                                 sizeof(payload.name) - 1);
                payload.flags = config.flags;
                payload.lit_color_format = config.lit_color_format;
                payload.coordinate_page_size = config.coordinate_page_size;
                for (std::size_t i = 0; i < 3; ++i)
                    payload.scene_origin_page[i] = config.scene_origin_page[i];
                builder.pushWithReply(
                    opcodes::CommandOp, type_ids::CreateScene,
                    payload, std::move(callback));
            });
    }

    RenderRequest<SceneCreatedReply> RenderControlSession::createScene(
        const char* name, std::uint32_t flags)
    {
        return createScene(CreateSceneConfig{.name = name, .flags = flags});
    }

    RenderSceneLease RenderControlSession::adoptScene(RenderSceneId scene) noexcept
    {
        return RenderSceneLease{*this, scene};
    }

    bool RenderControlSession::destroyScene(RenderSceneId scene)
    {
        return record([&](Builder& builder)
        {
            builder.push(opcodes::CommandOp, type_ids::DestroyScene,
                         DestroyScenePayload{scene});
        });
    }

    RenderRequest<GenericOkReply> RenderControlSession::setActiveScene(
        RenderSceneId scene, bool enabled)
    {
        return recordReply<GenericOkReply>(
            [&](Builder& builder, auto callback)
            {
                SetActiveScenePayload payload{};
                payload.scene_id = scene;
                payload.enabled = enabled;
                builder.pushWithReply(
                    opcodes::CommandOp, type_ids::SetActiveScene,
                    payload, std::move(callback));
            });
    }

    RenderRequest<ViewCreatedReply> RenderControlSession::addView(
        RenderSceneId scene, common::Size2D extent, const char* name)
    {
        return recordReply<ViewCreatedReply>(
            [&](Builder& builder, auto callback)
            {
                AddViewPayload payload{};
                payload.scene_id = scene;
                payload.extent = extent;
                if (name)
                    std::strncpy(payload.name, name,
                                 sizeof(payload.name) - 1);
                builder.pushWithReply(
                    opcodes::CommandOp, type_ids::AddView,
                    payload, std::move(callback));
            });
    }

    RenderViewLease RenderControlSession::adoptView(
        RenderSceneId scene, ViewHandle view,
        RenderViewReleaseObserver observer) noexcept
    {
        return RenderViewLease{*this, scene, view, std::move(observer)};
    }

    RenderRequest<GenericOkReply> RenderControlSession::removeView(
        RenderSceneId scene, ViewHandle view)
    {
        return recordReply<GenericOkReply>(
            [&](Builder& builder, auto callback)
            {
                RemoveViewPayload payload{};
                payload.scene_id = scene;
                payload.view = view;
                builder.pushWithReply(
                    opcodes::CommandOp, type_ids::RemoveView,
                    payload, std::move(callback));
            });
    }

    RenderRequest<TargetReadyReply>
    RenderControlSession::createOffscreenRenderTarget(
        common::Size2D extent, std::uint32_t flags)
    {
        return recordReply<TargetReadyReply>(
            [&](Builder& builder, auto callback)
            {
                CreateOffscreenTargetPayload payload{};
                payload.extent = extent;
                payload.flags = flags;
                builder.pushWithReply(
                    opcodes::CommandOp, type_ids::CreateOffscreenTarget,
                    payload, std::move(callback));
            });
    }

    RenderRequest<TargetReadyReply>
    RenderControlSession::createSurfaceRenderTarget(
        std::uint64_t native_window_handle, common::Size2D extent)
    {
        return recordReply<TargetReadyReply>(
            [&](Builder& builder, auto callback)
            {
                CreateSurfaceTargetPayload payload{};
                payload.native_window_handle = native_window_handle;
                payload.extent = extent;
                builder.pushWithReply(
                    opcodes::CommandOp, type_ids::CreateSurfaceTarget,
                    payload, std::move(callback));
            });
    }

    RenderTargetLease RenderControlSession::adoptTarget(
        RenderTargetId target, RenderTargetReleaseObserver observer) noexcept
    {
        return RenderTargetLease{*this, target, std::move(observer)};
    }

    RenderRequest<TargetReleasedReply>
    RenderControlSession::destroyRenderTarget(RenderTargetId target)
    {
        return recordReply<TargetReleasedReply>(
            [&](Builder& builder, auto callback)
            {
                builder.pushWithReply(
                    opcodes::CommandOp, type_ids::DestroyTarget,
                    DestroyTargetPayload{target}, std::move(callback));
            });
    }

    void RenderControlSession::setLayer(
        RenderTargetId target, std::uint32_t order,
        RenderSceneId scene, ViewHandle view)
    {
        (void)record([&](Builder& builder)
        {
            SetLayerPayload payload{};
            payload.target = target;
            payload.order = order;
            payload.scene_id = scene;
            payload.view = view;
            builder.push(opcodes::CommandOp, type_ids::SetLayer, payload);
        });
    }

    void RenderControlSession::removeLayer(
        RenderTargetId target, std::uint32_t order)
    {
        (void)record([&](Builder& builder)
        {
            RemoveLayerPayload payload{};
            payload.target = target;
            payload.order = order;
            builder.push(
                opcodes::CommandOp,
                type_ids::RemoveLayer,
                payload);
        });
    }

    void RenderControlSession::resizeTarget(
        RenderTargetId target, common::Size2D extent)
    {
        (void)record([&](Builder& builder)
        {
            ResizeTargetPayload payload{};
            payload.target = target;
            payload.new_extent = extent;
            builder.push(opcodes::CommandOp, type_ids::ResizeTarget, payload);
        });
    }

    void RenderControlSession::bindSwapchain(
        RenderSceneId scene, ViewHandle view)
    {
        (void)record([&](Builder& builder)
        {
            BindSwapchainPayload payload{};
            payload.scene_id = scene;
            payload.view = view;
            builder.push(opcodes::CommandOp, type_ids::BindSwapchain, payload);
        });
    }

    RenderRequest<ReadbackTargetReply> RenderControlSession::readbackTarget(
        RenderTargetId target, void* dst, std::size_t capacity, TargetSlot slot)
    {
        return recordReply<ReadbackTargetReply>(
            [&](Builder& builder, auto callback)
            {
                ReadbackTargetPayload payload{};
                payload.target = target;
                payload.dst_ptr = reinterpret_cast<std::uint64_t>(dst);
                payload.dst_capacity = static_cast<std::uint64_t>(capacity);
                payload.slot = static_cast<std::uint8_t>(slot);
                builder.pushWithReply(
                    opcodes::CommandOp, type_ids::ReadbackTarget,
                    payload, std::move(callback));
            });
    }

    RenderRequest<ReadbackTargetReply>
    RenderControlSession::readbackTargetAsync(
        RenderTargetId target, void* dst, std::size_t capacity,
        std::uint32_t settle_frames, TargetSlot slot)
    {
        return recordReply<ReadbackTargetReply>(
            [&](Builder& builder, auto callback)
            {
                ReadbackTargetAsyncPayload payload{};
                payload.target = target;
                payload.dst_ptr = reinterpret_cast<std::uint64_t>(dst);
                payload.dst_capacity = static_cast<std::uint64_t>(capacity);
                payload.settle_frames = settle_frames;
                payload.slot = static_cast<std::uint8_t>(slot);
                builder.pushWithReply(
                    opcodes::CommandOp, type_ids::ReadbackTargetAsync,
                    payload, std::move(callback));
            });
    }

    RenderRequest<RenderGraphDumpReply> RenderControlSession::dumpRenderGraph(
        RenderSceneId scene, void* dst, std::size_t capacity)
    {
        return recordReply<RenderGraphDumpReply>(
            [&](Builder& builder, auto callback)
            {
                DumpRenderGraphPayload payload{};
                payload.scene_id = scene;
                payload.dst_ptr = reinterpret_cast<std::uint64_t>(dst);
                payload.dst_capacity = static_cast<std::uint64_t>(capacity);
                builder.pushWithReply(
                    opcodes::CommandOp, type_ids::DumpRenderGraph,
                    payload, std::move(callback));
            });
    }

    RenderRequest<GpuTimingReply> RenderControlSession::queryGpuTiming(
        RenderSceneId scene,
        void* dst,
        std::size_t capacity)
    {
        return recordReply<GpuTimingReply>(
            [&](Builder& builder, auto callback)
            {
                QueryGpuTimingPayload payload{};
                payload.scene_id = scene;
                payload.dst_ptr = reinterpret_cast<std::uint64_t>(dst);
                payload.dst_capacity = static_cast<std::uint64_t>(capacity);
                builder.pushWithReply(
                    opcodes::CommandOp,
                    type_ids::QueryGpuTiming,
                    payload,
                    std::move(callback)
                );
            });
    }

    RenderRequest<QueryFeatureParamsReply>
    RenderControlSession::queryFeatureParams(
        RenderSceneId scene, void* dst, std::size_t capacity)
    {
        return recordReply<QueryFeatureParamsReply>(
            [&](Builder& builder, auto callback)
            {
                QueryFeatureParamsPayload payload{};
                payload.scene_id = scene;
                payload.dst_ptr = reinterpret_cast<std::uint64_t>(dst);
                payload.dst_capacity = static_cast<std::uint64_t>(capacity);
                builder.pushWithReply(
                    opcodes::CommandOp, type_ids::QueryFeatureParams,
                    payload, std::move(callback));
            });
    }

    RenderRequest<DeviceCapsReply> RenderControlSession::queryDeviceCaps(
        DeviceCaps& output)
    {
        return recordReply<DeviceCapsReply>(
            [&](Builder& builder, auto callback)
            {
                QueryDeviceCapsPayload payload{};
                payload.dst_ptr = reinterpret_cast<std::uint64_t>(&output);
                payload.dst_capacity = sizeof(DeviceCaps);
                builder.pushWithReply(
                    opcodes::CommandOp, type_ids::QueryDeviceCaps,
                    payload, std::move(callback));
            });
    }

    RenderRequest<ShaderCompiledReply> RenderControlSession::compileShader(
        std::span<const std::byte> spirv,
        std::span<const std::byte> shader_info)
    {
        return recordReply<ShaderCompiledReply>(
            [&](Builder& builder, auto callback)
            {
                CompileShaderPayload payload{};
                payload.spirv_data = builder.pushOwnedBytesCopy(
                    spirv.data(), static_cast<std::uint32_t>(spirv.size()));
                if (!shader_info.empty())
                    payload.shader_info_data = builder.pushOwnedBytesCopy(
                        shader_info.data(),
                        static_cast<std::uint32_t>(shader_info.size()));
                builder.pushResource(
                    type_ids::CompileShader, payload, std::move(callback));
            });
    }

    RenderRequest<ShaderCompiledReply> RenderControlSession::compileShader(
        std::shared_ptr<const std::vector<std::byte>> spirv,
        std::shared_ptr<const std::vector<std::byte>> shader_info)
    {
        return recordReply<ShaderCompiledReply>(
            [&](Builder& builder, auto callback) mutable
            {
                CompileShaderPayload payload{};
                const auto* spirv_data = spirv ? spirv->data() : nullptr;
                const auto spirv_size = spirv
                    ? static_cast<std::uint32_t>(spirv->size()) : 0u;
                payload.spirv_data = builder.pushSharedBytes(
                    std::static_pointer_cast<const void>(spirv),
                    spirv_data, spirv_size);
                if (shader_info && !shader_info->empty())
                    payload.shader_info_data = builder.pushSharedBytes(
                        std::static_pointer_cast<const void>(shader_info),
                        shader_info->data(),
                        static_cast<std::uint32_t>(shader_info->size()));
                builder.pushResource(
                    type_ids::CompileShader, payload, std::move(callback));
            });
    }

    void RenderControlSession::destroyShader(ShaderHandle shader)
    {
        (void)record([&](Builder& builder)
        {
            builder.push(opcodes::ResourceOp, type_ids::DestroyShader,
                         DestroyShaderPayload{shader});
        });
    }

    RenderRequest<FeatureTypeRegisteredReply>
    RenderControlSession::registerFeatureType(
        const FeatureFactory& factory,
        std::shared_ptr<const void> module_lease)
    {
        return recordReply<FeatureTypeRegisteredReply>(
            [&](Builder& builder, auto callback)
            {
                RegisterFeatureTypePayload payload{};
                payload.factory = factory;
                if (module_lease)
                {
                    payload.module_lease_attachment =
                        builder.template emplaceAttachment<
                            std::shared_ptr<const void>>(
                                attachment_types::LifetimeLease,
                                std::move(module_lease));
                }
                builder.pushWithReply(
                    opcodes::CommandOp, type_ids::RegisterFeatureType,
                    payload, std::move(callback));
            });
    }

    RenderRequest<GenericOkReply>
    RenderControlSession::unregisterFeatureType(std::uint32_t feature_type_id)
    {
        return recordReply<GenericOkReply>(
            [&](Builder& builder, auto callback)
            {
                builder.pushWithReply(
                    opcodes::CommandOp,
                    type_ids::UnregisterFeatureType,
                    UnregisterFeatureTypePayload{feature_type_id},
                    std::move(callback));
            });
    }

    RenderRequest<QueryTypeIdReply> RenderControlSession::queryTypeId(
        const char* name)
    {
        return recordReply<QueryTypeIdReply>(
            [&](Builder& builder, auto callback)
            {
                QueryTypeIdPayload payload{};
                if (name)
                    std::strncpy(payload.name, name, sizeof(payload.name) - 1);
                builder.pushWithReply(
                    opcodes::CommandOp, type_ids::QueryTypeId,
                    payload, std::move(callback));
            });
    }

    RenderRequest<FeatureAddedReply> RenderControlSession::addFeatureRaw(
        RenderSceneId scene, std::uint32_t feature_type_id,
        std::span<const std::byte> config)
    {
        return addFeatureRaw(
            scene,
            feature_type_id,
            lux::cxx::SharedBytes<>::copyOf(config));
    }

    RenderRequest<FeatureAddedReply> RenderControlSession::addFeatureRaw(
        RenderSceneId scene, std::uint32_t feature_type_id,
        lux::cxx::SharedBytes<> config)
    {
        return recordReply<FeatureAddedReply>(
            [&](Builder& builder, auto callback)
            {
                const auto attachment =
                    builder.pushSharedBytes(config).attachment_index;
                AddFeaturePayload payload{};
                payload.scene_id = scene;
                payload.feature_type_id = feature_type_id;
                payload.attachment_index = attachment;
                builder.pushWithReply(
                    opcodes::CommandOp, type_ids::AddFeature,
                    payload, std::move(callback));
            });
    }

    RenderRequest<GenericOkReply> RenderControlSession::removeFeature(
        RenderSceneId scene, FeatureHandle feature)
    {
        return recordReply<GenericOkReply>(
            [&](Builder& builder, auto callback)
            {
                RemoveFeaturePayload payload{};
                payload.scene_id = scene;
                payload.feature = feature;
                builder.pushWithReply(
                    opcodes::CommandOp, type_ids::RemoveFeature,
                    payload, std::move(callback));
            });
    }

    RenderRequest<GenericOkReply> RenderControlSession::setFeatureEnabled(
        RenderSceneId scene, FeatureHandle feature, bool enabled)
    {
        return recordReply<GenericOkReply>(
            [&](Builder& builder, auto callback)
            {
                SetFeatureEnabledPayload payload{};
                payload.scene_id = scene;
                payload.feature = feature;
                payload.enabled = enabled;
                builder.pushWithReply(
                    opcodes::CommandOp, type_ids::SetFeatureEnabled,
                    payload, std::move(callback));
            });
    }

    void RenderControlSession::destroyTexture(RTextureHandle handle)
    {
        send(opcodes::ResourceOp, type_ids::DestroyTexture,
             DestroyTexturePayload{handle});
    }

    void RenderControlSession::destroyCubeTexture(RTextureHandle handle)
    {
        send(opcodes::ResourceOp, type_ids::DestroyCubeTexture,
             DestroyCubeTexturePayload{handle});
    }

    void RenderControlSession::deferDestroyScene(RenderSceneId scene) noexcept
    {
        if (scene.isValid()) deferred_scenes_.push_back(scene);
    }

    void RenderControlSession::deferRemoveView(
        RenderSceneId scene, ViewHandle view,
        RenderViewReleaseObserver observer) noexcept
    {
        if (scene.isValid() && view.isValid())
            deferred_views_.push_back(
                {scene, view, std::move(observer)});
    }

    void RenderControlSession::deferDestroyTarget(
        RenderTargetId target, RenderTargetReleaseObserver observer) noexcept
    {
        if (target.isValid())
            deferred_targets_.push_back({target, std::move(observer)});
    }

    bool RenderControlSession::flushDeferredReleases() noexcept
    {
        if (deferred_views_.empty() && deferred_scenes_.empty() &&
            deferred_targets_.empty())
            return true;

        // Publish in child-before-parent order. A rejected packet leaves that
        // item and every successor owned by this session for a later retry;
        // never clear the obligation before admission succeeds.
        std::size_t published = 0;
        while (published < deferred_views_.size())
        {
            auto& item = deferred_views_[published];
            auto request = removeView(item.scene, item.view);
            if (request.isReady() && request.failed())
                break;
            if (item.observer) request.then(std::move(item.observer));
            ++published;
        }
        deferred_views_.erase(
            deferred_views_.begin(),
            deferred_views_.begin() +
                static_cast<std::ptrdiff_t>(published)
        );
        if (!deferred_views_.empty())
            return false;

        published = 0;
        while (published < deferred_scenes_.size() &&
               destroyScene(deferred_scenes_[published]))
            ++published;
        deferred_scenes_.erase(
            deferred_scenes_.begin(),
            deferred_scenes_.begin() +
                static_cast<std::ptrdiff_t>(published)
        );
        if (!deferred_scenes_.empty())
            return false;

        published = 0;
        while (published < deferred_targets_.size())
        {
            auto& item = deferred_targets_[published];
            auto request = destroyRenderTarget(item.target);
            if (request.isReady() && request.failed())
                break;
            if (item.observer) request.then(std::move(item.observer));
            ++published;
        }
        deferred_targets_.erase(
            deferred_targets_.begin(),
            deferred_targets_.begin() +
                static_cast<std::ptrdiff_t>(published)
        );
        return deferred_targets_.empty();
    }

    std::size_t RenderControlSession::pendingSceneReleases() const noexcept
    {
        return deferred_scenes_.size();
    }
    std::size_t RenderControlSession::pendingViewReleases() const noexcept
    {
        return deferred_views_.size();
    }
    std::size_t RenderControlSession::pendingTargetReleases() const noexcept
    {
        return deferred_targets_.size();
    }

    void RenderControlSession::requestStop() noexcept
    {
        sync_->requestStop();
    }
} // namespace lux::render
