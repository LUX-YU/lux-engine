#pragma once
/**
 * @file TilemapSystem.hpp
 * @brief ECS owner for authored TilemapComponent runtime bindings.
 */

#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/ecs/tilemap/TilemapTypes.hpp>
#include <lux/engine/ecs/PersistentEntityId.hpp>
#include <lux/engine/ecs/tilemap/visibility.h>

#include <cstdint>
#include <memory>
#include <type_traits>

namespace lux::ecs
{
    class PersistentEntityIndex;
    class TilemapRuntime;

    struct TilemapSystemSnapshot final
    {
        std::uint64_t intents_enqueued{0u};
        std::uint64_t commands_applied{0u};
        std::uint64_t created_bindings{0u};
        std::uint64_t updated_bindings{0u};
        std::uint64_t destroyed_bindings{0u};
        std::uint64_t live_owned_bindings{0u};
        std::uint64_t command_rejections{0u};
        bool closing{false};
        bool closed{false};
    };

    /// Observers enqueue only small value intents. Runtime ownership and all
    /// transient component mutations happen at Schedule's command barrier.
    class LUX_ENGINE_ECS_TILEMAP_PUBLIC TilemapSystem final : public ISystem
    {
    public:
        TilemapSystem(
            TilemapRuntime& runtime,
            PersistentEntityIndex& persistent_entities);
        ~TilemapSystem() override;

        TilemapSystem(const TilemapSystem&) = delete;
        TilemapSystem& operator=(const TilemapSystem&) = delete;

        void onAdded(const SystemSetupContext& setup) override;
        void onRemoved(const SystemRemovalContext& removal) override;
        void update(const SystemUpdateContext&) override {}
        [[nodiscard]] bool supportsDynamicRemoval() const noexcept override
        {
            return true;
        }
        void requestClose() noexcept override;
        [[nodiscard]] bool closeComplete() const noexcept override;
        [[nodiscard]] bool closeNeedsOwnerTick() const noexcept override;

        [[nodiscard]] TilemapSystemSnapshot snapshot() const noexcept;
        [[nodiscard]] TilemapHandle resolveTilemap(
            const PersistentEntityRef& reference)
            const noexcept;

    private:
        friend struct TilemapIntentCommand;
        void applyReconcile(
            lux::meta::EntityRegistry& registry,
            entt::entity entity) noexcept;
        void releaseOwned(TilemapHandle handle) noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    enum class ETilemapIntentAction : std::uint8_t
    {
        RECONCILE,
        DESTROY_OWNED_HANDLE
    };

    struct TilemapIntentCommand final
    {
        using Producer = TilemapSystem;

        ETilemapIntentAction action{ETilemapIntentAction::RECONCILE};
        entt::entity entity{entt::null};
        TilemapHandle handle{};

        [[nodiscard]] std::size_t registryPublicationBytes() const noexcept
        {
            return ecsCommandSparsePublicationBytes(1u);
        }
        void prepareRegistryPublication(
            lux::meta::EntityRegistry& registry) const noexcept;

        void apply(
            lux::meta::EntityRegistry& registry,
            TilemapSystem& system) const noexcept
        {
            if (action == ETilemapIntentAction::DESTROY_OWNED_HANDLE)
                system.releaseOwned(handle);
            else
                system.applyReconcile(registry, entity);
        }
    };

    static_assert(std::is_trivially_copyable_v<TilemapIntentCommand>);
} // namespace lux::ecs
