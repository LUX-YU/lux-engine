// 驻留 T8:网格域子服务实现。配方注释见头文件。

#include <lux/engine/runtime/render/scene/detail/residency/subservices/MeshSubservice.hpp>
#include <lux/engine/runtime/render/scene/detail/residency/OwnerReplyReaper.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/MeshAsset.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <lux/engine/function/render/client/FeatureCatalog.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/function/render/client/features/meshstack/MeshStackOperation.hpp>   // 便捷面 uploadMesh
#include <lux/engine/platform/FormatCompat.h>   // lux::format

#include <utility>

namespace lux::runtime
{
    namespace
    {
        void deferMeshRelease(
            lux::render::RenderControlSession& control,
            lux::render::MeshStackOperationIds ops,
            lux::render::RMeshHandle          handle) noexcept
        {
            if (handle.isNull())
                return;
            control.send(
                lux::render::opcode_of_v<lux::render::DestroyMeshOp>,
                ops.id<lux::render::DestroyMeshOp>(),
                lux::render::DestroyMeshPayload{.handle = handle});
        }
    }

    MeshSubservice::MeshSubservice(lux::render::RenderControlSession& control,
                                   lux::render::RenderUploadClient upload,
                                   lux::asset::AssetManager&          assets,
                                   const lux::render::FeatureCatalog& catalog,
                                   TryPostToMain                      post_main) noexcept
        : control_(&control)
        , upload_(std::move(upload))
        , assets_(&assets)
        , catalog_(&catalog)
        , post_main_(post_main)
        , replies_(std::make_unique<detail::OwnerReplyReaper<
              lux::render::MeshUploadedReply>>(std::move(post_main)))
    {
    }

    MeshSubservice::~MeshSubservice() = default;

    lux::ecs::EResourceDomain MeshSubservice::domain() const
    {
        return lux::ecs::EResourceDomain::MESH;
    }

    void MeshSubservice::submit(const lux::asset::asset_id_t& id,
                                SubmitDone                    done)
    {
        trySubmit(id, std::move(done));
    }

    void MeshSubservice::trySubmit(
        const lux::asset::asset_id_t& id,
        SubmitDone done)
    {
        const auto* mesh_asset = assets_->fetchAssetAs<lux::asset::MeshAsset>(id);
        if (mesh_asset == nullptr || mesh_asset->data() == nullptr)
        {
            done(0, "mesh data absent at submit (load/submit ordering bug?)");
            return;
        }

        const auto ops = catalog_->ops<lux::render::MeshStackOperationIds>(
            "StandardMeshStack"
        );
        if (!ops.valid())
        {
            // 特性类型没注册进目录 —— 响亮终败(见头文件,不许静默 no-op)。
            done(0, "StandardMeshStack ops unavailable "
                    "(feature type not registered in the catalog)");
            return;
        }

        auto owned = std::make_shared<lux::rdesc::Mesh>(*mesh_asset->data());
        const auto retained_bytes = lux::rdesc::meshRetainedBytes(*owned);
        if (!retained_bytes)
        {
            done(0, "mesh retained-byte accounting overflow");
            return;
        }
        const auto upload_id = ops.id<lux::render::UploadMeshOp>();
        auto submitted = upload_.trySubmit<lux::render::MeshUploadedReply>(
            [owned = std::move(owned), upload_id,
             retained_bytes = *retained_bytes](
                lux::render::RenderUploadClient::Builder& builder) mutable
            {
                lux::render::UploadMeshPayload payload{};
                payload.layout_id = lux::render::kDefaultVertexLayoutId;
                payload.mesh_desc = builder.pushSharedBytes(
                    std::static_pointer_cast<const void>(owned),
                    reinterpret_cast<const std::byte*>(owned.get()),
                    static_cast<std::uint32_t>(sizeof(lux::rdesc::Mesh)),
                    lux::render::attachment_types::OwnedBytes,
                    retained_bytes);
                builder.pushPreparedResource(upload_id, payload);
            },
            lux::render::UploadPayloadAccounting{
                .copied_bytes = *retained_bytes}
        );

        if (!submitted)
        {
            const auto error = submitted.error();
            if (error == lux::render::ERenderUploadSubmitError::QUEUE_FULL ||
                error == lux::render::ERenderUploadSubmitError::BYTE_BUDGET_EXHAUSTED)
            {
                if (post_main_ && post_main_(
                        [this, id, done = std::move(done)]() mutable noexcept
                        { trySubmit(id, std::move(done)); }))
                    return;
            }
            done(0, error == lux::render::ERenderUploadSubmitError::PAYLOAD_INVALID
                        ? "mesh upload payload invalid"
                        : "mesh upload channel stopping");
            return;
        }
        replies_->track(
            std::move(*submitted),
            [control = control_, ops, done = std::move(done)]
            (const lux::render::MeshUploadedReply& r,
             bool compensation_only) mutable noexcept
            {
                if (compensation_only)
                {
                    deferMeshRelease(*control, ops, r.handle);
                    return;
                }
                const bool ok = (r.status == 0 && !r.handle.isNull());
                if (ok)
                {
                    done(lux::ecs::packHandleBits(r.handle), {});
                    return;
                }
                // 先收回畸形失败回执里的非空 owner，再发失败。
                deferMeshRelease(*control, ops, r.handle);
                if (r.capacity_shortfall.present)
                {
                    done(0, lux::format(
                        "mesh upload failed (status={}, capacity_hash={}, "
                        "requested={}, effective={}, bytes={}, "
                        "available_bytes={}, reason={})",
                        r.status,
                        r.capacity_shortfall.domain_hash,
                        r.capacity_shortfall.requested,
                        r.capacity_shortfall.effective,
                        r.capacity_shortfall.bytes,
                        r.capacity_shortfall.available_bytes,
                        static_cast<std::uint32_t>(
                            r.capacity_shortfall.reason)));
                    return;
                }
                done(0, lux::format(
                    "mesh upload failed (status={}, no capacity shortfall)",
                    r.status));
            }
        );
    }

    void MeshSubservice::destroy(std::uint64_t handle_bits) noexcept
    {
        const auto ops = catalog_->ops<lux::render::MeshStackOperationIds>(
            "StandardMeshStack");
        const auto handle =
            lux::ecs::unpackHandleBits<lux::render::RMeshHandle>(handle_bits);
        deferMeshRelease(*control_, ops, handle);
    }

    std::size_t MeshSubservice::pendingReplies() const noexcept
    {
        return replies_->pending();
    }

    bool MeshSubservice::ownerControlsQuiescent() const noexcept
    {
        return replies_->pending() == 0;
    }

    void MeshSubservice::abandonPendingReplies() noexcept
    {
        replies_->abandon();
    }

} // namespace lux::runtime
