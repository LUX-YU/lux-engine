#pragma once
/**
 * @file Infinite2DPixelSystem.hpp
 * @brief Pixel-domain consumer for resident PixelChunk2D ECS facts.
 */

#include <lux/engine/ecs/pixel/PixelFieldTypes.hpp>
#include <lux/engine/ecs/entity_scene/ContentBlobStorage.hpp>
#include <lux/engine/ecs/pixel/streaming/Infinite2DPixelPreparePort.hpp>
#include <lux/engine/ecs/pixel/streaming/visibility.h>
#include <lux/engine/ecs/systems/ISystem.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

namespace lux::ecs
{
    class PixelFieldRuntime;
    class PixelFieldSystem;
    class PixelChunkPersistenceStore;
}

namespace lux::ecs::spatial2d::streaming
{
    class SpatialInterest2DSystem;
}

namespace lux::ecs::pixel::streaming
{
    enum class EPixelChunkDomainState : std::uint8_t
    {
        WAITING_FIELD,
        STAGING,
        READY,
        FAILED
    };

    enum class EPixelChunkDomainError : std::uint8_t
    {
        NONE,
        CONTENT_UNAVAILABLE,
        CONTENT_INVALID,
        RUNTIME_REJECTED,
        COMMAND_REJECTED
    };

    /// Transient, non-reflected domain status. It is never cooked and does
    /// not make Section ACTIVE depend on Pixel readiness.
    struct PixelChunkDomainStateComponent final
    {
        EPixelChunkDomainState state{
            EPixelChunkDomainState::WAITING_FIELD};
        EPixelChunkDomainError error{EPixelChunkDomainError::NONE};
    };

    struct Infinite2DPixelSnapshot final
    {
        std::size_t waiting_chunks{0u};
        std::size_t staging_chunks{0u};
        std::size_t ready_chunks{0u};
        std::size_t failed_chunks{0u};
        std::size_t resident_chunks{0u};
        std::size_t active_chunks{0u};
        std::size_t admission_chunks{0u};
        std::size_t background_chunks{0u};
        std::size_t publish_pending_chunks{0u};
        std::size_t retiring_chunks{0u};
        std::uint64_t prepared_chunks{0u};
        std::uint64_t published_chunks{0u};
        std::uint64_t hidden_chunks{0u};
        std::uint64_t retired_chunks{0u};
        std::uint64_t retirement_barriers{0u};
        std::uint64_t retirement_granules{0u};
        std::uint32_t retirement_granules_last_update{0u};
        std::uint32_t maximum_retirement_granules_per_update{0u};
        std::uint64_t retries{0u};
        std::uint64_t queue_backpressure{0u};
        std::uint64_t stale_completions{0u};
        std::uint64_t command_rejections{0u};
        std::uint64_t persistence_recoveries{0u};
        std::uint64_t persistence_failures{0u};
        bool closing{false};
        bool operations_drained{false};
        bool closed{false};
    };

    class LUX_ENGINE_ECS_PIXEL_STREAMING_PUBLIC
    Infinite2DPixelSystem final : public lux::ecs::ISystem
    {
    public:
        Infinite2DPixelSystem(
            Infinite2DPixelPrepareClient preparation,
            lux::ecs::PixelFieldRuntime& runtime,
            lux::ecs::PixelFieldSystem& fields,
            lux::ecs::PixelChunkPersistenceStore& persistence,
            lux::ecs::entity_scene::ContentBlobClient content,
            const lux::ecs::spatial2d::streaming::SpatialInterest2DSystem&
                activity);
        ~Infinite2DPixelSystem() override;

        Infinite2DPixelSystem(const Infinite2DPixelSystem&) = delete;
        Infinite2DPixelSystem& operator=(const Infinite2DPixelSystem&) =
            delete;

        [[nodiscard]] Infinite2DPixelSnapshot snapshot() const noexcept;
        void requestClose() noexcept override;
        void requestClose(lux::ecs::SystemCloseProgressSink progress)
            noexcept override;
        [[nodiscard]] bool closeComplete() const noexcept override;
        [[nodiscard]] bool closeNeedsOwnerTick() const noexcept override;

        void onAdded(const lux::ecs::SystemSetupContext& setup) override;
        void update(const lux::ecs::SystemUpdateContext& context) override;
        [[nodiscard]] std::span<const Type> prerequisites() const noexcept
            override;
        [[nodiscard]] std::span<const Type> runsAfter() const noexcept
            override;

    private:
        friend struct Infinite2DPixelCommand;

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
        void acceptPreparation(
            std::uint32_t slot,
            std::uint32_t generation,
            lux::async::OperationOutcome<PrepareInfinite2DPixelChunk> outcome)
            noexcept;
        void applyCloseFence() noexcept;
        void retireOne() noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    enum class EInfinite2DPixelCommandAction : std::uint8_t
    {
        RECONCILE,
        PUBLISH,
        RETIRE,
        CLOSE_FENCE
    };

    struct Infinite2DPixelCommand final
    {
        using Producer = Infinite2DPixelSystem;

        EInfinite2DPixelCommandAction action{
            EInfinite2DPixelCommandAction::RECONCILE};
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
            Infinite2DPixelSystem& system) const noexcept;
    };

    static_assert(std::is_trivially_copyable_v<Infinite2DPixelCommand>);
} // namespace lux::ecs::pixel::streaming
