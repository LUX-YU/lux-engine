#pragma once

#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/function/render/client/RenderClient.hpp>
#include <lux/engine/function/render/client/RenderLease.hpp>
#include <lux/engine/function/render/client/RenderRequest.hpp>
#include <lux/engine/function/render/client/RenderTargetLayout.hpp>
#include <lux/engine/function/render/client/resources/ops/ShaderResourceOperation.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace lux::render
{
    /// Main-thread control-plane endpoint. Every operation is published to a
    /// dedicated bounded SPSC channel immediately; none requires an OPEN frame.
    class LUX_FUNCTION_PUBLIC RenderControlSession final
    {
    public:
        using CallbackStore = ResponseCallbackStore<>;
        using Builder       = SingleOperationBuilder<>;

        explicit RenderControlSession(
            std::shared_ptr<RenderControlChannel<>> channel,
            std::shared_ptr<RenderChannelSync> sync
        );

        RenderControlSession(const RenderControlSession&) = delete;
        RenderControlSession& operator=(const RenderControlSession&) = delete;

        std::size_t pumpReplies();
        [[nodiscard]] bool waitAndPumpReplies();

        template <class Reply>
        [[nodiscard]] Expected<Reply> syncCall(RenderRequest<Reply> request)
        {
            if (!request.valid())
                return renderFailure<err::comm::RequestInvalid>();
            while (!request.isReady())
                if (!waitAndPumpReplies())
                    return renderFailure<err::comm::ChannelStopping>();
            auto result = request.tryResult();
            if (!result)
                return lux::cxx::unexpected<RenderError>(result.error());
            return result->get();
        }

        template <class Predicate>
        [[nodiscard]] bool awaitAllReady(Predicate&& all_ready)
        {
            while (!all_ready())
                if (!waitAndPumpReplies())
                    return false;
            return true;
        }

        struct CreateSceneConfig
        {
            const char*   name{""};
            std::uint32_t flags{0};
            lux::common::ETextureFormat lit_color_format{
                lux::common::ETextureFormat::RGBA16_SFLOAT};
            double coordinate_page_size{1024.0};
            std::int64_t scene_origin_page[3]{};
        };

        [[nodiscard]] RenderRequest<SceneCreatedReply> createScene(
            const CreateSceneConfig& config);
        [[nodiscard]] RenderRequest<SceneCreatedReply> createScene(
            const char* name, std::uint32_t flags = 0);
        [[nodiscard]] RenderSceneLease adoptScene(RenderSceneId scene) noexcept;
        [[nodiscard]] bool destroyScene(RenderSceneId scene);
        [[nodiscard]] RenderRequest<GenericOkReply> setActiveScene(
            RenderSceneId scene, bool enabled = true);

        [[nodiscard]] RenderRequest<ViewCreatedReply> addView(
            RenderSceneId scene, common::Size2D extent, const char* name);
        [[nodiscard]] RenderViewLease adoptView(
            RenderSceneId scene, ViewHandle view,
            RenderViewReleaseObserver observer = {}) noexcept;
        [[nodiscard]] RenderRequest<GenericOkReply> removeView(
            RenderSceneId scene, ViewHandle view);

        [[nodiscard]] RenderRequest<TargetReadyReply>
        createOffscreenRenderTarget(common::Size2D extent, std::uint32_t flags = 0);

        [[nodiscard]] RenderRequest<TargetReadyReply>
        createSurfaceRenderTarget(std::uint64_t native_window_handle, common::Size2D extent);

        [[nodiscard]] RenderTargetLease
        adoptTarget(RenderTargetId target, RenderTargetReleaseObserver observer = {}) noexcept;

        [[nodiscard]] RenderRequest<TargetReleasedReply>
        destroyRenderTarget(RenderTargetId target);

        void setLayer(RenderTargetId target, std::uint32_t order, RenderSceneId scene, ViewHandle view);
        void removeLayer(RenderTargetId target, std::uint32_t order);
        void resizeTarget(RenderTargetId target, common::Size2D extent);
        void bindSwapchain(RenderSceneId scene, ViewHandle view);

        [[nodiscard]] RenderRequest<ReadbackTargetReply>
        readbackTarget(RenderTargetId target, void* dst, std::size_t capacity, TargetSlot slot = TargetSlot::SCENE_COLOR);

        [[nodiscard]] RenderRequest<ReadbackTargetReply>
        readbackTargetAsync(RenderTargetId target, void* dst, std::size_t capacity, std::uint32_t settle_frames = 3, TargetSlot slot = TargetSlot::SCENE_COLOR);
        [[nodiscard]] RenderRequest<RenderGraphDumpReply> dumpRenderGraph(RenderSceneId scene, void* dst, std::size_t capacity);
        [[nodiscard]] RenderRequest<GpuTimingReply> queryGpuTiming(RenderSceneId scene, void* dst, std::size_t capacity);
        [[nodiscard]] RenderRequest<QueryFeatureParamsReply> queryFeatureParams(RenderSceneId scene, void* dst, std::size_t capacity);
        [[nodiscard]] RenderRequest<DeviceCapsReply> queryDeviceCaps(DeviceCaps& output);

        [[nodiscard]] RenderRequest<ShaderCompiledReply> compileShader(
            std::span<const std::byte> spirv,
            std::span<const std::byte> shader_info = {}
        );

        [[nodiscard]] RenderRequest<ShaderCompiledReply> compileShader(
            std::shared_ptr<const std::vector<std::byte>> spirv,
            std::shared_ptr<const std::vector<std::byte>> shader_info = {}
        );

        void destroyShader(ShaderHandle shader);

        [[nodiscard]] RenderRequest<FeatureTypeRegisteredReply>
        registerFeatureType(
            const FeatureFactory& factory,
            std::shared_ptr<const void> module_lease = {}
        );

        [[nodiscard]] RenderRequest<GenericOkReply> unregisterFeatureType(std::uint32_t feature_type_id);
        [[nodiscard]] RenderRequest<QueryTypeIdReply> queryTypeId(const char* name);

        template <class Config>
        [[nodiscard]] RenderRequest<FeatureAddedReply> addFeature(
            RenderSceneId scene,
            std::uint32_t feature_type_id,
            const Config& config)
        {
            static_assert(std::is_trivially_copyable_v<Config>);
            return recordReply<FeatureAddedReply>(
                [&](Builder& builder, auto callback)
                {
                    const auto attachment =
                        builder.template emplaceAttachment<Config>(
                            attachment_types::OwnedObject, config);
                    AddFeaturePayload payload{};
                    payload.scene_id = scene;
                    payload.feature_type_id = feature_type_id;
                    payload.attachment_index = attachment;
                    builder.pushWithReply(
                        opcodes::CommandOp, type_ids::AddFeature,
                        payload, std::move(callback));
                });
        }

        [[nodiscard]] RenderRequest<FeatureAddedReply> addFeatureRaw(
            RenderSceneId scene,
            std::uint32_t feature_type_id,
            std::span<const std::byte> config);
        [[nodiscard]] RenderRequest<FeatureAddedReply> addFeatureRaw(
            RenderSceneId scene,
            std::uint32_t feature_type_id,
            lux::cxx::SharedBytes<> config);
        [[nodiscard]] RenderRequest<GenericOkReply> removeFeature(
            RenderSceneId scene, FeatureHandle feature);
        [[nodiscard]] RenderRequest<GenericOkReply> setFeatureEnabled(
            RenderSceneId scene, FeatureHandle feature, bool enabled);

        template <FrameBlobPayload Payload>
        void send(OpCode opcode, TypeId type_id, const Payload& payload)
        {
            (void)record([&](Builder& builder)
            {
                builder.push(opcode, type_id, payload);
            });
        }

        template <class Reply, FrameBlobPayload Payload>
        [[nodiscard]] RenderRequest<Reply> request(
            OpCode opcode,
            TypeId type_id,
            const Payload& payload)
        {
            return recordReply<Reply>(
                [&](Builder& builder, auto callback)
                {
                    if (opcode == opcodes::ResourceOp)
                        builder.pushResource(
                            type_id,
                            payload,
                            std::move(callback)
                        );
                    else
                        builder.pushWithReply(
                            opcode,
                            type_id,
                            payload,
                            std::move(callback)
                        );
                }
            );
        }

        void destroyTexture(RTextureHandle handle);
        void destroyCubeTexture(RTextureHandle handle);

        /// Explicitly publish passive RAII fallbacks in child-before-parent
        /// order. Normal close() calls publish immediately.
        [[nodiscard]] bool flushDeferredReleases() noexcept;
        [[nodiscard]] std::size_t pendingSceneReleases() const noexcept;
        [[nodiscard]] std::size_t pendingViewReleases() const noexcept;
        [[nodiscard]] std::size_t pendingTargetReleases() const noexcept;

        void requestStop() noexcept;

    private:
        friend class RenderSceneLease;
        friend class RenderViewLease;
        friend class RenderTargetLease;

        [[nodiscard]] bool publishPacket(
            OperationPacket<>&& packet,
            bool blocking = true
        );

        template <class Reply, class Record>
        [[nodiscard]] RenderRequest<Reply> recordReply(Record&& record)
        {
            if (sync_->isStopping())
                return RenderRequestFactory<Reply>::makeImmediateFailure({});

            OperationPacket<> packet{};
            Builder builder(packet, callbacks_);
            builder.begin({});
            auto [request, callback] = RenderRequestFactory<Reply>::make();
            record(builder, std::move(callback));
            const auto request_id = packet.requestId();
            RenderRequestFactory<Reply>::bindRequestId(request, request_id);
            if (!builder.valid() || builder.commandCount() != 1u ||
                !publishPacket(std::move(packet)))
            {
                callbacks_.cancel(request_id);
                return RenderRequestFactory<Reply>::makeImmediateFailure({});
            }
            return request;
        }

        template <class Record>
        [[nodiscard]] bool record(Record&& record)
        {
            if (sync_->isStopping())
                return false;
            OperationPacket<> packet{};
            Builder builder(packet, callbacks_);
            builder.begin({});
            record(builder);
            return builder.valid() && builder.commandCount() == 1u &&
                   publishPacket(std::move(packet));
        }

        void deferDestroyScene(RenderSceneId scene) noexcept;
        void deferRemoveView(RenderSceneId scene, ViewHandle view,
                             RenderViewReleaseObserver observer) noexcept;
        void deferDestroyTarget(RenderTargetId target,
                                RenderTargetReleaseObserver observer) noexcept;

        struct DeferredView
        {
            RenderSceneId scene{};
            ViewHandle view{};
            RenderViewReleaseObserver observer{};
        };
        struct DeferredTarget
        {
            RenderTargetId target{};
            RenderTargetReleaseObserver observer{};
        };

        std::shared_ptr<RenderControlChannel<>> channel_;
        std::shared_ptr<RenderChannelSync>      sync_;
        CallbackStore                          callbacks_{ERequestLane::CONTROL};
        std::vector<RenderSceneId>   deferred_scenes_;
        std::vector<DeferredView>    deferred_views_;
        std::vector<DeferredTarget>  deferred_targets_;
    };
} // namespace lux::render
