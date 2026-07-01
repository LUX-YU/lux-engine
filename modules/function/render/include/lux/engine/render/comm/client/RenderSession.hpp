#pragma once
// ============================================================================
//  RenderSession.hpp — High-level semantic wrapper around GeneralRenderClient
//
//  Provides a clean API for scene, view, feature, resource, and instance
//  management without exposing FrameProgramBuilder or raw payload construction.
// ============================================================================

#include <lux/engine/render/comm/client/RenderClient.hpp>
#include <lux/engine/render/comm/client/RenderRequest.hpp>
#include <lux/engine/render/comm/RenderProtocol.hpp>
#include <lux/engine/render/renderer/RenderTargetLayout.hpp>   // TargetSlot (readback semantic)
// Mesh data ops moved to renderer/features/meshstack/MeshStackOperation.hpp (feature domain).
#include <lux/engine/render/resources/ops/TextureResourceOperation.hpp>
// Material ops moved to renderer/features/material/MaterialOperation.hpp (feature domain).
// (Light ops moved to renderer/features/light/LightOperation.hpp — feature-scoped.)
#include <lux/engine/render/resources/ops/ShaderResourceOperation.hpp>

#include <lux/engine/function/visibility.h>

#include <Eigen/Core>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// NB: rdesc::Mesh is a struct (description/Mesh.hpp). MSVC mangles the
// struct/class key into method signatures, so the forward-decl keyword MUST
// match the definition or callers that include the real header first get
// LNK2019 (class-vs-struct, V vs U mangling).
namespace lux::rdesc
{
    struct Mesh;
    enum class ETexturePixelFormat : uint32_t;
}

namespace lux::render
{
    class LUX_FUNCTION_PUBLIC RenderSession
    {
    public:
        using Client  = GeneralRenderClient;
        using Builder = Client::Builder;

        explicit RenderSession(
            std::shared_ptr<RenderProgramChannel<>> channel,
            std::shared_ptr<RenderChannelSync> sync
        );

        // =================================================================
        //  Frame lifecycle
        // =================================================================
        /// Drain all pending reply callbacks.
        void pumpReplies();

        /// Block until at least one reply arrives, then drain.
        bool waitAndPumpReplies();

        /// Begin recording a new frame.
        bool beginFrame(const FrameMemoryHints& hints = {});

        /// Finalize and submit the current frame.
        bool submitFrame(bool blocking = false);

        /// Synchronous request-reply helper for initialization.
        /// Submits the current frame, blocks until the request is ready,
        /// then begins a new frame. Returns the reply by value.
        ///
        /// Stop-aware: if the channel is stopping (requestStop / render thread
        /// shutdown), submitFrame(true) and waitAndPumpReplies() both return false
        /// immediately and the reply can never arrive. Without the guards below the
        /// while-loop degenerated into a 100% CPU spin that hung the caller
        /// un-joinably. On an aborted call we return a value-initialized T{} — init
        /// paths should treat a zero/empty reply as "startup aborted". (medium)
        template <typename T>
        T syncCall(RenderRequest<T> req)
        {
            if (!submitFrame(/*blocking=*/true))
                return T{};
            while (!req.isReady())
            {
                if (!waitAndPumpReplies())
                    break;   // channel stopping — the reply will never arrive
            }
            if (!req.isReady())
                return T{};
            T result = req.result();
            beginFrame({});
            return result;
        }

        /// Convenience: pumpReplies → retryPending → begin → fill → submit.
        template <typename Fn>
        bool tick(Fn&& fn, const FrameMemoryHints& hints = {})
        {
            pumpReplies();
            if (!client_.retryPendingSubmit(false)) return false;
            if (!beginFrame(hints)) return false;
            fn(*this);
            return submitFrame(false);
        }

        // =================================================================
        //  Scene management
        // =================================================================

        /// Full scene creation config (client-side, non-trivially-copyable).
        struct CreateSceneConfig
        {
            const char*      name{""};
            std::uint32_t    flags{0};
            lux::common::ETextureFormat lit_color_format{lux::common::ETextureFormat::RGBA16_SFLOAT};

            struct ViewInit
            {
                common::Size2D    extent{};
                const char* name{"View"};
            };
            const ViewInit*  views{nullptr};
            std::uint32_t    view_count{0};
        };

        RenderRequest<SceneCreatedReply> createScene(const CreateSceneConfig& config);

        /// Convenience overload — creates a scene with default HDR pipeline, no initial views.
        RenderRequest<SceneCreatedReply> createScene(const char* name, std::uint32_t flags = 0);

        void destroyScene(RenderSceneId scene_id);

        RenderRequest<GenericOkReply> setActiveScene(RenderSceneId scene_id, bool enabled = true);

        // =================================================================
        //  View management
        // =================================================================
        /// Create an offscreen view. Domain-neutral (View 去 3D 化): no camera —
        /// send a StandardViewCamera op (ViewCameraProxy::update) for the returned
        /// view to set its per-frame matrices.
        RenderRequest<ViewCreatedReply> addView(
            RenderSceneId scene_id,
            common::Size2D extent,
            const char* name
        );

        void removeView(RenderSceneId scene_id, ViewHandle view);

        /// Bind a view to the swapchain for direct on-screen rendering.
        /// Uses the default swapchain layout from the server.
        void bindSwapchain(RenderSceneId scene_id, ViewHandle view);

        /// Request the server to create a view, bind swapchain, and return the result.
        /// On UIRenderServer this also sets up the ImGui overlay graph.
        RenderRequest<SwapchainBoundReply> requestSwapchainScene(RenderSceneId scene_id);

        // (addUIView / removeUIView moved to ImGuiProxy — see ImGuiCommConfig.hpp)

        void resizeView(RenderSceneId scene_id, ViewHandle view, common::Size2D new_extent);

        /// GPU->CPU readback of @p view's rendered color image into @p dst — a
        /// CALLER-OWNED buffer that must stay valid until the returned request
        /// resolves (use a blocking syncCall, since the server writes into it
        /// directly). Pixels are copied in the view's native color format; the
        /// reply reports width / height / bytes_per_pixel / bytes_written /
        /// format. Intended for offscreen thumbnail capture. @p slot selects WHICH
        /// output semantic to read (default SceneColor; e.g. InstanceId for picking,
        /// LinearDepth for sensors) — must be a slot the view's layout exposes.
        RenderRequest<ReadbackViewReply> readbackView(
            RenderSceneId scene_id, ViewHandle view, void* dst, std::size_t dst_capacity,
            TargetSlot slot = TargetSlot::SceneColor);

        /// Async (non-blocking) readback: returns immediately; the request
        /// resolves via a DEFERRED reply once the server has settled
        /// @p settle_frames render ticks and the GPU copy completes (like
        /// uploadMesh). @p dst is CALLER-OWNED and must stay valid until the
        /// request resolves — the server writes the pixels there directly. Poll
        /// the request (isReady) from a frame loop; do NOT block the UI thread.
        RenderRequest<ReadbackViewReply> readbackViewAsync(
            RenderSceneId scene_id, ViewHandle view, void* dst, std::size_t dst_capacity,
            std::uint32_t settle_frames = 3, TargetSlot slot = TargetSlot::SceneColor);

        /// Debug: dump @p scene_id's CURRENT compiled render graph (human-readable
        /// text) into @p dst — a CALLER-OWNED buffer that must stay valid until the
        /// returned request resolves (in-memory, like readbackView; no file I/O).
        /// Poll the request (isReady) from the UI loop; do NOT syncCall inside a
        /// frame. The reply's `needed` reports the full size, so resize @p dst to
        /// `needed` and re-issue if it exceeded @p dst_capacity.
        RenderRequest<RenderGraphDumpReply> dumpRenderGraph(
            RenderSceneId scene_id, void* dst, std::size_t dst_capacity);

        /// Enumerate the scene's render features + their reflectable params into
        /// @p dst (caller-owned, same dst_ptr idiom + poll/resize contract as
        /// dumpRenderGraph). The reply's `count` records are packed per
        /// QueryFeatureParamsPayload; `needed` reports the full size.
        RenderRequest<QueryFeatureParamsReply> queryFeatureParams(
            RenderSceneId scene_id, void* dst, std::size_t dst_capacity);

        // =================================================================
        //  Shader compilation
        // =================================================================

        RenderRequest<ShaderCompiledReply> compileShader(
            std::span<const std::byte> spirv,
            std::span<const std::byte> shader_info = {});

        // =================================================================
        //  Feature management
        // =================================================================

        RenderRequest<FeatureTypeRegisteredReply> registerFeatureType(const FeatureFactory& factory);

        /// Query the server-side TypeId for a named CommandOp handler.
        RenderRequest<QueryTypeIdReply> queryTypeId(const char* name);

        /// Add a feature with a trivially-copyable config struct.
        template <typename Config>
        RenderRequest<FeatureAddedReply> addFeature(RenderSceneId scene_id, std::uint32_t feature_type_id, const Config& config)
        {
            static_assert(std::is_trivially_copyable_v<Config>,
                "Feature config must be trivially copyable for attachment transport.");

            auto [req, cb] = RenderRequestFactory<FeatureAddedReply>::make();

            auto& b = builder();
            std::uint32_t att_idx = b.template emplaceAttachment<Config>(
                attachment_types::BorrowedBytes, config);

            AddFeaturePayload afp{};
            afp.scene_id         = scene_id;
            afp.feature_type_id  = feature_type_id;
            afp.attachment_index = att_idx;
            b.pushWithReply(opcodes::CommandOp, type_ids::AddFeature, afp, std::move(cb));

            return req;
        }

        void removeFeature(RenderSceneId scene_id, FeatureHandle feature);

        void setFeatureEnabled(RenderSceneId scene_id, FeatureHandle feature, bool enabled);

        /// Convenience: register a feature type then add it to a scene in two syncCall rounds.
        /// Only for sequential initialization — not for use inside manual frame loops.
        struct FeatureRegistrationResult
        {
            FeatureTypeRegisteredReply type_reply;
            FeatureAddedReply          add_reply;
        };

        template <typename Config>
        FeatureRegistrationResult registerAndAddFeature(
            RenderSceneId scene_id,
            const FeatureFactory& factory,
            const Config& config)
        {
            auto type_reply = syncCall(registerFeatureType(factory));
            auto add_reply  = syncCall(addFeature(scene_id, type_reply.feature_type_id, config));
            return {type_reply, add_reply};
        }

        // =================================================================
        //  Resource upload
        //
        //  LIFETIME:
        //  - the texture create/update methods COPY the source data into
        //    owned/shared storage that the async upload worker pins, so caller
        //    memory may be freed or reused as soon as the call returns.
        //  - shader compile data is still borrowed and must stay valid until
        //    submitFrame() (it is consumed synchronously by the server tick).
        //
        //  THREAD SAFETY: for the still-borrowed paths, the caller must ensure no
        //  other thread mutates or frees the data while the frame is in flight.
        // =================================================================

        // (uploadMesh moved to a feature-scoped MeshStackProxy — mesh data upload is a
        //  StandardMeshStack feature op now (renderer/features/meshstack/MeshStackOperation.hpp).
        //  The deep-copy/worker-pin send lives in the proxy; the reply MeshUploadedReply
        //  stays a core protocol reply (emitted by the shared async-upload worker).)

        // Material upload / modify (uploadGraphMaterial / modifyGraphMaterial) moved
        // to a feature-scoped MaterialProxy — materials are a feature domain now
        // (renderer/features/material/MaterialOperation.hpp). The core session no
        // longer names material upload. (uploadMaterial(rdesc::Material) was retired
        // earlier in W5a.)

        /// Create a 2D texture from raw pixel data.
        RenderRequest<Texture2DCreatedReply> createTexture2D(
            const std::byte* pixels, std::uint32_t byte_count,
            std::int32_t width, std::int32_t height,
            std::int32_t channels = 4,
            EPixelFormat format = EPixelFormat::RGBA8_SRGB,
            bool generate_mips = true
        );

        /// Create a 2D texture from a byte span (convenience overload).
        RenderRequest<Texture2DCreatedReply> createTexture2D(
            std::span<const std::byte> pixels,
            std::int32_t width, std::int32_t height,
            std::int32_t channels = 4,
            EPixelFormat format = EPixelFormat::RGBA8_SRGB,
            bool generate_mips = true)
        {
            return createTexture2D(
                pixels.data(), static_cast<std::uint32_t>(pixels.size()),
                width, height, channels, format, generate_mips);
        }

        /// rdesc-typed overload — accepts the asset-side pixel format so the
        /// caller does not need to import render::EPixelFormat. Internally
        /// translates via render::toPixelFormat; if the format is unsupported
        /// (ETC2 / ASTC) the returned request resolves synchronously with
        /// status != 0 (the caller's .then sees the known-bad reply).
        RenderRequest<Texture2DCreatedReply> createTexture2D(
            const std::byte* pixels, std::uint32_t byte_count,
            std::int32_t width, std::int32_t height,
            std::int32_t channels,
            lux::rdesc::ETexturePixelFormat format,
            bool generate_mips = true);

        /// Span convenience overload of the rdesc-typed createTexture2D.
        RenderRequest<Texture2DCreatedReply> createTexture2D(
            std::span<const std::byte> pixels,
            std::int32_t width, std::int32_t height,
            std::int32_t channels,
            lux::rdesc::ETexturePixelFormat format,
            bool generate_mips = true)
        {
            return createTexture2D(
                pixels.data(), static_cast<std::uint32_t>(pixels.size()),
                width, height, channels, format, generate_mips);
        }

        struct Texture2DMipLevel
        {
            const std::byte* pixels{nullptr};
            std::uint32_t byte_count{0};
            std::uint32_t width{0};
            std::uint32_t height{0};
        };

        /// Create a 2D texture with an explicit mip chain.
        /// When @p mip_count > 1, server-side runtime mip generation is ignored.
        RenderRequest<Texture2DCreatedReply> createTexture2DMips(
            const Texture2DMipLevel* mip_levels,
            std::uint32_t mip_count,
            std::int32_t channels = 4,
            EPixelFormat format = EPixelFormat::RGBA8_SRGB,
            bool generate_mips = false
        );

        /// rdesc-typed overload of createTexture2DMips — see createTexture2D
        /// (rdesc) for the unsupported-format fallback behaviour.
        RenderRequest<Texture2DCreatedReply> createTexture2DMips(
            const Texture2DMipLevel* mip_levels,
            std::uint32_t mip_count,
            std::int32_t channels,
            lux::rdesc::ETexturePixelFormat format,
            bool generate_mips = false);

        /// Update an existing 2D texture from one base level.
        RenderRequest<GenericOkReply> updateTexture2D(
            RTextureHandle handle,
            const std::byte* pixels, std::uint32_t byte_count,
            std::int32_t width, std::int32_t height,
            bool generate_mips = true
        );

        /// Update an existing 2D texture from an explicit mip chain.
        RenderRequest<GenericOkReply> updateTexture2DMips(
            RTextureHandle handle,
            const Texture2DMipLevel* mip_levels,
            std::uint32_t mip_count,
            bool generate_mips = false
        );

        /// Create a cube texture from 6 faces of raw pixel data.
        RenderRequest<CubeTextureCreatedReply> createCubeTexture(
            const std::byte* face_data[6], std::uint32_t face_bytes,
            std::int32_t face_size,
            std::int32_t channels = 4,
            EPixelFormat format = EPixelFormat::RGBA8_SRGB
        );

        /// Update an existing cube texture from 6 faces of raw pixel data.
        RenderRequest<GenericOkReply> updateCubeTexture(
            RTextureHandle handle,
            const std::byte* face_data[6],
            std::uint32_t face_bytes
        );

        // (Light create/update/destroy/batch moved to a feature-scoped LightProxy:
        //  renderer/features/light/LightOperation.hpp. The core session no longer
        //  names light; clients send via LightProxy(session, light_ops).)

        // (destroyMesh moved to MeshStackProxy — mesh data ops are a StandardMeshStack
        //  feature domain now; see the uploadMesh note above.)

        // (destroyMaterial moved to MaterialProxy — material is a feature domain now.)

        /// Destroy a 2D texture resource (created via createTexture2D).
        void destroyTexture(RTextureHandle handle);

        /// Destroy a cube texture resource (created via createCubeTexture).
        /// MUST be used for cube handles — destroyTexture targets only the 2D
        /// set, whose index space is independent of the cube set's.
        void destroyCubeTexture(RTextureHandle handle);

        // (Point cloud methods moved to PointCloudProxy — see PointCloudOperation.hpp)
        // (Trajectory methods moved to TrajectoryProxy — see TrajectoryOperation.hpp)

        // Instance management (addMeshInstance / removeMeshInstance / make|
        // hideInstanceForView) AND per-instance transform / flags / render-state /
        // user-meta updates moved to MeshStackProxy — mesh instances are a feature
        // domain now (renderer/features/meshstack/MeshStackOperation.hpp).
        // Bone-palette uploads moved to SkinningProxy. updateView stays below:
        // views are core, not mesh.

        // =================================================================
        //  Per-frame bulk data
        // =================================================================

        // (updateView removed — per-view camera matrices go through the StandardViewCamera
        //  feature op now: ViewCameraProxy::update, ViewCameraOperation.hpp.)

        // =================================================================
        //  Escape hatch
        // =================================================================

        /// Access the underlying client for advanced usage.
        Client& rawClient() noexcept { return client_; }
        const Client& rawClient() const noexcept { return client_; }

        /// Access the current frame's builder (only valid between begin/submit).
        Builder& builder() noexcept;

        void requestStop();

    private:
        Client client_;
    };

} // namespace lux::render
