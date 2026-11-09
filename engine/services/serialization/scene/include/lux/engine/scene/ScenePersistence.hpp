#pragma once

#include <lux/engine/scene/SceneAssetCodec.hpp>
#include <lux/engine/assets/AssetRef.hpp>
#include <lux/engine/assets/AssetDataRef.hpp>
#include <lux/engine/error/Error.hpp>
#include <exec/task.hpp>

namespace lux::engine::assets { class AssetRepository; }

namespace lux::scene
{
    // Inputs own complete changed blocks and matching codec-produced summaries. Omitted blocks are preserved.
    // A successful publication invalidates Repository's current snapshot; existing readers retain their source.
    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC exec::task<cxx::expected<void, error::Error>>
    saveScene(
        engine::assets::AssetRepository& assets, engine::assets::AssetRef<SceneAsset> asset,
        engine::assets::AssetDataRef<SceneData> base, SceneDescription description,
        std::vector<SceneBlockUpdate> updates, SceneStreamingData streaming
    ) noexcept;

    // Creates a new asset. The returned reference is published only after content and root are durable.
    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC
    exec::task<cxx::expected<engine::assets::AssetRef<SceneAsset>, error::Error>>
    createSceneAsset(
        engine::assets::AssetRepository& assets, std::string path, asset::AssetInfo info,
        SceneDescription description, std::vector<SceneBlockUpdate> updates, SceneStreamingData streaming
    ) noexcept;

    // Uses a new AssetId while preserving internal object/block identities. Repository bounds each range read.
    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC
    exec::task<cxx::expected<engine::assets::AssetRef<SceneAsset>, error::Error>>
    copySceneAsset(
        engine::assets::AssetRepository& assets, engine::assets::AssetDataRef<SceneData> base,
        std::string path, asset::AssetInfo info
    ) noexcept;

    // Copies only reachable blocks with a bounded buffer. Open old readers remain valid.
    // Owned orphan cleanup is best-effort after publication; locked files await the next explicit compaction.
    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC exec::task<cxx::expected<void, error::Error>>
    compactSceneAsset(
        engine::assets::AssetRepository& assets, engine::assets::AssetRef<SceneAsset> asset,
        engine::assets::AssetDataRef<SceneData> base, std::size_t copy_buffer_bytes
    ) noexcept;
} // namespace lux::scene
