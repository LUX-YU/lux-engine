#pragma once
/**
 * @file PixelFieldSystem.hpp
 * @brief ECS owner for authored PixelField2DComponent runtime bindings.
 */

#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/ecs/pixel/PixelFieldTypes.hpp>
#include <lux/engine/ecs/PersistentEntityId.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <memory>
#include <type_traits>

namespace lux::ecs
{
    class PixelFieldRuntime;
    class PersistentEntityIndex;

    struct PixelFieldSystemSnapshot final
    {
        bool closing{false};
        bool closed{false};
        std::uint64_t intents_enqueued{0u};
        std::uint64_t commands_applied{0u};
        std::uint64_t create_commands{0u};
        std::uint64_t update_commands{0u};
        std::uint64_t destroy_commands{0u};
        std::uint64_t live_owned_bindings{0u};
        std::uint64_t frame_fields_visited{0u};
        std::uint64_t frame_updates{0u};
        std::uint64_t command_rejections{0u};
    };

    /// Observes only authored field facts. Observer callbacks enqueue small
    /// value commands; the unique Schedule barrier owns binding structure and
    /// PixelFieldRuntime create/destroy. Per-frame work is O(live ECS fields),
    /// independent of unloaded history or backing-store capacity.
    class LUX_FUNCTION_PUBLIC PixelFieldSystem final : public ISystem
    {
    public:
        PixelFieldSystem(
            PixelFieldRuntime& runtime,
            PersistentEntityIndex& persistent_entities);
        ~PixelFieldSystem() override;
        PixelFieldSystem(const PixelFieldSystem&) = delete;
        PixelFieldSystem& operator=(const PixelFieldSystem&) = delete;

        void onAdded(const SystemSetupContext& setup) override;
        void update(const SystemUpdateContext& context) override;
        void requestClose() noexcept override;
        [[nodiscard]] bool closeComplete() const noexcept override;
        [[nodiscard]] bool closeNeedsOwnerTick() const noexcept override;

        [[nodiscard]] PixelFieldSystemSnapshot snapshot() const noexcept;
        /// Resolve one optional cross-Section field reference without
        /// exposing the transient binding component outside the Pixel domain.
        [[nodiscard]] PixelFieldHandle resolveField(
            const PersistentEntityRef& reference)
            const noexcept;

    private:
        friend struct PixelFieldIntentCommand;
        void applyIntent(
            lux::ecs::Registry& registry,
            entt::entity entity) noexcept;
        void releaseOwned(PixelFieldHandle handle) noexcept;

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    enum class EPixelFieldIntentAction : std::uint8_t
    {
        RECONCILE_ENTITY,
        DESTROY_OWNED_HANDLE
    };

    struct PixelFieldIntentCommand final
    {
        using Producer = PixelFieldSystem;
        EPixelFieldIntentAction action{
            EPixelFieldIntentAction::RECONCILE_ENTITY};
        entt::entity entity{entt::null};
        PixelFieldHandle field{};

        [[nodiscard]] std::size_t registryPublicationBytes() const noexcept
        {
            return ecsCommandSparsePublicationBytes(1u);
        }
        void prepareRegistryPublication(
            lux::ecs::Registry& registry) const noexcept;

        void apply(
            lux::ecs::Registry& registry,
            PixelFieldSystem& system) const noexcept
        {
            if (action == EPixelFieldIntentAction::DESTROY_OWNED_HANDLE)
                system.releaseOwned(field);
            else
                system.applyIntent(registry, entity);
        }
    };

    static_assert(std::is_trivially_copyable_v<PixelFieldIntentCommand>);
}
