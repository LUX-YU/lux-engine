#pragma once
/**
 * @file TilemapChunkSystem.hpp
 * @brief ECS-first owner for TileChunk2D content residency.
 */

#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/ecs/entity_scene/ContentBlobStorage.hpp>
#include <lux/engine/ecs/tilemap/streaming/TilemapPreparePort.hpp>
#include <lux/engine/ecs/tilemap/TilemapTypes.hpp>
#include <lux/engine/ecs/tilemap/streaming/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

namespace lux::ecs
{
    class TilemapRuntime;
    class TilemapSystem;
}

namespace lux::ecs::tilemap::streaming
{
    /// Optional policy seam. Fixed/manual scenes omit it and keep every
    /// resident chunk active; Infinite2D adapts its own interest system here
    /// without introducing a reverse dependency into this Tilemap leaf.
    class LUX_ENGINE_ECS_TILEMAP_STREAMING_PUBLIC
        TilemapChunkActivity2D
    {
    public:
        virtual ~TilemapChunkActivity2D() = default;
        [[nodiscard]] virtual bool isActive(
            lux::ecs::TileChunkCoord coordinate) const noexcept = 0;
    };

    enum class ETilemapChunkDomainState : std::uint8_t
    {
        WAITING_TILEMAP,
        STAGING,
        READY,
        FAILED
    };

    enum class ETilemapChunkDomainError : std::uint8_t
    {
        NONE,
        CONTENT_UNAVAILABLE,
        CONTENT_INVALID,
        RUNTIME_REJECTED,
        CAPACITY_EXHAUSTED,
        COMMAND_REJECTED
    };

    /// Transient domain status. Section ACTIVE is independent of this state.
    struct TilemapChunkDomainStateComponent final
    {
        ETilemapChunkDomainState state{
            ETilemapChunkDomainState::WAITING_TILEMAP};
        ETilemapChunkDomainError error{
            ETilemapChunkDomainError::NONE};
        std::uint64_t generation{0u};
    };

    struct TilemapChunkSystemConfig final
    {
        std::uint32_t maximum_tracked_chunks{256u};
        std::uint32_t maximum_retirements_per_update{1u};
    };

    struct TilemapChunkSystemSnapshot final
    {
        std::size_t waiting_chunks{0u};
        std::size_t staging_chunks{0u};
        std::size_t ready_chunks{0u};
        std::size_t failed_chunks{0u};
        std::size_t tracked_chunks{0u};
        std::size_t background_chunks{0u};
        std::size_t retiring_chunks{0u};
        std::size_t owned_blob_leases{0u};
        std::uint64_t preparation_attempts{0u};
        std::uint64_t published_chunks{0u};
        std::uint64_t hidden_chunks{0u};
        std::uint64_t retired_chunks{0u};
        std::uint64_t stale_completions{0u};
        std::uint64_t queue_backpressure{0u};
        std::uint64_t commands_enqueued{0u};
        std::uint64_t commands_applied{0u};
        std::uint64_t command_rejections{0u};
        std::uint64_t capacity_rejections{0u};
        std::uint32_t retirement_granules_last_update{0u};
        std::uint32_t maximum_retirement_granules_per_update{0u};
        bool closing{false};
        bool closed{false};
    };

    class LUX_ENGINE_ECS_TILEMAP_STREAMING_PUBLIC
        TilemapChunkSystem final : public lux::ecs::ISystem
    {
    public:
        TilemapChunkSystem(
            TilemapPrepareClient preparation,
            lux::ecs::TilemapRuntime& runtime,
            lux::ecs::TilemapSystem& tilemaps,
            lux::ecs::entity_scene::ContentBlobClient content,
            const TilemapChunkActivity2D* activity = nullptr,
            TilemapChunkSystemConfig config = {});
        ~TilemapChunkSystem() override;

        TilemapChunkSystem(const TilemapChunkSystem&) = delete;
        TilemapChunkSystem& operator=(const TilemapChunkSystem&) = delete;

        void onAdded(const lux::ecs::SystemSetupContext& setup) override;
        void onRemoved(
            const lux::ecs::SystemRemovalContext& removal) override;
        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }
        void update(const lux::ecs::SystemUpdateContext& context) override;
        [[nodiscard]] std::span<const Type> prerequisites() const noexcept
            override;
        [[nodiscard]] std::span<const Type> runsAfter() const noexcept
            override;

        void requestClose() noexcept override;
        void requestClose(lux::ecs::SystemCloseProgressSink progress)
            noexcept override;
        [[nodiscard]] bool closeComplete() const noexcept override;
        [[nodiscard]] bool closeNeedsOwnerTick() const noexcept override;
        [[nodiscard]] TilemapChunkSystemSnapshot snapshot() const noexcept;

    private:
        friend struct TilemapChunkIntentCommand;
        void applyReconcile(
            lux::ecs::Registry& registry,
            entt::entity entity) noexcept;
        void applyPublish(
            lux::ecs::Registry& registry,
            entt::entity entity,
            std::uint32_t slot,
            std::uint32_t generation) noexcept;
        void applyRetire(
            lux::ecs::Registry& registry,
            std::uint32_t slot,
            std::uint32_t generation) noexcept;
        void applyCloseFence() noexcept;
        void acceptPreparation(
            std::uint32_t slot,
            std::uint32_t generation,
            lux::async::OperationOutcome<PrepareTilemapChunk> outcome) noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    enum class ETilemapChunkIntentAction : std::uint8_t
    {
        RECONCILE,
        PUBLISH,
        RETIRE,
        CLOSE_FENCE
    };

    struct TilemapChunkIntentCommand final
    {
        using Producer = TilemapChunkSystem;

        ETilemapChunkIntentAction action{
            ETilemapChunkIntentAction::RECONCILE};
        entt::entity entity{entt::null};
        std::uint32_t slot{~std::uint32_t{0u}};
        std::uint32_t generation{0u};

        [[nodiscard]] std::size_t registryPublicationBytes() const noexcept
        {
            return lux::ecs::ecsCommandSparsePublicationBytes(2u);
        }
        void prepareRegistryPublication(
            lux::ecs::Registry& registry) const noexcept;

        void apply(
            lux::ecs::Registry& registry,
            TilemapChunkSystem& system) const noexcept;
    };

    static_assert(std::is_trivially_copyable_v<
        TilemapChunkIntentCommand>);
} // namespace lux::ecs::tilemap::streaming
