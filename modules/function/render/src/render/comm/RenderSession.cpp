// ============================================================================
//  RenderSession.cpp — Implementation of the high-level render session facade
// ============================================================================

#include <lux/engine/render/comm/client/RenderSession.hpp>

#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Texture.hpp>   // ETexturePixelFormat full def for the rdesc-typed createTexture2D overloads

#include <algorithm>
#include <cassert>
#include <cstring>

namespace lux::render
{

    // =========================================================================
    //  Construction
    // =========================================================================

    RenderSession::RenderSession(std::shared_ptr<RenderProgramChannel<>> channel,
                                 std::shared_ptr<RenderChannelSync> sync)
        : client_(std::move(channel), std::move(sync))
    {
    }

    // =========================================================================
    //  Frame lifecycle
    // =========================================================================

    void RenderSession::pumpReplies()
    {
        client_.pumpReplies();
    }

    bool RenderSession::waitAndPumpReplies()
    {
        return client_.waitAndPumpReplies();
    }

    bool RenderSession::beginFrame(const FrameMemoryHints& hints)
    {
        return client_.beginFrame(hints);
    }

    bool RenderSession::submitFrame(bool blocking)
    {
        return client_.submitFrame(blocking);
    }

    RenderSession::Builder& RenderSession::builder() noexcept
    {
        return client_.builder();
    }

    void RenderSession::requestStop()
    {
        client_.requestStop();
    }

    // =========================================================================
    //  Scene management
    // =========================================================================
    RenderRequest<SceneCreatedReply> RenderSession::createScene(const CreateSceneConfig& config)
    {
        auto [req, cb] = RenderRequestFactory<SceneCreatedReply>::make();

        CreateScenePayload csp{};
        if (config.name)
            std::strncpy(csp.name, config.name, sizeof(csp.name) - 1);
        csp.flags            = config.flags;
        csp.lit_color_format = config.lit_color_format;

        const auto n = std::min(config.view_count, std::uint32_t{4});
        csp.view_count = n;
        for (std::uint32_t i = 0; i < n; ++i)
        {
            auto& dst = csp.views[i];
            auto& src = config.views[i];
            dst.extent       = src.extent;
            if (src.name)
                std::strncpy(dst.name, src.name, sizeof(dst.name) - 1);
        }

        builder().pushWithReply(opcodes::CommandOp, type_ids::CreateScene, csp, std::move(cb));
        return req;
    }

    RenderRequest<SceneCreatedReply> RenderSession::createScene(const char* name, std::uint32_t flags)
    {
        CreateSceneConfig cfg{};
        cfg.name  = name;
        cfg.flags = flags;
        return createScene(cfg);
    }

    void RenderSession::destroyScene(RenderSceneId scene_id)
    {
        DestroyScenePayload dsp{};
        dsp.scene_id = scene_id;
        builder().push(opcodes::CommandOp, type_ids::DestroyScene, dsp);
    }

    RenderRequest<GenericOkReply> RenderSession::setActiveScene(RenderSceneId scene_id, bool enabled)
    {
        auto [req, cb] = RenderRequestFactory<GenericOkReply>::make();

        SetActiveScenePayload sas{};
        sas.scene_id = scene_id;
        sas.enabled  = enabled;

        builder().pushWithReply(opcodes::CommandOp, type_ids::SetActiveScene, sas, std::move(cb));
        return req;
    }

    // =========================================================================
    //  View management
    // =========================================================================

    RenderRequest<ViewCreatedReply> RenderSession::addView(
        RenderSceneId scene_id, common::Size2D extent, const char* name)
    {
        auto [req, cb] = RenderRequestFactory<ViewCreatedReply>::make();

        // AddView is domain-neutral now (View 去 3D 化): no initial camera. The client
        // sends a StandardViewCamera op for this view after the reply.
        AddViewPayload avp{};
        avp.scene_id     = scene_id;
        avp.extent       = extent;
        if (name)
            std::strncpy(avp.name, name, sizeof(avp.name) - 1);

        builder().pushWithReply(opcodes::CommandOp, type_ids::AddView, avp, std::move(cb));
        return req;
    }

    RenderRequest<ReadbackViewReply> RenderSession::readbackView(
        RenderSceneId scene_id, ViewHandle view, void* dst, std::size_t dst_capacity,
        TargetSlot slot)
    {
        auto [req, cb] = RenderRequestFactory<ReadbackViewReply>::make();

        ReadbackViewPayload p{};
        p.scene_id     = scene_id;
        p.view         = view;
        // The render thread shares this address space, and the caller blocks on
        // the request (syncCall), so handing it a raw pointer to fill is safe.
        p.dst_ptr      = reinterpret_cast<std::uint64_t>(dst);
        p.dst_capacity = static_cast<std::uint64_t>(dst_capacity);
        p.slot         = static_cast<std::uint8_t>(slot);

        builder().pushWithReply(opcodes::CommandOp, type_ids::ReadbackView, p, std::move(cb));
        return req;
    }

    RenderRequest<ReadbackViewReply> RenderSession::readbackViewAsync(
        RenderSceneId scene_id, ViewHandle view, void* dst, std::size_t dst_capacity,
        std::uint32_t settle_frames, TargetSlot slot)
    {
        auto [req, cb] = RenderRequestFactory<ReadbackViewReply>::make();

        ReadbackViewAsyncPayload p{};
        p.scene_id      = scene_id;
        p.view          = view;
        // The render thread shares this address space; unlike the sync readback
        // the caller does NOT block, so it must keep `dst` alive until the
        // returned request resolves (the server writes pixels there on
        // completion, several ticks later).
        p.dst_ptr       = reinterpret_cast<std::uint64_t>(dst);
        p.dst_capacity  = static_cast<std::uint64_t>(dst_capacity);
        p.settle_frames = settle_frames;
        p.slot          = static_cast<std::uint8_t>(slot);

        builder().pushWithReply(opcodes::CommandOp, type_ids::ReadbackViewAsync, p, std::move(cb));
        return req;
    }

    RenderRequest<RenderGraphDumpReply> RenderSession::dumpRenderGraph(
        RenderSceneId scene_id, void* dst, std::size_t dst_capacity)
    {
        auto [req, cb] = RenderRequestFactory<RenderGraphDumpReply>::make();

        DumpRenderGraphPayload p{};
        p.scene_id     = scene_id;
        // Same address space; the caller keeps `dst` alive until the request
        // resolves (it polls, doesn't block), so a raw pointer to fill is safe.
        p.dst_ptr      = reinterpret_cast<std::uint64_t>(dst);
        p.dst_capacity = static_cast<std::uint64_t>(dst_capacity);

        builder().pushWithReply(opcodes::CommandOp, type_ids::DumpRenderGraph, p, std::move(cb));
        return req;
    }

    RenderRequest<QueryFeatureParamsReply> RenderSession::queryFeatureParams(
        RenderSceneId scene_id, void* dst, std::size_t dst_capacity)
    {
        auto [req, cb] = RenderRequestFactory<QueryFeatureParamsReply>::make();

        QueryFeatureParamsPayload p{};
        p.scene_id     = scene_id;
        // Same address space; caller keeps `dst` alive until the request resolves
        // (it polls, doesn't block).
        p.dst_ptr      = reinterpret_cast<std::uint64_t>(dst);
        p.dst_capacity = static_cast<std::uint64_t>(dst_capacity);

        builder().pushWithReply(opcodes::CommandOp, type_ids::QueryFeatureParams, p, std::move(cb));
        return req;
    }

    void RenderSession::removeView(RenderSceneId scene_id, ViewHandle view)
    {
        RemoveViewPayload rvp{};
        rvp.scene_id = scene_id;
        rvp.view     = view;
        builder().push(opcodes::CommandOp, type_ids::RemoveView, rvp);
    }

    void RenderSession::bindSwapchain(RenderSceneId scene_id, ViewHandle view)
    {
        BindSwapchainPayload bsp{};
        bsp.scene_id = scene_id;
        bsp.view     = view;
        builder().push(opcodes::CommandOp, type_ids::BindSwapchain, bsp);
    }

    // (addUIView / removeUIView moved to ImGuiProxy — see ImGuiCommConfig.hpp)

    void RenderSession::resizeView(RenderSceneId scene_id, ViewHandle view, common::Size2D new_extent)
    {
        ResizeViewPayload rvp{};
        rvp.scene_id   = scene_id;
        rvp.view       = view;
        rvp.new_extent = new_extent;
        builder().push(opcodes::CommandOp, type_ids::ResizeView, rvp);
    }

    // =========================================================================
    //  Shader compilation
    // =========================================================================

    RenderRequest<ShaderCompiledReply> RenderSession::compileShader(
        std::span<const std::byte> spirv,
        std::span<const std::byte> shader_info)
    {
        auto [req, cb] = RenderRequestFactory<ShaderCompiledReply>::make();

        auto& b = builder();

        CompileShaderPayload csp{};
        csp.spirv_data = b.pushBorrowedBytes(
            spirv.data(), static_cast<std::uint32_t>(spirv.size()));

        if (!shader_info.empty())
        {
            csp.shader_info_data = b.pushBorrowedBytes(
                shader_info.data(), static_cast<std::uint32_t>(shader_info.size()));
        }

        b.pushResource(type_ids::CompileShader, csp, std::move(cb));
        return req;
    }

    void RenderSession::destroyShader(ShaderHandle handle)
    {
        DestroyShaderPayload dsp{};
        dsp.handle = handle;
        builder().push(opcodes::ResourceOp, type_ids::DestroyShader, dsp);
    }

    // =========================================================================
    //  Feature management
    // =========================================================================

    RenderRequest<FeatureTypeRegisteredReply> RenderSession::registerFeatureType(const FeatureFactory& factory)
    {
        auto [req, cb] = RenderRequestFactory<FeatureTypeRegisteredReply>::make();

        RegisterFeatureTypePayload rft{};
        rft.factory = factory;

        builder().pushWithReply(opcodes::CommandOp, type_ids::RegisterFeatureType, rft, std::move(cb));
        return req;
    }

    RenderRequest<QueryTypeIdReply> RenderSession::queryTypeId(const char* name)
    {
        auto [req, cb] = RenderRequestFactory<QueryTypeIdReply>::make();

        QueryTypeIdPayload qp{};
        if (name)
            std::strncpy(qp.name, name, sizeof(qp.name) - 1);

        builder().pushWithReply(opcodes::CommandOp, type_ids::QueryTypeId, qp, std::move(cb));
        return req;
    }

    void RenderSession::removeFeature(RenderSceneId scene_id, FeatureHandle feature)
    {
        RemoveFeaturePayload rfp{};
        rfp.scene_id = scene_id;
        rfp.feature  = feature;
        builder().push(opcodes::CommandOp, type_ids::RemoveFeature, rfp);
    }

    void RenderSession::setFeatureEnabled(RenderSceneId scene_id, FeatureHandle feature, bool enabled)
    {
        SetFeatureEnabledPayload sfp{};
        sfp.scene_id = scene_id;
        sfp.feature  = feature;
        sfp.enabled  = enabled;
        builder().push(opcodes::CommandOp, type_ids::SetFeatureEnabled, sfp);
    }

    // =========================================================================
    //  Resource upload
    // =========================================================================

    // (uploadMesh / destroyMesh moved to MeshStackProxy
    //  (renderer/features/meshstack/MeshStackOperation.hpp) — mesh data upload is a
    //  StandardMeshStack feature op now, sent with the feature's dynamic op-ids. The
    //  shared-copy send (pushSharedBytes, for the async worker) lives in the proxy.)

    // (uploadBonePalette / uploadBoneBatch moved to SkinningProxy in
    //  renderer/features/skinning/SkinningOperation.hpp — skinning is feature-scoped,
    //  sent with the feature's dynamic op-ids, not a core RenderSession method.)

    // (RenderSession::uploadMaterial(rdesc::Material) retired in W5a — the builtin
    //  closure material families were removed; uploadGraphMaterial is the sole path.)

    // uploadGraphMaterial / modifyGraphMaterial moved to MaterialProxy
    // (renderer/features/material/MaterialOperation.hpp) — materials are a feature
    // domain now. The borrow (upload) vs blob (modify) routing lives in the proxy.

    RenderRequest<Texture2DCreatedReply> RenderSession::createTexture2D(
        const std::byte* pixels, std::uint32_t byte_count,
        std::int32_t width, std::int32_t height,
        std::int32_t channels,
        EPixelFormat format,
        bool generate_mips)
    {
        auto [req, cb] = RenderRequestFactory<Texture2DCreatedReply>::make();

        auto& b = builder();

        CreateTexture2DPayload utp{};
        utp.width         = width;
        utp.height        = height;
        utp.channels      = channels;
        utp.format        = format;
        utp.generate_mips = generate_mips;
        utp.mip_count     = 1;
        utp.mips[0].pixels = b.pushOwnedBytesCopy(pixels, byte_count);
        utp.mips[0].width  = static_cast<uint32_t>(std::max(0, width));
        utp.mips[0].height = static_cast<uint32_t>(std::max(0, height));

        b.pushResource(type_ids::CreateTexture2D, utp, std::move(cb));
        return req;
    }

    RenderRequest<Texture2DCreatedReply> RenderSession::createTexture2D(
        const std::byte* pixels, std::uint32_t byte_count,
        std::int32_t width, std::int32_t height,
        std::int32_t channels,
        lux::rdesc::ETexturePixelFormat format,
        bool generate_mips)
    {
        EPixelFormat fmt{};
        if (!toPixelFormat(format, fmt))
        {
            // Unsupported (ETC2 / ASTC) — synchronously settle as known-bad
            // so the caller's .then() sees status!=0 with no wire dispatch.
            return RenderRequestFactory<Texture2DCreatedReply>::makeImmediate(
                Texture2DCreatedReply{RTextureHandle{}, 1u});
        }
        return createTexture2D(pixels, byte_count, width, height, channels, fmt, generate_mips);
    }

    RenderRequest<Texture2DCreatedReply> RenderSession::createPersistentTexture2D(
        const PersistentTexture2DDesc& desc)
    {
        // Client pre-flight with the SHARED U2-00 validator: a malformed desc settles
        // synchronously (status = the exact ERegionUploadStatus) with no wire dispatch.
        if (const auto v = validatePersistentTexture2DDesc(desc); !v.ok())
            return RenderRequestFactory<Texture2DCreatedReply>::makeImmediate(
                Texture2DCreatedReply{RTextureHandle{}, static_cast<uint32_t>(v.status)});

        auto [req, cb] = RenderRequestFactory<Texture2DCreatedReply>::make();
        CreatePersistentTexture2DPayload p{};
        p.desc = desc;
        builder().pushResource(type_ids::CreatePersistentTexture2D, p, std::move(cb));
        return req;
    }

    RenderRequest<TextureRegionsAppliedReply> RenderSession::updateTextureRegions(
        const OwnedTextureUploadBatch& batch)
    {
        // Structural refusals settle synchronously (echoing the revision so a
        // producer's ack bookkeeping stays uniform). Bounds are validated by the
        // SERVER against the authoritative create-desc; producers that know their
        // desc can pre-flight via batch.validate().
        if (batch.regions.empty())
            return RenderRequestFactory<TextureRegionsAppliedReply>::makeImmediate(
                TextureRegionsAppliedReply{batch.content_revision,
                    static_cast<uint32_t>(ERegionUploadStatus::NoRegions), 0});
        if (!batch.pixels || batch.pixel_bytes == 0)
            return RenderRequestFactory<TextureRegionsAppliedReply>::makeImmediate(
                TextureRegionsAppliedReply{batch.content_revision,
                    static_cast<uint32_t>(ERegionUploadStatus::DataOutOfRange), 0});

        auto [req, cb] = RenderRequestFactory<TextureRegionsAppliedReply>::make();
        auto& b = builder();

        // U2-00 ownership contract: the region descs are deep-copied into their own
        // owned attachment and the pixel block rides the batch's shared_ptr — the
        // caller's memory is reusable the moment we return; the attachments pin both
        // blocks until the render thread / transfer worker is done.
        auto owned_regions =
            std::make_shared<std::vector<TextureRegionDesc>>(batch.regions);

        UpdateTextureRegionsPayload p{};
        p.handle           = batch.dst;
        p.content_revision = batch.content_revision;
        p.region_count     = static_cast<uint32_t>(owned_regions->size());
        p.regions = b.pushSharedBytes(
            owned_regions,
            reinterpret_cast<const std::byte*>(owned_regions->data()),
            static_cast<uint32_t>(owned_regions->size() * sizeof(TextureRegionDesc)));
        p.pixels = b.pushSharedBytes(
            std::shared_ptr<const void>(batch.pixels, batch.pixels.get()),   // aliasing: array→void
            batch.pixels.get(),
            static_cast<uint32_t>(batch.pixel_bytes));
        b.pushResource(type_ids::UpdateTextureRegions, p, std::move(cb));
        return req;
    }

    RenderRequest<Texture2DCreatedReply> RenderSession::createTexture2DMips(
        const Texture2DMipLevel* mip_levels,
        std::uint32_t mip_count,
        std::int32_t channels,
        EPixelFormat format,
        bool generate_mips)
    {
        // Reject malformed input up front: mip_count==0 (e.g. an empty mip vector
        // from a failed loader) or a null array. The old code clamped mip_count to
        // [1,16] and then unconditionally dereferenced mip_levels[0]. Settle as
        // known-bad (status!=0) without a wire dispatch, mirroring the
        // unsupported-format path. (medium, M8)
        if (mip_count == 0u || mip_levels == nullptr)
        {
            return RenderRequestFactory<Texture2DCreatedReply>::makeImmediate(
                Texture2DCreatedReply{RTextureHandle{}, 1u});
        }

        auto [req, cb] = RenderRequestFactory<Texture2DCreatedReply>::make();

        auto& b = builder();

        CreateTexture2DPayload utp{};
        utp.channels      = channels;
        utp.format        = format;
        utp.generate_mips = generate_mips;

        const uint32_t clamped = std::clamp<uint32_t>(
            mip_count,
            1u,
            kTextureUploadMaxMipCount);
        utp.mip_count = clamped;

        for (uint32_t i = 0; i < clamped; ++i)
        {
            const auto& src = mip_levels[i];
            utp.mips[i].pixels = b.pushOwnedBytesCopy(src.pixels, src.byte_count);
            utp.mips[i].width  = src.width;
            utp.mips[i].height = src.height;
        }

        utp.width = static_cast<int32_t>(utp.mips[0].width);
        utp.height = static_cast<int32_t>(utp.mips[0].height);

        b.pushResource(type_ids::CreateTexture2D, utp, std::move(cb));
        return req;
    }

    RenderRequest<Texture2DCreatedReply> RenderSession::createTexture2DMips(
        const Texture2DMipLevel* mip_levels,
        std::uint32_t mip_count,
        std::int32_t channels,
        lux::rdesc::ETexturePixelFormat format,
        bool generate_mips)
    {
        EPixelFormat fmt{};
        if (!toPixelFormat(format, fmt))
        {
            return RenderRequestFactory<Texture2DCreatedReply>::makeImmediate(
                Texture2DCreatedReply{RTextureHandle{}, 1u});
        }
        return createTexture2DMips(mip_levels, mip_count, channels, fmt, generate_mips);
    }

    RenderRequest<GenericOkReply> RenderSession::updateTexture2D(
        RTextureHandle handle,
        const std::byte* pixels,
        std::uint32_t byte_count,
        std::int32_t width,
        std::int32_t height,
        bool generate_mips)
    {
        auto [req, cb] = RenderRequestFactory<GenericOkReply>::make();

        auto& b = builder();

        UpdateTexture2DPayload utp{};
        utp.handle = handle;
        utp.generate_mips = generate_mips;
        utp.mip_count = 1;
        utp.mips[0].pixels = b.pushOwnedBytesCopy(pixels, byte_count);
        utp.mips[0].width = static_cast<uint32_t>(std::max(0, width));
        utp.mips[0].height = static_cast<uint32_t>(std::max(0, height));

        b.pushResource(type_ids::UpdateTexture2D, utp, std::move(cb));
        return req;
    }

    RenderRequest<GenericOkReply> RenderSession::updateTexture2DMips(
        RTextureHandle handle,
        const Texture2DMipLevel* mip_levels,
        std::uint32_t mip_count,
        bool generate_mips)
    {
        // Reject malformed input up front (see createTexture2DMips). (medium, M8)
        if (mip_count == 0u || mip_levels == nullptr)
        {
            return RenderRequestFactory<GenericOkReply>::makeImmediate(GenericOkReply{1u});
        }

        auto [req, cb] = RenderRequestFactory<GenericOkReply>::make();

        auto& b = builder();

        UpdateTexture2DPayload utp{};
        utp.handle = handle;
        utp.generate_mips = generate_mips;

        const uint32_t clamped = std::clamp<uint32_t>(
            mip_count,
            1u,
            kTextureUploadMaxMipCount);
        utp.mip_count = clamped;

        for (uint32_t i = 0; i < clamped; ++i)
        {
            const auto& src = mip_levels[i];
            utp.mips[i].pixels = b.pushOwnedBytesCopy(src.pixels, src.byte_count);
            utp.mips[i].width = src.width;
            utp.mips[i].height = src.height;
        }

        b.pushResource(type_ids::UpdateTexture2D, utp, std::move(cb));
        return req;
    }

    RenderRequest<CubeTextureCreatedReply> RenderSession::createCubeTexture(
        const std::byte* face_data[6], std::uint32_t face_bytes,
        std::int32_t face_size,
        std::int32_t channels,
        EPixelFormat format)
    {
        auto [req, cb] = RenderRequestFactory<CubeTextureCreatedReply>::make();

        auto& b = builder();

        CreateCubeTexturePayload ucp{};
        for (int i = 0; i < 6; ++i)
            ucp.face_data[i] = b.pushOwnedBytesCopy(face_data[i], face_bytes);
        ucp.face_size = face_size;
        ucp.channels  = channels;
        ucp.format    = format;

        b.pushResource(type_ids::CreateCubeTexture, ucp, std::move(cb));
        return req;
    }

    RenderRequest<GenericOkReply> RenderSession::updateCubeTexture(
        RTextureHandle handle,
        const std::byte* face_data[6],
        std::uint32_t face_bytes)
    {
        auto [req, cb] = RenderRequestFactory<GenericOkReply>::make();

        auto& b = builder();

        UpdateCubeTexturePayload ucp{};
        ucp.handle = handle;
        for (int i = 0; i < 6; ++i)
            ucp.face_data[i] = b.pushOwnedBytesCopy(face_data[i], face_bytes);

        b.pushResource(type_ids::UpdateCubeTexture, ucp, std::move(cb));
        return req;
    }

    // (createLight / updateLight / updateLights / destroyLight moved to a
    //  feature-scoped LightProxy — renderer/features/light/LightOperation.hpp.
    //  The core session no longer names light.)

    // (destroyMesh moved to MeshStackProxy — see the uploadMesh note above; mesh data
    //  ops are a StandardMeshStack feature domain now.)

    // destroyMaterial moved to MaterialProxy (material is a feature domain now).

    void RenderSession::destroyTexture(RTextureHandle handle)
    {
        DestroyTexturePayload dtp{};
        dtp.handle = handle;
        builder().push(opcodes::ResourceOp, type_ids::DestroyTexture, dtp);
    }

    void RenderSession::destroyCubeTexture(RTextureHandle handle)
    {
        DestroyCubeTexturePayload dtp{};
        dtp.handle = handle;
        builder().push(opcodes::ResourceOp, type_ids::DestroyCubeTexture, dtp);
    }

    // (Point cloud + trajectory methods moved to PointCloudProxy / TrajectoryProxy)

    // Instance management (addMeshInstance / removeMeshInstance / make|hide
    // InstanceForView) + per-instance transform / flags / render-state / user-meta
    // updates moved to MeshStackProxy (MeshStackOperation.hpp) — mesh instances
    // are a feature domain now. updateView stays (views are core).

    // =========================================================================
    //  Per-frame bulk data
    // =========================================================================

    // RenderSession::updateView was REMOVED (View 去 3D 化). Per-view camera updates go
    // through the StandardViewCamera feature-scoped op now (ViewCameraProxy::update →
    // ViewCameraOperation.hpp). The core session no longer names camera data.

    // updateTransform / updateInstanceFlags / updateInstanceRenderState /
    // updateInstanceUserMeta moved to MeshStackProxy (MeshStackOperation.hpp).

    RenderRequest<SwapchainBoundReply> RenderSession::requestSwapchainScene(RenderSceneId scene_id)
    {
        auto [req, cb] = RenderRequestFactory<SwapchainBoundReply>::make();

        RequestSwapchainScenePayload rsp{};
        rsp.scene_id = scene_id;

        builder().pushWithReply(opcodes::CommandOp, type_ids::RequestSwapchainScene, rsp, std::move(cb));
        return req;
    }
} // namespace lux::render
