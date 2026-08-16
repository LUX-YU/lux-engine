#pragma once
/**
 * @file SceneGeometryPrepareService.hpp
 * @brief Bounded typed CPU preparation for ECS geometry presentation leaves.
 */

#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/function/render/client/features/render_cluster/RenderClusterOperation.hpp>
#include <lux/engine/resource/asset/AssetId.hpp>
#include <lux/engine/resource/classic_mesh/ClassicMeshBatch.hpp>
#include <lux/engine/resource/entity_scene/EntitySection.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/render/scene/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lux::runtime
{
    inline constexpr std::size_t kClassicMeshPrepareQueueCapacity = 16u;
    inline constexpr std::size_t kClassicMeshPrepareByteBudget =
        768u * 1024u * 1024u;
    inline constexpr std::size_t kClassicMeshPrepareDrainBatch = 4u;
    inline constexpr std::size_t kTerrainPrepareQueueCapacity = 64u;
    inline constexpr std::size_t kTerrainPrepareByteBudget =
        256u * 1024u * 1024u;
    inline constexpr std::size_t kTerrainPrepareDrainBatch = 8u;

    struct SceneGeometryPrepareDomainConfig final
    {
        std::size_t capacity{1u};
        std::size_t byte_budget{1u};
        std::size_t drain_batch{1u};

        [[nodiscard]] bool valid() const noexcept
        {
            return capacity != 0u && byte_budget != 0u &&
                drain_batch != 0u;
        }
    };

    struct SceneGeometryPrepareConfig final
    {
        SceneGeometryPrepareDomainConfig classic_mesh{
            kClassicMeshPrepareQueueCapacity,
            kClassicMeshPrepareByteBudget,
            kClassicMeshPrepareDrainBatch};
        SceneGeometryPrepareDomainConfig terrain{
            kTerrainPrepareQueueCapacity,
            kTerrainPrepareByteBudget,
            kTerrainPrepareDrainBatch};

        [[nodiscard]] bool valid() const noexcept
        {
            return classic_mesh.valid() && terrain.valid();
        }
    };

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
        /// Allocated on the background worker.  The owner-thread leaf fills
        /// handles and entity-relative transforms incrementally.
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

    namespace detail
    {
        struct SceneGeometryPrepareControl;
        struct SceneGeometryPrepareReservation;
    } // namespace detail

    struct PrepareClassicMeshBatch final
    {
        using Value = PreparedClassicMeshBatch;
        using Error = SceneGeometryPrepareFailure;

        PrepareClassicMeshBatch() noexcept = default;
        PrepareClassicMeshBatch(
            lux::cxx::SharedBytes<> content_value,
            lux::entity_scene::ContentBlobRef reference_value,
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
        lux::entity_scene::ContentBlobRef reference;
        std::uint64_t request_generation{0u};

    private:
        std::shared_ptr<detail::SceneGeometryPrepareReservation> admission_;

        friend class ClassicMeshPrepareClient;
        friend class SceneGeometryPrepareService;
    };

    struct PrepareTerrainTile final
    {
        using Value = PreparedTerrainTile;
        using Error = SceneGeometryPrepareFailure;

        PrepareTerrainTile() noexcept = default;
        PrepareTerrainTile(
            lux::cxx::SharedBytes<> content_value,
            lux::entity_scene::ContentBlobRef reference_value,
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
        lux::entity_scene::ContentBlobRef reference;
        std::uint64_t request_generation{0u};

    private:
        std::shared_ptr<detail::SceneGeometryPrepareReservation> admission_;

        friend class TerrainPrepareClient;
        friend class SceneGeometryPrepareService;
    };

    using ClassicMeshPrepareSender =
        lux::exec::AsyncExecuteSender<PrepareClassicMeshBatch>;
    using TerrainPrepareSender =
        lux::exec::AsyncExecuteSender<PrepareTerrainTile>;

    class LUX_RUNTIME_RENDER_SCENE_PUBLIC ClassicMeshPrepareClient final
    {
    public:
        ClassicMeshPrepareClient() noexcept = default;

        [[nodiscard]] lux::cxx::expected<
            ClassicMeshPrepareSender,
            lux::exec::EAsyncSubmitError>
        execute(PrepareClassicMeshBatch request) const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept;

    private:
        friend class SceneGeometryPrepareService;
        ClassicMeshPrepareClient(
            std::weak_ptr<detail::SceneGeometryPrepareControl> control,
            std::uint64_t generation,
            lux::exec::AsyncOperationClient<PrepareClassicMeshBatch>
                operation) noexcept;

        std::weak_ptr<detail::SceneGeometryPrepareControl> control_;
        std::uint64_t generation_{0u};
        lux::exec::AsyncOperationClient<PrepareClassicMeshBatch> operation_;
    };

    class LUX_RUNTIME_RENDER_SCENE_PUBLIC TerrainPrepareClient final
    {
    public:
        TerrainPrepareClient() noexcept = default;

        [[nodiscard]] lux::cxx::expected<
            TerrainPrepareSender,
            lux::exec::EAsyncSubmitError>
        execute(PrepareTerrainTile request) const noexcept;
        [[nodiscard]] explicit operator bool() const noexcept;

    private:
        friend class SceneGeometryPrepareService;
        TerrainPrepareClient(
            std::weak_ptr<detail::SceneGeometryPrepareControl> control,
            std::uint64_t generation,
            lux::exec::AsyncOperationClient<PrepareTerrainTile>
                operation) noexcept;

        std::weak_ptr<detail::SceneGeometryPrepareControl> control_;
        std::uint64_t generation_{0u};
        lux::exec::AsyncOperationClient<PrepareTerrainTile> operation_;
    };

    struct SceneGeometryPrepareDomainSnapshot final
    {
        /// Reservations which have not yet produced a terminal operation
        /// outcome.  Prepared values already handed to a Scene consumer are
        /// owned and accounted by that consumer, not by this service.
        std::size_t active_requests{0u};
        std::size_t active_bytes{0u};
        std::size_t request_high_water{0u};
        std::size_t byte_high_water{0u};
    };

    struct SceneGeometryPrepareSnapshot final
    {
        bool closing{false};
        SceneGeometryPrepareDomainSnapshot classic_mesh;
        SceneGeometryPrepareDomainSnapshot terrain;
    };

    class LUX_RUNTIME_RENDER_SCENE_PUBLIC SceneGeometryPrepareService final
    {
    public:
        [[nodiscard]] static lux::cxx::expected<
            SceneGeometryPrepareService,
            lux::exec::AsyncAssemblyFailure>
        addTo(
            lux::exec::AsyncRuntimeBuilder& builder,
            SceneGeometryPrepareConfig config = {});

        SceneGeometryPrepareService(
            const SceneGeometryPrepareService&) = delete;
        SceneGeometryPrepareService& operator=(
            const SceneGeometryPrepareService&) = delete;
        SceneGeometryPrepareService(
            SceneGeometryPrepareService&& other) noexcept;
        SceneGeometryPrepareService& operator=(
            SceneGeometryPrepareService&& other) noexcept;
        ~SceneGeometryPrepareService();

        [[nodiscard]] ClassicMeshPrepareClient classicMeshClient()
            const noexcept;
        [[nodiscard]] TerrainPrepareClient terrainClient() const noexcept;
        [[nodiscard]] SceneGeometryPrepareSnapshot snapshot() const noexcept;
        void close() noexcept;

    private:
        SceneGeometryPrepareService(
            std::shared_ptr<detail::SceneGeometryPrepareControl> control,
            lux::exec::AsyncOperationClient<PrepareClassicMeshBatch>
                classic_mesh,
            lux::exec::AsyncOperationClient<PrepareTerrainTile>
                terrain) noexcept;

        std::shared_ptr<detail::SceneGeometryPrepareControl> control_;
        lux::exec::AsyncOperationClient<PrepareClassicMeshBatch>
            classic_mesh_;
        lux::exec::AsyncOperationClient<PrepareTerrainTile> terrain_;
        bool closed_{false};
    };
} // namespace lux::runtime
