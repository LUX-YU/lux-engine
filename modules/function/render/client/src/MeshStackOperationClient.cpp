#include <lux/engine/function/render/client/features/meshstack/MeshStackOperation.hpp>

#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <lux/engine/function/render/client/RenderUploadSession.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/description/Mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

namespace lux::render
{
    RenderRequest<MeshInstanceSlotReply> addMeshInstance(
        MeshStackProxy proxy,
        RenderSceneId scene_id,
        RMeshHandle mesh,
        RMaterialHandle material,
        const RenderSpatialTransform3D& transform,
        std::uint32_t flags,
        EGeometryKind geometry_kind,
        PassMask pass_mask,
        std::uint32_t user_meta_index,
        std::uint32_t transition_milliseconds,
        std::uint32_t transition_seed
    )
    {
        AddMeshInstancePayload payload{};
        payload.scene_id = scene_id;
        payload.mesh = mesh;
        payload.material = material;
        payload.transform = transform;
        payload.flags = flags;
        payload.geometry_kind = geometry_kind;
        payload.pass_mask = pass_mask;
        payload.user_meta_index = user_meta_index;
        payload.transition_milliseconds = transition_milliseconds;
        payload.transition_seed = transition_seed;
        return proxy.addMeshInstance(payload);
    }

    void updateTransform(
        MeshStackProxy proxy,
        RenderSceneId scene_id,
        RenderObjectHandle object,
        const RenderSpatialTransform3D& transform
    )
    {
        TransformWriteEntry entry{};
        entry.scene_id = scene_id;
        entry.object = object;
        entry.transform = transform;
        proxy.updateTransforms(std::span<const TransformWriteEntry>{&entry, 1});
    }

    RenderRequest<MeshInstanceSlotReply> addTransientMeshInstance(
        MeshStackProxy proxy,
        RenderSceneId scene_id,
        RMeshHandle mesh,
        RMaterialHandle material,
        const float transform[16],
        std::uint32_t flags,
        EGeometryKind geometry_kind,
        PassMask pass_mask,
        std::uint32_t user_meta_index
    )
    {
        return addMeshInstance(
            proxy,
            scene_id,
            mesh,
            material,
            makeTransientRenderSpatialTransform3D(transform),
            flags,
            geometry_kind,
            pass_mask,
            user_meta_index,
            0u,
            0u
        );
    }

    void updateTransientMeshTransform(
        MeshStackProxy proxy,
        RenderSceneId scene_id,
        RenderObjectHandle object,
        const float transform[16]
    )
    {
        updateTransform(proxy, scene_id, object, makeTransientRenderSpatialTransform3D(transform));
    }

    void updateTransforms(MeshStackProxy proxy, RenderSceneId scene_id, std::span<TransformWriteEntry> entries)
    {
        for (auto& entry : entries)
            entry.scene_id = scene_id;
        proxy.updateTransforms(entries);
    }

    void updateTransforms(MeshStackProxy proxy, std::span<const TransformWriteEntry> entries)
    {
        proxy.updateTransforms(entries);
    }

    lux::cxx::expected<RenderRequest<MeshUploadedReply>, ERenderUploadSubmitError>
    uploadMesh(
        MeshStackUploadClient client,
        asset::AssetId asset_id,
        const lux::rdesc::Mesh& mesh,
        VertexLayoutId layout_id
    )
    {
        auto owned = std::make_shared<lux::rdesc::Mesh>(mesh);
        const TypeId operation_id = client.ops().id<UploadMeshOp>();
        if (operation_id == kInvalidTypeId)
        {
            return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
        }

        const auto retained_bytes = lux::rdesc::meshRetainedBytes(*owned);
        if (!retained_bytes)
        {
            return lux::cxx::unexpected(ERenderUploadSubmitError::PAYLOAD_INVALID);
        }

        return client.session().trySubmit<MeshUploadedReply>(
            [owned = std::move(owned), asset_id, layout_id, operation_id, retained_bytes = *retained_bytes](
                RenderUploadClient::Builder& builder) {
                UploadMeshPayload payload{};
                payload.asset_id = asset_id;
                payload.layout_id = layout_id;
                payload.mesh_desc = builder.pushSharedBytes(
                    std::static_pointer_cast<const void>(owned),
                    reinterpret_cast<const std::byte*>(owned.get()),
                    static_cast<std::uint32_t>(sizeof(lux::rdesc::Mesh)),
                    attachment_types::OwnedBytes,
                    retained_bytes
                );
                builder.pushPreparedResource(operation_id, payload);
            }
        );
    }
} // namespace lux::render
