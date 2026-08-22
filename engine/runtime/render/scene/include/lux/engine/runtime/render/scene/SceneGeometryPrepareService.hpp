#pragma once
/**
 * @file SceneGeometryPrepareService.hpp
 * @brief Bounded typed CPU preparation for ECS geometry presentation leaves.
 */

#include <lux/engine/ecs/render/SceneGeometryPreparation.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeBuilder.hpp>
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

    namespace detail
    {
        struct SceneGeometryPrepareControl;
        struct SceneGeometryPrepareReservation;
    } // namespace detail

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

        [[nodiscard]] lux::ecs::ClassicMeshPreparePort classicMeshPort()
            const noexcept;
        [[nodiscard]] lux::ecs::TerrainPreparePort terrainPort()
            const noexcept;
        [[nodiscard]] SceneGeometryPrepareSnapshot snapshot() const noexcept;
        void close() noexcept;

    private:
        SceneGeometryPrepareService(
            std::shared_ptr<detail::SceneGeometryPrepareControl> control,
            lux::async::OperationPort<lux::ecs::PrepareClassicMeshBatch>
                classic_mesh,
            lux::async::OperationPort<lux::ecs::PrepareTerrainTile>
                terrain) noexcept;

        std::shared_ptr<detail::SceneGeometryPrepareControl> control_;
        lux::async::OperationPort<lux::ecs::PrepareClassicMeshBatch>
            classic_mesh_;
        lux::async::OperationPort<lux::ecs::PrepareTerrainTile> terrain_;
        bool closed_{false};
    };
} // namespace lux::runtime
