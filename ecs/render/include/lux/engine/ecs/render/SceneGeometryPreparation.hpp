#pragma once
/**
 * @file SceneGeometryPreparation.hpp
 * @brief Typed ECS-facing ports for background geometry preparation.
 *
 * The operation values are renderer/ECS data contracts. Queueing, worker
 * selection, admission policy and sender adaptation remain Runtime concerns.
 */

#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/core/async/OperationPort.hpp>
#include <lux/engine/ecs/scene_format/EntitySection.hpp>
#include <lux/engine/function/render/client/features/render_cluster/RenderClusterOperation.hpp>
#include <lux/engine/function/render/standard/content/ClassicMeshBatch.hpp>
#include <lux/engine/resource/asset/AssetId.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lux::ecs
{
    enum class ESceneGeometryPrepareError : std::uint8_t
    {
        INVALID_REQUEST,
        CONTENT_MISMATCH,
        DECODE_FAILED,
        UNSUPPORTED_CONTENT,
        SERVICE_CLOSED
    };

    struct SceneGeometryPrepareFailure final
    {
        ESceneGeometryPrepareError code{
            ESceneGeometryPrepareError::INVALID_REQUEST};
        std::string detail;
    };

    struct PreparedClassicMeshBatch final
    {
        PreparedClassicMeshBatch() noexcept = default;
        PreparedClassicMeshBatch(PreparedClassicMeshBatch&&) noexcept =
            default;
        PreparedClassicMeshBatch& operator=(PreparedClassicMeshBatch&&)
            noexcept = default;
        PreparedClassicMeshBatch(const PreparedClassicMeshBatch&) = delete;
        PreparedClassicMeshBatch& operator=(
            const PreparedClassicMeshBatch&) = delete;

        std::uint64_t request_generation{0u};
        std::shared_ptr<lux::classic_mesh::ClassicMeshBatchBlobV1> decoded;
        std::shared_ptr<std::vector<
            lux::render::RenderClusterWireInstance>> wire;
        std::vector<lux::asset::asset_id_t> mesh_assets;
        std::vector<lux::asset::asset_id_t> material_assets;
    };

    struct PreparedTerrainTile final
    {
        PreparedTerrainTile() noexcept = default;
        PreparedTerrainTile(PreparedTerrainTile&&) noexcept = default;
        PreparedTerrainTile& operator=(PreparedTerrainTile&&) noexcept =
            default;
        PreparedTerrainTile(const PreparedTerrainTile&) = delete;
        PreparedTerrainTile& operator=(const PreparedTerrainTile&) = delete;

        std::uint64_t request_generation{0u};
        float height_min{0.0f};
        float height_max{1.0f};
        float sample_spacing{1.0f};
        std::uint8_t weight_layer_count{0u};
        std::shared_ptr<std::vector<std::byte>> wire;
    };

    struct PrepareClassicMeshBatch final
    {
        using Value = PreparedClassicMeshBatch;
        using Error = SceneGeometryPrepareFailure;

        PrepareClassicMeshBatch() noexcept = default;
        PrepareClassicMeshBatch(
            lux::cxx::SharedBytes<> content_value,
            lux::ecs::scene_format::ContentBlobRef reference_value,
            std::uint64_t generation_value) noexcept
            : content(std::move(content_value)),
              reference(std::move(reference_value)),
              request_generation(generation_value)
        {}
        PrepareClassicMeshBatch(PrepareClassicMeshBatch&&) noexcept = default;
        PrepareClassicMeshBatch& operator=(PrepareClassicMeshBatch&&)
            noexcept = default;
        PrepareClassicMeshBatch(const PrepareClassicMeshBatch&) = delete;
        PrepareClassicMeshBatch& operator=(const PrepareClassicMeshBatch&) =
            delete;

        lux::cxx::SharedBytes<> content;
        lux::ecs::scene_format::ContentBlobRef reference;
        std::uint64_t request_generation{0u};

        // Runtime admission wraps the lower-level OperationPort and pins its
        // reservation here until the handler publishes a terminal outcome.
        std::shared_ptr<void> admission_lifetime;
    };

    struct PrepareTerrainTile final
    {
        using Value = PreparedTerrainTile;
        using Error = SceneGeometryPrepareFailure;

        PrepareTerrainTile() noexcept = default;
        PrepareTerrainTile(
            lux::cxx::SharedBytes<> content_value,
            lux::ecs::scene_format::ContentBlobRef reference_value,
            std::uint64_t generation_value) noexcept
            : content(std::move(content_value)),
              reference(std::move(reference_value)),
              request_generation(generation_value)
        {}
        PrepareTerrainTile(PrepareTerrainTile&&) noexcept = default;
        PrepareTerrainTile& operator=(PrepareTerrainTile&&) noexcept =
            default;
        PrepareTerrainTile(const PrepareTerrainTile&) = delete;
        PrepareTerrainTile& operator=(const PrepareTerrainTile&) = delete;

        lux::cxx::SharedBytes<> content;
        lux::ecs::scene_format::ContentBlobRef reference;
        std::uint64_t request_generation{0u};
        std::shared_ptr<void> admission_lifetime;
    };

    using ClassicMeshPreparePort =
        lux::async::OperationPort<PrepareClassicMeshBatch>;
    using TerrainPreparePort = lux::async::OperationPort<PrepareTerrainTile>;
}
