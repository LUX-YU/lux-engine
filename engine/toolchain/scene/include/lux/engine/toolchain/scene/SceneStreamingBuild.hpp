#pragma once

#include <lux/engine/assets/AssetDataRef.hpp>
#include <lux/engine/error/Error.hpp>
#include <lux/cxx/compile_time/expected.hpp>
#include <exec/task.hpp>
#include <lux/engine/scene/SceneData.hpp>
#include <lux/engine/scene/SceneStreamingData.hpp>
#include <lux/engine/world/SceneGridConfiguration.hpp>
#include <lux/engine/world/SceneGridConfiguration.scene_codec.hpp>

namespace lux::engine { class EngineMetaFunctionRegistry; }
namespace lux::engine::assets { class AssetRepository; }

namespace lux::engine::toolchain
{
    // Synchronous pure construction; a declaration selects its dimension without owning an algorithm object.
    template <scene::ESceneSpatialDimension Dimension>
    [[nodiscard]] cxx::expected<scene::SceneSpatialIndex, error::Error>
    buildSpatialIndex(std::span<const scene::SceneSpatialEntry> entries, std::span<const std::byte> configuration) noexcept;

    // Base is optional for a new asset. Description/updates/metadata remain immutable until completion.
    // Unchanged blocks read only summary ranges; CPU work uses shared Execution.
    [[nodiscard]] exec::task<cxx::expected<scene::SceneStreamingData, error::Error>>
    buildSceneStreaming(
        assets::AssetRepository& assets, assets::AssetDataRef<scene::SceneData> base,
        const scene::SceneDescription& description, std::span<const scene::SceneBlockUpdate> updates,
        const EngineMetaFunctionRegistry& metadata, std::size_t max_metadata_bytes
    ) noexcept;
} // namespace lux::engine::toolchain
