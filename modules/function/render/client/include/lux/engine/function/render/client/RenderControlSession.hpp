#pragma once

#include <lux/cxx/core/move_only_function.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/function/render/client/RenderClient.hpp>
#include <lux/engine/function/render/client/RenderProtocol.hpp>
#include <lux/engine/function/render/client/RenderRequest.hpp>
#include <lux/engine/function/render/client/RenderTargetLayout.hpp>
#include <lux/engine/function/render/client/resources/ops/ShaderResourceOperation.hpp>
#include <lux/engine/function/visibility.h>

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
        using CallbackStore = TResponseCallbackStore<>;
        using Builder = SingleOperationBuilder<>;

        explicit RenderControlSession(
            std::shared_ptr<TRenderControlChannel<>> channel,
            std::shared_ptr<RenderChannelSync> sync
        );

        ~RenderControlSession();

        RenderControlSession(const RenderControlSession&) = delete;
        RenderControlSession& operator=(const RenderControlSession&) = delete;

        std::size_t pumpReplies(std::size_t budget = (std::numeric_limits<std::size_t>::max)());

        [[nodiscard]] bool waitAndPumpReplies();

        template <class Reply> [[nodiscard]] Expected<Reply> syncCall(TRenderRequest<Reply> request)
        {
            if (!request.valid())
            {
                return renderFailure<err::comm::RequestInvalid>();
            }
            while (!request.isReady())
            {
                if (!waitAndPumpReplies())
                {
                    return renderFailure<err::comm::ChannelStopping>();
                }
            }
            auto result = request.tryResult();
            if (!result)
            {
                return lux::cxx::unexpected<RenderError>(result.error());
            }
            return result->get();
        }

        template <class Predicate> [[nodiscard]] bool awaitAllReady(Predicate&& all_ready)
        {
            while (!all_ready())
            {
                if (!waitAndPumpReplies())
                {
                    return false;
                }
            }
            return true;
        }

        struct CreateSceneConfig
        {
            const char* name{""};
            std::uint32_t flags{0};
            lux::rdesc::ETextureFormat lit_color_format{lux::rdesc::ETextureFormat::RGBA16_SFLOAT};
            double coordinate_page_size{1024.0};
            std::int64_t scene_origin_page[3]{};
        };

        [[nodiscard]] TRenderRequest<SceneCreatedReply> createScene(const CreateSceneConfig& config);
        [[nodiscard]] TRenderRequest<SceneCreatedReply> createScene(const char* name, std::uint32_t flags = 0);
        [[nodiscard]] bool canSubmit(std::size_t commands = 1) const noexcept;
        [[nodiscard]] TRenderRequest<GenericOkReply> destroyScene(RenderSceneId scene);
        [[nodiscard]] TRenderRequest<GenericOkReply> setActiveScene(RenderSceneId scene, bool enabled = true);

        [[nodiscard]] TRenderRequest<ViewCreatedReply> addView(
            RenderSceneId scene,
            lux::math::Extent2u extent,
            const char* name
        );
        [[nodiscard]] TRenderRequest<GenericOkReply> removeView(RenderSceneId scene, ViewHandle view);

        [[nodiscard]] TRenderRequest<TargetReadyReply> createOffscreenRenderTarget(
            lux::math::Extent2u extent,
            std::uint32_t flags = 0
        );

        [[nodiscard]] TRenderRequest<TargetReadyReply> createSurfaceRenderTarget(
            std::uint64_t native_window_handle,
            lux::math::Extent2u extent
        );

        [[nodiscard]] TRenderRequest<TargetReleasedReply> destroyRenderTarget(RenderTargetId target);

        [[nodiscard]] TRenderRequest<GenericOkReply> setLayer(
            RenderTargetId target,
            std::uint32_t order,
            RenderSceneId scene,
            ViewHandle view
        );
        void removeLayer(RenderTargetId target, std::uint32_t order);
        [[nodiscard]] TRenderRequest<GenericOkReply> switchViewTarget(
            RenderTargetId previous,
            RenderTargetId next,
            RenderSceneId scene,
            ViewHandle view,
            RenderSubmissionState production = {}
        );
        void resizeTarget(RenderTargetId target, lux::math::Extent2u extent);
        [[nodiscard]] TRenderRequest<TargetResizedReply> requestResizeTarget(
            RenderTargetId target,
            lux::math::Extent2u extent
        );
        void bindSwapchain(RenderSceneId scene, ViewHandle view);

        [[nodiscard]] TRenderRequest<ReadbackTargetReply> readbackTarget(
            RenderTargetId target,
            void* dst,
            std::size_t capacity,
            ETargetSlot slot = ETargetSlot::SCENE_COLOR
        );

        [[nodiscard]] TRenderRequest<ReadbackTargetReply> readbackTargetAsync(
            RenderTargetId target,
            void* dst,
            std::size_t capacity,
            std::uint32_t settle_frames = 3,
            ETargetSlot slot = ETargetSlot::SCENE_COLOR
        );
        [[nodiscard]] TRenderRequest<RenderGraphDumpReply> dumpRenderGraph(
            RenderSceneId scene,
            void* dst,
            std::size_t capacity
        );
        [[nodiscard]] TRenderRequest<GpuTimingReply> queryGpuTiming(
            RenderSceneId scene,
            void* dst,
            std::size_t capacity
        );
        [[nodiscard]] TRenderRequest<QueryFeatureParamsReply> queryFeatureParams(
            RenderSceneId scene,
            void* dst,
            std::size_t capacity
        );
        [[nodiscard]] TRenderRequest<DeviceCapsReply> queryDeviceCaps(DeviceCaps& output);

        [[nodiscard]] TRenderRequest<ShaderCompiledReply> compileShader(
            std::span<const std::byte> spirv,
            std::span<const std::byte> shader_info = {}
        );

        [[nodiscard]] TRenderRequest<ShaderCompiledReply> compileShader(
            std::shared_ptr<const std::vector<std::byte>> spirv,
            std::shared_ptr<const std::vector<std::byte>> shader_info = {}
        );

        void destroyShader(ShaderHandle shader);

        [[nodiscard]] TRenderRequest<FeatureTypeRegisteredReply> registerFeatureType(
            const FeatureFactory& factory,
            std::shared_ptr<const void> module_lease = {}
        );

        [[nodiscard]] TRenderRequest<GenericOkReply> unregisterFeatureType(std::uint32_t feature_type_id);
        [[nodiscard]] TRenderRequest<QueryTypeIdReply> queryTypeId(const char* name);

        template <class Config>
        [[nodiscard]] TRenderRequest<FeatureAddedReply> addFeature(
            RenderSceneId scene,
            std::uint32_t feature_type_id,
            const Config& config
        )
        {
            static_assert(std::is_trivially_copyable_v<Config>);
            return recordReply<FeatureAddedReply>([&](Builder& builder, auto callback) {
                const auto attachment =
                    builder.template emplaceAttachment<Config>(attachment_types::OwnedObject, config);
                AddFeaturePayload payload{};
                payload.scene_id = scene;
                payload.feature_type_id = feature_type_id;
                payload.attachment_index = attachment;
                builder.pushWithReply(opcodes::CommandOp, type_ids::AddFeature, payload, std::move(callback));
            });
        }

        [[nodiscard]] TRenderRequest<FeatureAddedReply> addFeatureRaw(
            RenderSceneId scene,
            std::uint32_t feature_type_id,
            std::span<const std::byte> config
        );
        [[nodiscard]] TRenderRequest<FeatureAddedReply> addFeatureRaw(
            RenderSceneId scene,
            std::uint32_t feature_type_id,
            lux::cxx::SharedBytes<> config
        );
        [[nodiscard]] TRenderRequest<GenericOkReply> removeFeature(RenderSceneId scene, FeatureHandle feature);
        [[nodiscard]] TRenderRequest<GenericOkReply> setFeatureEnabled(
            RenderSceneId scene,
            FeatureHandle feature,
            bool enabled
        );

        template <FrameBlobPayload Payload> void send(OpCode opcode, TypeId type_id, const Payload& payload)
        {
            (void)record([&](Builder& builder) { builder.push(opcode, type_id, payload); });
        }

        template <class Reply, FrameBlobPayload Payload>
        [[nodiscard]] TRenderRequest<Reply> request(OpCode opcode, TypeId type_id, const Payload& payload)
        {
            return recordReply<Reply>([&](Builder& builder, auto callback) {
                if (opcode == opcodes::ResourceOp)
                {
                    builder.pushResource(type_id, payload, std::move(callback));
                }
                else
                {
                    builder.pushWithReply(opcode, type_id, payload, std::move(callback));
                }
            });
        }

        void destroyTexture(RTextureHandle handle);
        void destroyCubeTexture(RTextureHandle handle);

        void requestStop() noexcept;

    private:
        friend class RenderRuntime;

        [[nodiscard]] bool publishPacket(TOperationPacket<>&& packet, bool blocking = true);

        template <class Reply, class Record>
        [[nodiscard]] TRenderRequest<Reply> recordReply(Record&& record, bool blocking = true)
        {
            if (sync_->isStopping())
            {
                return TRenderRequestFactory<Reply>::makeImmediateFailure({});
            }

            TOperationPacket<> packet{};
            Builder builder(packet, callbacks_);
            builder.begin({});
            auto [request, callback] = TRenderRequestFactory<Reply>::make();
            record(builder, std::move(callback));
            const auto request_id = packet.requestId();
            TRenderRequestFactory<Reply>::bindRequestId(request, request_id);
            if (!builder.valid() || builder.commandCount() != 1u || !publishPacket(std::move(packet), blocking))
            {
                callbacks_.cancel(request_id);
                return TRenderRequestFactory<Reply>::makeImmediateFailure({});
            }
            return request;
        }

        template <class Record> [[nodiscard]] bool record(Record&& record, bool blocking = true)
        {
            if (sync_->isStopping())
            {
                return false;
            }
            TOperationPacket<> packet{};
            Builder builder(packet, callbacks_);
            builder.begin({});
            record(builder);
            return builder.valid() && builder.commandCount() == 1u && publishPacket(std::move(packet), blocking);
        }

        std::shared_ptr<TRenderControlChannel<>> channel_;
        std::shared_ptr<RenderChannelSync> sync_;
        CallbackStore callbacks_{ERequestLane::CONTROL};
    };
} // namespace lux::render
