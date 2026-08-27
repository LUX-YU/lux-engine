#include <lux/engine/function/render/client/features/canvas2d/Canvas2DOperation.hpp>

#include <lux/engine/function/render/client/genops/Canvas2DOperation.ops.hpp>

#include <cstring>
#include <span>

namespace lux::render
{
    RenderRequest<Image2DSlotReply> addImage(
        Canvas2DProxy proxy,
        RenderSceneId scene,
        const Image2DInstanceData& data,
        float priority,
        bool visible,
        std::uint32_t group
    )
    {
        AddImage2DPayload payload{};
        payload.scene = scene;
        payload.data = data;
        payload.priority = priority;
        payload.visible = visible ? 1u : 0u;
        payload.group = group;
        return proxy.addImageRaw(payload);
    }

    void removeImage(Canvas2DProxy proxy, RenderSceneId scene, Image2DHandle handle)
    {
        if (!proxy.valid())
            return;
        proxy.removeImageRaw(RemoveImage2DPayload{scene, handle});
    }

    void updateTransforms(Canvas2DProxy proxy, std::span<const Image2DTransformEntry> entries)
    {
        if (!proxy.valid() || entries.empty())
            return;
        proxy.updateTransformsRaw(entries);
    }

    void updateTransforms(Canvas2DProxy proxy, RenderSceneId scene, std::span<Image2DTransformEntry> entries)
    {
        if (!proxy.valid() || entries.empty())
            return;
        for (auto& entry : entries)
            entry.scene = scene;
        proxy.updateTransformsRaw(entries);
    }

    void updateTransform(
        Canvas2DProxy proxy,
        RenderSceneId scene,
        Image2DHandle handle,
        const float matrix[6],
        const std::int32_t page_delta[2]
    )
    {
        if (!proxy.valid())
            return;
        Image2DTransformEntry entry{};
        entry.scene = scene;
        entry.handle = handle;
        std::memcpy(entry.m, matrix, sizeof(entry.m));
        std::memcpy(entry.page_delta, page_delta, sizeof(entry.page_delta));
        proxy.updateTransformsRaw(std::span<const Image2DTransformEntry>{&entry, 1});
    }

    void updateVisual(
        Canvas2DProxy proxy,
        RenderSceneId scene,
        Image2DHandle handle,
        const float uv[4],
        std::uint32_t tint,
        std::uint32_t texture_bindless
    )
    {
        if (!proxy.valid())
            return;
        UpdateImage2DVisualPayload payload{};
        payload.scene = scene;
        payload.handle = handle;
        std::memcpy(payload.uv, uv, sizeof(payload.uv));
        payload.tint = tint;
        payload.texture_bindless = texture_bindless;
        proxy.updateVisualRaw(payload);
    }

    void updateKey(
        Canvas2DProxy proxy,
        RenderSceneId scene,
        Image2DHandle handle,
        float priority,
        bool visible,
        std::uint32_t group
    )
    {
        if (!proxy.valid())
            return;
        UpdateImage2DKeyPayload payload{};
        payload.scene = scene;
        payload.handle = handle;
        payload.priority = priority;
        payload.visible = visible ? 1u : 0u;
        payload.group = group;
        proxy.updateKeyRaw(payload);
    }

    void setEnabled(Canvas2DProxy proxy, RenderSceneId scene, bool enabled)
    {
        if (!proxy.valid())
            return;
        proxy.setEnabledRaw(SetCanvas2DEnabledPayload{scene, enabled ? 1u : 0u});
    }

    RenderRequest<PixelFieldSlotReply> addPixelField(
        Canvas2DProxy proxy,
        RenderSceneId scene,
        const PixelField2DInstanceData& data,
        float priority,
        bool visible
    )
    {
        AddPixelField2DPayload payload{};
        payload.scene = scene;
        payload.data = data;
        payload.priority = priority;
        payload.visible = visible ? 1u : 0u;
        return proxy.addPixelFieldRaw(payload);
    }

    void removePixelField(Canvas2DProxy proxy, RenderSceneId scene, PixelFieldInstanceHandle handle)
    {
        if (!proxy.valid())
            return;
        proxy.removePixelFieldRaw(RemovePixelField2DPayload{scene, handle});
    }

    void updatePixelFieldTransform(
        Canvas2DProxy proxy,
        RenderSceneId scene,
        PixelFieldInstanceHandle handle,
        const float matrix[6],
        const std::int32_t page_delta[2]
    )
    {
        if (!proxy.valid())
            return;
        UpdatePixelField2DTransformPayload payload{};
        payload.scene = scene;
        payload.handle = handle;
        std::memcpy(payload.m, matrix, sizeof(payload.m));
        std::memcpy(payload.page_delta, page_delta, sizeof(payload.page_delta));
        proxy.updatePixelFieldTransformRaw(payload);
    }

    void updatePixelFieldKey(
        Canvas2DProxy proxy,
        RenderSceneId scene,
        PixelFieldInstanceHandle handle,
        float priority,
        bool visible
    )
    {
        if (!proxy.valid())
            return;
        UpdatePixelField2DKeyPayload payload{};
        payload.scene = scene;
        payload.handle = handle;
        payload.priority = priority;
        payload.visible = visible ? 1u : 0u;
        proxy.updatePixelFieldKeyRaw(payload);
    }

    RenderRequest<Tile2DSlotReply>
    addTilemap(Canvas2DProxy proxy, RenderSceneId scene, const Tile2DInstanceData& data, float priority, bool visible)
    {
        AddTile2DPayload payload{};
        payload.scene = scene;
        payload.data = data;
        payload.priority = priority;
        payload.visible = visible ? 1u : 0u;
        return proxy.addTilemapRaw(payload);
    }

    void removeTilemap(Canvas2DProxy proxy, RenderSceneId scene, Tile2DInstanceHandle handle)
    {
        if (!proxy.valid())
            return;
        proxy.removeTilemapRaw(RemoveTile2DPayload{scene, handle});
    }

    void updateTilemapTransform(
        Canvas2DProxy proxy,
        RenderSceneId scene,
        Tile2DInstanceHandle handle,
        const float matrix[6],
        const std::int32_t page_delta[2]
    )
    {
        if (!proxy.valid())
            return;
        UpdateTile2DTransformPayload payload{};
        payload.scene = scene;
        payload.handle = handle;
        std::memcpy(payload.m, matrix, sizeof(payload.m));
        std::memcpy(payload.page_delta, page_delta, sizeof(payload.page_delta));
        proxy.updateTilemapTransformRaw(payload);
    }

    void updateTilemapKey(
        Canvas2DProxy proxy,
        RenderSceneId scene,
        Tile2DInstanceHandle handle,
        float priority,
        bool visible
    )
    {
        if (!proxy.valid())
            return;
        UpdateTile2DKeyPayload payload{};
        payload.scene = scene;
        payload.handle = handle;
        payload.priority = priority;
        payload.visible = visible ? 1u : 0u;
        proxy.updateTilemapKeyRaw(payload);
    }
} // namespace lux::render
