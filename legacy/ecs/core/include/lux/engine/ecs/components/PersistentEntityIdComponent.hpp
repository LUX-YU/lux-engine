#pragma once
/**
 * @file PersistentEntityIdComponent.hpp
 * @brief Opt-in identity for references that must survive ECS unload/reload.
 *
 * An entt::entity remains the identity for ordinary live interaction. This
 * component is deliberately absent from ordinary entities and is only added
 * when persistence, replication, networking or a cross-Section reference
 * requires an identity beyond one registry residency interval.
 */

#include <lux/engine/ecs/PersistentEntityId.hpp>

#include <utility>

namespace lux::ecs
{
    class PersistentEntityIndex;

    class PersistentEntityIdComponent final
    {
    public:
        PersistentEntityIdComponent(
            const PersistentEntityIdComponent&) = default;
        PersistentEntityIdComponent& operator=(
            const PersistentEntityIdComponent&) = delete;
        // EnTT requires constructibility while relocating storage. Moving a
        // component must nevertheless not blank the source ID behind the
        // index's back, so this intentionally has copy semantics.
        PersistentEntityIdComponent(
            PersistentEntityIdComponent&& other) noexcept
            : id_(other.id_)
        {}
        PersistentEntityIdComponent& operator=(
            PersistentEntityIdComponent&&) noexcept = delete;

        [[nodiscard]] const PersistentEntityId& id()
            const noexcept
        {
            return id_;
        }

    private:
        friend class PersistentEntityIndex;

        explicit PersistentEntityIdComponent(
            PersistentEntityId id) noexcept
            : id_(std::move(id))
        {
        }

        PersistentEntityId id_;
    };
}
