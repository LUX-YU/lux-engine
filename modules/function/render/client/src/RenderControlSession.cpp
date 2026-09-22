#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/detail/RenderReleaseRecords.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

namespace lux::render
{
RenderControlSession::RenderControlSession(std::shared_ptr<RenderControlChannel<>> channel,
                                           std::shared_ptr<RenderChannelSync> sync)
    : channel_(std::move(channel)), sync_(std::move(sync))
{
}

RenderControlSession::~RenderControlSession() = default;

std::size_t RenderControlSession::pumpReplies(std::size_t budget)
{
    const auto consumed = detail::pumpReplyEnvelopes(*channel_, *sync_, callbacks_, budget);
    static_cast<void>(maintainScenes(0));
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

bool RenderControlSession::publishPacket(OperationPacket<> &&packet, bool blocking)
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

RenderRequest<SceneCreatedReply> RenderControlSession::createScene(const CreateSceneConfig &config)
{
    return recordReply<SceneCreatedReply>([&](Builder &builder, auto callback) {
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

RenderRequest<SceneCreatedReply> RenderControlSession::createScene(const char *name, std::uint32_t flags)
{
    return createScene(CreateSceneConfig{.name = name, .flags = flags});
}

RenderSceneLease RenderControlSession::adoptScene(RenderSceneId scene) noexcept
{
    auto record = std::make_shared<detail::SceneReleaseRecord>();
    record->scene = scene;
    scenes_.push_back(record);
    return RenderSceneLease{*this, std::move(record)};
}

RenderSceneLease RenderControlSession::prepareScene(const CreateSceneConfig &config,
                                                    std::vector<SceneFeatureAttachment> features)
{
    auto record = std::make_shared<detail::SceneReleaseRecord>();
    record->state = ESceneResourceState::QUEUED;
    if (config.name)
    {
        std::strncpy(record->creation.name, config.name, sizeof(record->creation.name) - 1);
    }
    record->creation.flags = config.flags;
    record->creation.lit_color_format = config.lit_color_format;
    record->creation.coordinate_page_size = config.coordinate_page_size;
    std::copy_n(config.scene_origin_page, 3, record->creation.scene_origin_page);
    record->attachments = std::move(features);
    record->features.reserve(record->attachments.size());
    scenes_.push_back(record); // Acceptance precedes every external effect.
    return RenderSceneLease(*this, std::move(record));
}

std::size_t RenderControlSession::maintainScenes(std::size_t budget)
{
    std::size_t submitted{};
    for (auto &record : scenes_)
    {
        auto &item = *record;
        if (item.state == ESceneResourceState::RETIRED)
        {
            continue;
        }
        item.requested |= item.uses.load(std::memory_order_acquire) == 0;
        if (sync_->isStopping())
        {
            item.fail(sync_->terminalError().ok() ? renderError<err::comm::ChannelStopping>() : sync_->terminalError(),
                      0);
            continue; // Stop intent alone proves neither CPU nor GPU retirement.
        }
        if (item.create.valid() && item.create.isReady())
        {
            auto result = item.create.tryResult();
            if (!result || !result->get().error.ok() || !result->get().scene_id.isValid())
            {
                item.fail(result ? result->get().error : result.error(), item.create.requestId());
            }
            else
            {
                item.scene = result->get().scene_id;
                item.state = ESceneResourceState::ATTACHING;
            }
            item.create = {};
        }
        if (item.attach.valid() && item.attach.isReady())
        {
            auto result = item.attach.tryResult();
            const auto type = item.attachments[item.next].type;
            if (!result || !result->get().error.ok() || !result->get().feature.isValid())
            {
                item.fail(result ? result->get().error : result.error(), item.attach.requestId(), type);
            }
            else
            {
                item.features.emplace_back(type, result->get().feature);
                ++item.next;
            }
            item.attach = {};
        }
        if (item.release.valid() && item.release.isReady())
        {
            auto result = item.release.tryResult();
            if (!result || result->get().code != 0 || !result->get().error.ok())
            {
                item.fail(result ? result->get().error : result.error(), item.release.requestId());
            }
            else
            {
                item.state = ESceneResourceState::RETIRED;
            }
            item.release = {};
        }
        if (item.requested || item.state == ESceneResourceState::RETIRED)
        {
            if (!item.submitted && item.uses.load(std::memory_order_acquire) == 0 && submitted < budget)
            {
                const bool needs_command = item.scene.isValid();
                if (closeScene(item) == ERenderLeaseCloseStatus::Released && needs_command)
                {
                    ++submitted;
                }
            }
            continue;
        }
        if (item.state == ESceneResourceState::QUEUED && submitted < budget)
        {
            auto request = recordReply<SceneCreatedReply>(
                [&](Builder &builder, auto callback) {
                    builder.pushWithReply(opcodes::CommandOp, type_ids::CreateScene, item.creation,
                                          std::move(callback));
                },
                false);
            if (!request.failed())
            {
                item.create = std::move(request);
                item.state = ESceneResourceState::CREATING;
                ++submitted;
            }
        }
        if (item.state == ESceneResourceState::ATTACHING && !item.attach.valid())
        {
            if (item.next == item.attachments.size())
            {
                item.state = ESceneResourceState::READY;
            }
            else if (submitted < budget)
            {
                const auto &feature = item.attachments[item.next];
                auto request = recordReply<FeatureAddedReply>(
                    [&](Builder &builder, auto callback) {
                        const auto config =
                            builder.pushSharedBytes(lux::cxx::SharedBytes<>::copyOf(feature.configuration));
                        AddFeaturePayload payload{};
                        payload.scene_id = item.scene;
                        payload.feature_type_id = feature.registered_type;
                        payload.attachment_index = config.attachment_index;
                        builder.pushWithReply(opcodes::CommandOp, type_ids::AddFeature, payload, std::move(callback));
                    },
                    false);
                if (!request.failed())
                {
                    item.attach = std::move(request);
                    ++submitted;
                }
            }
        }
    }
    std::erase_if(scenes_, [](const auto &item) {
        return item->state == ESceneResourceState::RETIRED && item->uses.load(std::memory_order_acquire) == 0;
    });
    return submitted;
}

void RenderControlSession::retireScenesAfterBackendStopped() noexcept
{
    backend_retired_ = true;
    for (const auto &record : scenes_)
    {
        if (record->state != ESceneResourceState::RETIRED)
        {
            record->fail(
                sync_->terminalError().ok() ? renderError<err::comm::ChannelStopping>() : sync_->terminalError(), 0);
            record->create = {};
            record->attach = {};
            record->release = {};
            record->submitted = true;
            record->state = ESceneResourceState::RETIRED;
        }
    }
}

bool RenderControlSession::destroyScene(RenderSceneId scene)
{
    const auto request = recordReply<GenericOkReply>([&](Builder &builder, auto callback) {
        builder.pushWithReply(opcodes::CommandOp, type_ids::DestroyScene, DestroyScenePayload{scene},
                              std::move(callback));
    });
    return !request.failed();
}

RenderRequest<GenericOkReply> RenderControlSession::setActiveScene(RenderSceneId scene, bool enabled)
{
    return recordReply<GenericOkReply>([&](Builder &builder, auto callback) {
        SetActiveScenePayload payload{};
        payload.scene_id = scene;
        payload.enabled = enabled;
        builder.pushWithReply(opcodes::CommandOp, type_ids::SetActiveScene, payload, std::move(callback));
    });
}

RenderRequest<ViewCreatedReply> RenderControlSession::addView(RenderSceneId scene, lux::math::Extent2u extent,
                                                              const char *name)
{
    return recordReply<ViewCreatedReply>([&](Builder &builder, auto callback) {
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

RenderViewLease RenderControlSession::adoptView(RenderSceneId scene, ViewHandle view,
                                                RenderViewReleaseObserver observer) noexcept
{
    auto &record = views_.emplace_back(scene, view, std::move(observer));
    return RenderViewLease{*this, scene, view, record};
}

RenderRequest<GenericOkReply> RenderControlSession::removeView(RenderSceneId scene, ViewHandle view)
{
    return recordReply<GenericOkReply>([&](Builder &builder, auto callback) {
        RemoveViewPayload payload{};
        payload.scene_id = scene;
        payload.view = view;
        builder.pushWithReply(opcodes::CommandOp, type_ids::RemoveView, payload, std::move(callback));
    });
}

RenderRequest<TargetReadyReply> RenderControlSession::createOffscreenRenderTarget(lux::math::Extent2u extent,
                                                                                  std::uint32_t flags)
{
    return recordReply<TargetReadyReply>([&](Builder &builder, auto callback) {
        CreateOffscreenTargetPayload payload{};
        payload.extent = extent;
        payload.flags = flags;
        builder.pushWithReply(opcodes::CommandOp, type_ids::CreateOffscreenTarget, payload, std::move(callback));
    });
}

RenderRequest<TargetReadyReply> RenderControlSession::createSurfaceRenderTarget(std::uint64_t native_window_handle,
                                                                                lux::math::Extent2u extent)
{
    return recordReply<TargetReadyReply>([&](Builder &builder, auto callback) {
        CreateSurfaceTargetPayload payload{};
        payload.native_window_handle = native_window_handle;
        payload.extent = extent;
        builder.pushWithReply(opcodes::CommandOp, type_ids::CreateSurfaceTarget, payload, std::move(callback));
    });
}

RenderTargetLease RenderControlSession::adoptTarget(RenderTargetId target,
                                                    RenderTargetReleaseObserver observer) noexcept
{
    auto &record = targets_.emplace_back(target, std::move(observer));
    return RenderTargetLease{*this, target, record};
}

RenderRequest<TargetReleasedReply> RenderControlSession::destroyRenderTarget(RenderTargetId target)
{
    return recordReply<TargetReleasedReply>([&](Builder &builder, auto callback) {
        builder.pushWithReply(opcodes::CommandOp, type_ids::DestroyTarget, DestroyTargetPayload{target},
                              std::move(callback));
    });
}

void RenderControlSession::setLayer(RenderTargetId target, std::uint32_t order, RenderSceneId scene, ViewHandle view)
{
    (void)record([&](Builder &builder) {
        SetLayerPayload payload{};
        payload.target = target;
        payload.order = order;
        payload.scene_id = scene;
        payload.view = view;
        builder.push(opcodes::CommandOp, type_ids::SetLayer, payload);
    });
}

void RenderControlSession::removeLayer(RenderTargetId target, std::uint32_t order)
{
    (void)record([&](Builder &builder) {
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
    (void)record([&](Builder &builder) {
        BindSwapchainPayload payload{};
        payload.scene_id = scene;
        payload.view = view;
        builder.push(opcodes::CommandOp, type_ids::BindSwapchain, payload);
    });
}

RenderRequest<TargetResizedReply> RenderControlSession::requestResizeTarget(RenderTargetId target,
                                                                            lux::math::Extent2u extent)
{
    return recordReply<TargetResizedReply>([&](Builder &builder, auto callback) {
        ResizeTargetPayload payload{target, extent};
        builder.pushWithReply(opcodes::CommandOp, type_ids::ResizeTarget, payload, std::move(callback));
    });
}

RenderRequest<ReadbackTargetReply> RenderControlSession::readbackTarget(RenderTargetId target, void *dst,
                                                                        std::size_t capacity, TargetSlot slot)
{
    return recordReply<ReadbackTargetReply>([&](Builder &builder, auto callback) {
        ReadbackTargetPayload payload{};
        payload.target = target;
        payload.dst_ptr = reinterpret_cast<std::uint64_t>(dst);
        payload.dst_capacity = static_cast<std::uint64_t>(capacity);
        payload.slot = static_cast<std::uint8_t>(slot);
        builder.pushWithReply(opcodes::CommandOp, type_ids::ReadbackTarget, payload, std::move(callback));
    });
}

RenderRequest<ReadbackTargetReply> RenderControlSession::readbackTargetAsync(RenderTargetId target, void *dst,
                                                                             std::size_t capacity,
                                                                             std::uint32_t settle_frames,
                                                                             TargetSlot slot)
{
    return recordReply<ReadbackTargetReply>([&](Builder &builder, auto callback) {
        ReadbackTargetAsyncPayload payload{};
        payload.target = target;
        payload.dst_ptr = reinterpret_cast<std::uint64_t>(dst);
        payload.dst_capacity = static_cast<std::uint64_t>(capacity);
        payload.settle_frames = settle_frames;
        payload.slot = static_cast<std::uint8_t>(slot);
        builder.pushWithReply(opcodes::CommandOp, type_ids::ReadbackTargetAsync, payload, std::move(callback));
    });
}

RenderRequest<RenderGraphDumpReply> RenderControlSession::dumpRenderGraph(RenderSceneId scene, void *dst,
                                                                          std::size_t capacity)
{
    return recordReply<RenderGraphDumpReply>([&](Builder &builder, auto callback) {
        DumpRenderGraphPayload payload{};
        payload.scene_id = scene;
        payload.dst_ptr = reinterpret_cast<std::uint64_t>(dst);
        payload.dst_capacity = static_cast<std::uint64_t>(capacity);
        builder.pushWithReply(opcodes::CommandOp, type_ids::DumpRenderGraph, payload, std::move(callback));
    });
}

RenderRequest<GpuTimingReply> RenderControlSession::queryGpuTiming(RenderSceneId scene, void *dst, std::size_t capacity)
{
    return recordReply<GpuTimingReply>([&](Builder &builder, auto callback) {
        QueryGpuTimingPayload payload{};
        payload.scene_id = scene;
        payload.dst_ptr = reinterpret_cast<std::uint64_t>(dst);
        payload.dst_capacity = static_cast<std::uint64_t>(capacity);
        builder.pushWithReply(opcodes::CommandOp, type_ids::QueryGpuTiming, payload, std::move(callback));
    });
}

RenderRequest<QueryFeatureParamsReply> RenderControlSession::queryFeatureParams(RenderSceneId scene, void *dst,
                                                                                std::size_t capacity)
{
    return recordReply<QueryFeatureParamsReply>([&](Builder &builder, auto callback) {
        QueryFeatureParamsPayload payload{};
        payload.scene_id = scene;
        payload.dst_ptr = reinterpret_cast<std::uint64_t>(dst);
        payload.dst_capacity = static_cast<std::uint64_t>(capacity);
        builder.pushWithReply(opcodes::CommandOp, type_ids::QueryFeatureParams, payload, std::move(callback));
    });
}

RenderRequest<DeviceCapsReply> RenderControlSession::queryDeviceCaps(DeviceCaps &output)
{
    return recordReply<DeviceCapsReply>([&](Builder &builder, auto callback) {
        QueryDeviceCapsPayload payload{};
        payload.dst_ptr = reinterpret_cast<std::uint64_t>(&output);
        payload.dst_capacity = sizeof(DeviceCaps);
        builder.pushWithReply(opcodes::CommandOp, type_ids::QueryDeviceCaps, payload, std::move(callback));
    });
}

RenderRequest<ShaderCompiledReply> RenderControlSession::compileShader(std::span<const std::byte> spirv,
                                                                       std::span<const std::byte> shader_info)
{
    return recordReply<ShaderCompiledReply>([&](Builder &builder, auto callback) {
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

RenderRequest<ShaderCompiledReply> RenderControlSession::compileShader(
    std::shared_ptr<const std::vector<std::byte>> spirv, std::shared_ptr<const std::vector<std::byte>> shader_info)
{
    return recordReply<ShaderCompiledReply>([&](Builder &builder, auto callback) mutable {
        CompileShaderPayload payload{};
        const auto *spirv_data = spirv ? spirv->data() : nullptr;
        const auto spirv_size = spirv ? static_cast<std::uint32_t>(spirv->size()) : 0u;
        payload.spirv_data =
            builder.pushSharedBytes(std::static_pointer_cast<const void>(spirv), spirv_data, spirv_size);
        if (shader_info && !shader_info->empty())
        {
            payload.shader_info_data =
                builder.pushSharedBytes(std::static_pointer_cast<const void>(shader_info), shader_info->data(),
                                        static_cast<std::uint32_t>(shader_info->size()));
        }
        builder.pushResource(type_ids::CompileShader, payload, std::move(callback));
    });
}

void RenderControlSession::destroyShader(ShaderHandle shader)
{
    (void)record([&](Builder &builder) {
        builder.push(opcodes::ResourceOp, type_ids::DestroyShader, DestroyShaderPayload{shader});
    });
}

RenderRequest<FeatureTypeRegisteredReply> RenderControlSession::registerFeatureType(
    const FeatureFactory &factory, std::shared_ptr<const void> module_lease)
{
    return recordReply<FeatureTypeRegisteredReply>([&](Builder &builder, auto callback) {
        RegisterFeatureTypePayload payload{};
        payload.factory = factory;
        if (module_lease)
        {
            payload.module_lease_attachment = builder.template emplaceAttachment<std::shared_ptr<const void>>(
                attachment_types::LifetimeLease, std::move(module_lease));
        }
        builder.pushWithReply(opcodes::CommandOp, type_ids::RegisterFeatureType, payload, std::move(callback));
    });
}

RenderRequest<GenericOkReply> RenderControlSession::unregisterFeatureType(std::uint32_t feature_type_id)
{
    return recordReply<GenericOkReply>([&](Builder &builder, auto callback) {
        builder.pushWithReply(opcodes::CommandOp, type_ids::UnregisterFeatureType,
                              UnregisterFeatureTypePayload{feature_type_id}, std::move(callback));
    });
}

RenderRequest<QueryTypeIdReply> RenderControlSession::queryTypeId(const char *name)
{
    return recordReply<QueryTypeIdReply>([&](Builder &builder, auto callback) {
        QueryTypeIdPayload payload{};
        if (name)
        {
            std::strncpy(payload.name, name, sizeof(payload.name) - 1);
        }
        builder.pushWithReply(opcodes::CommandOp, type_ids::QueryTypeId, payload, std::move(callback));
    });
}

RenderRequest<FeatureAddedReply> RenderControlSession::addFeatureRaw(RenderSceneId scene, std::uint32_t feature_type_id,
                                                                     std::span<const std::byte> config)
{
    return addFeatureRaw(scene, feature_type_id, lux::cxx::SharedBytes<>::copyOf(config));
}

RenderRequest<FeatureAddedReply> RenderControlSession::addFeatureRaw(RenderSceneId scene, std::uint32_t feature_type_id,
                                                                     lux::cxx::SharedBytes<> config)
{
    return recordReply<FeatureAddedReply>([&](Builder &builder, auto callback) {
        const auto attachment = builder.pushSharedBytes(config).attachment_index;
        AddFeaturePayload payload{};
        payload.scene_id = scene;
        payload.feature_type_id = feature_type_id;
        payload.attachment_index = attachment;
        builder.pushWithReply(opcodes::CommandOp, type_ids::AddFeature, payload, std::move(callback));
    });
}

RenderRequest<GenericOkReply> RenderControlSession::removeFeature(RenderSceneId scene, FeatureHandle feature)
{
    return recordReply<GenericOkReply>([&](Builder &builder, auto callback) {
        RemoveFeaturePayload payload{};
        payload.scene_id = scene;
        payload.feature = feature;
        builder.pushWithReply(opcodes::CommandOp, type_ids::RemoveFeature, payload, std::move(callback));
    });
}

RenderRequest<GenericOkReply> RenderControlSession::setFeatureEnabled(RenderSceneId scene, FeatureHandle feature,
                                                                      bool enabled)
{
    return recordReply<GenericOkReply>([&](Builder &builder, auto callback) {
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

ERenderLeaseCloseStatus RenderControlSession::closeScene(detail::SceneReleaseRecord &item) noexcept
{
    item.requested = true;
    if (item.submitted)
    {
        return ERenderLeaseCloseStatus::Released;
    }
    if (sync_->isStopping())
    {
        return ERenderLeaseCloseStatus::Stopping;
    }
    if (item.uses.load(std::memory_order_acquire) > 1 || item.create.valid() || item.attach.valid())
    {
        return ERenderLeaseCloseStatus::Deferred;
    }
    if (!item.scene.isValid())
    {
        item.submitted = true;
        item.state = ESceneResourceState::RETIRED;
        return ERenderLeaseCloseStatus::Released;
    }
    if (std::ranges::any_of(views_, [&](const auto &child) { return child.scene == item.scene && !child.submitted; }))
    {
        return ERenderLeaseCloseStatus::Deferred;
    }
    auto request = recordReply<GenericOkReply>(
        [&](Builder &builder, auto callback) {
            builder.pushWithReply(opcodes::CommandOp, type_ids::DestroyScene, DestroyScenePayload{item.scene},
                                  std::move(callback));
        },
        false);
    if (request.failed())
    {
        return ERenderLeaseCloseStatus::Deferred;
    }
    item.release = std::move(request);
    item.submitted = true;
    item.state = ESceneResourceState::RELEASING;
    return ERenderLeaseCloseStatus::Released;
}

ERenderLeaseCloseStatus RenderControlSession::closeView(detail::ViewReleaseRecord &item) noexcept
{
    item.requested = true;
    if (item.submitted)
    {
        return ERenderLeaseCloseStatus::Released;
    }
    if (sync_->isStopping())
    {
        return ERenderLeaseCloseStatus::Stopping;
    }
    auto request = recordReply<GenericOkReply>(
        [&](Builder &builder, auto callback) {
            builder.pushWithReply(opcodes::CommandOp, type_ids::RemoveView, RemoveViewPayload{item.scene, item.view},
                                  std::move(callback));
        },
        false);
    if (request.failed())
    {
        return ERenderLeaseCloseStatus::Deferred;
    }
    item.submitted = true;
    if (item.observer)
    {
        request.then(std::move(item.observer));
    }
    return ERenderLeaseCloseStatus::Released;
}

RenderTargetCloseResult RenderControlSession::closeTarget(detail::TargetReleaseRecord &item) noexcept
{
    item.requested = true;
    if (item.submitted)
    {
        return item.request;
    }
    if (sync_->isStopping())
    {
        return lux::cxx::unexpected(ERenderTargetCloseError::Stopping);
    }
    auto request = recordReply<TargetReleasedReply>(
        [&](Builder &builder, auto callback) {
            builder.pushWithReply(opcodes::CommandOp, type_ids::DestroyTarget, DestroyTargetPayload{item.target},
                                  std::move(callback));
        },
        false);
    if (request.failed())
    {
        return lux::cxx::unexpected(ERenderTargetCloseError::Busy);
    }
    item.request = std::move(request);
    item.submitted = true;
    if (item.observer)
    {
        item.request.then(std::move(item.observer));
    }
    return item.request;
}

std::size_t RenderControlSession::maintainResources(std::size_t budget)
{
    const auto initial = budget;
    budget -= maintainScenes(budget);
    // Stable records were admitted with their owner, never in a destructor.
    // Feature cleanup owns late replies independently of any Registry.
    // Zero budget still permits reply adoption and terminal local release.
    for (auto it = resources_.begin(); it != resources_.end();)
    {
        auto &record = **it;
        if (record.uses.load(std::memory_order_acquire) == 0 &&
            record.release(record.payload.get(), *this, budget, backend_retired_))
        {
            it = resources_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    // Maintenance may prepare packets; lease destructors never do. Each
    // accepted packet consumes one unit and is never prepared again.
    for (auto &item : views_)
    {
        if (budget == 0)
        {
            break;
        }
        if (item.requested && !item.submitted)
        {
            if (closeView(item) != ERenderLeaseCloseStatus::Released)
            {
                break;
            }
            --budget;
        }
    }
    for (auto &record : scenes_)
    {
        auto &item = *record;
        item.requested |= item.uses.load(std::memory_order_acquire) == 0;
        if (budget == 0)
        {
            break;
        }
        if (item.requested && !item.submitted && item.uses.load(std::memory_order_acquire) == 0)
        {
            if (closeScene(item) != ERenderLeaseCloseStatus::Released)
            {
                break;
            }
            --budget;
        }
    }
    for (auto &item : targets_)
    {
        if (budget == 0)
        {
            break;
        }
        if (item.requested && !item.submitted)
        {
            if (!closeTarget(item))
            {
                break;
            }
            --budget;
        }
    }
    const auto released = [](const auto &item) { return item.submitted && !item.owned; };
    std::erase_if(views_, released);
    std::erase_if(scenes_, [](const auto &item) {
        return item->state == ESceneResourceState::RETIRED && item->uses.load(std::memory_order_acquire) == 0;
    });
    std::erase_if(targets_, released);
    return initial - budget;
}

Expected<bool> RenderControlSession::flushDeferredReleases(std::size_t budget) noexcept
{
    static_cast<void>(maintainResources(budget));
    return pendingViewReleases() == 0 && pendingSceneReleases() == 0 && pendingTargetReleases() == 0 &&
           pendingResourceReleases() == 0;
}

Expected<RenderResourceUse> RenderControlSession::retainResource(std::shared_ptr<void> payload,
                                                                 RenderResourceReleaseStep release,
                                                                 std::shared_ptr<const void> code)
{
    if (sync_->isStopping())
    {
        return renderFailure<err::comm::ChannelStopping>();
    }
    if (!payload || !release)
    {
        return renderFailure<err::comm::RequestInvalid>();
    }
    auto record = std::make_shared<detail::ResourceReleaseRecord>();
    record->code = std::move(code);
    record->payload = std::move(payload);
    record->release = release;
    resources_.push_back(record);
    return RenderResourceUse(std::move(record));
}

bool RenderControlSession::canSubmit(std::size_t commands) const noexcept
{
    return !sync_->isStopping() && commands <= channel_->requests.capacity() - channel_->requests.size();
}

std::size_t RenderControlSession::pendingResourceReleases() const noexcept
{
    return std::ranges::count_if(resources_,
                                 [](const auto &record) { return record->uses.load(std::memory_order_acquire) == 0; });
}

std::size_t RenderControlSession::activeResourceUses() const noexcept
{
    std::size_t count{};
    for (const auto &item : scenes_)
    {
        count += item->uses.load(std::memory_order_acquire);
    }
    for (const auto &item : views_)
    {
        count += item.owned;
    }
    for (const auto &item : targets_)
    {
        count += item.owned;
    }
    for (const auto &item : resources_)
    {
        count += item->uses.load(std::memory_order_acquire);
    }
    return count;
}

std::size_t RenderControlSession::sceneRecordCount() const noexcept
{
    return scenes_.size();
}

bool RenderControlSession::owns(const RenderSceneLease &lease) const noexcept
{
    return lease.session_ == this && bool(lease.record_);
}

std::size_t RenderControlSession::pendingSceneReleases() const noexcept
{
    return std::ranges::count_if(scenes_, [](const auto &item) {
        return (item->requested || item->uses.load(std::memory_order_acquire) == 0) &&
               item->state != ESceneResourceState::RETIRED;
    });
}

std::size_t RenderControlSession::pendingViewReleases() const noexcept
{
    return std::ranges::count_if(views_, [](const auto &item) { return item.requested && !item.submitted; });
}

std::size_t RenderControlSession::pendingTargetReleases() const noexcept
{
    return std::ranges::count_if(targets_, [](const auto &item) { return item.requested && !item.submitted; });
}

void RenderControlSession::requestStop() noexcept
{
    sync_->requestStop();
}
} // namespace lux::render
