#pragma once
/**
 * @file StaticCollider3DBindingComponent.hpp
 * @brief Non-reflected, non-persistent ownership of prepared 3D physics data.
 */

#include <lux/engine/ecs/physics3d/systems/Physics3DScene.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/runtime/spatial3d/physics/StaticCollider3DPrepareService.hpp>
#include <lux/engine/runtime/spatial3d/physics/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::runtime::spatial3d
{
    class StaticCollider3DSystem;

    /// Shared only between the domain owner table and its transient ECS
    /// binding.  It pins both the cooked bytes and the private Jolt bodies.
    /// No runtime pointer is serialized into EntityScene content.
    class LUX_ENGINE_RUNTIME_SPATIAL3D_PHYSICS_PUBLIC
        StaticCollider3DBinding final
    {
      public:
        ~StaticCollider3DBinding() noexcept;
        StaticCollider3DBinding(const StaticCollider3DBinding&) = delete;
        StaticCollider3DBinding&
        operator=(const StaticCollider3DBinding&) = delete;

        [[nodiscard]] bool active() const noexcept;
        [[nodiscard]] std::uint64_t generation() const noexcept;
        [[nodiscard]] const lux::ecs::scene_format::ContentBlobRef&
        content() const noexcept;

        /// Immediate backend hide used by on_destroy.  Backend allocation and
        /// blob ownership survive until the domain retirement queue releases
        /// the last shared owner.
        void hide() noexcept;
        /// Claims the single retirement token and hides the backend bodies.
        /// False means another observer/owner already queued this binding.
        [[nodiscard]] bool beginRetirement() noexcept;
        /// Destroys at most @p maximum_units of already-hidden backend data.
        /// A unit is one body or one unadopted prepared shape.
        /// Returns true once backend and budget ownership are fully released.
        [[nodiscard]] bool retireSome(
            std::uint32_t maximum_units) noexcept;
        /// Destruction fallback. Normal domain shutdown uses retireSome().
        void retire() noexcept;
        [[nodiscard]] bool retired() const noexcept;
        [[nodiscard]] std::uint32_t remainingBodies() const noexcept;
        [[nodiscard]] std::uint32_t
        remainingRetirementUnits() const noexcept;
        [[nodiscard]] std::size_t accountedBytes() const noexcept;

      private:
        friend class StaticCollider3DSystem;
        StaticCollider3DBinding(
            std::uint64_t generation,
            std::shared_ptr<lux::ecs::Physics3DScene> scene,
            lux::ecs::entity_scene::ContentBlobLease content,
            StaticCollider3DPrepareBudgetLease budget,
            std::unique_ptr<lux::ecs::Physics3DStaticBatchLease> physics)
        noexcept;
        void publish() noexcept;

        std::uint64_t generation_{0u};
        std::shared_ptr<lux::ecs::Physics3DScene> scene_;
        lux::ecs::entity_scene::ContentBlobLease content_;
        StaticCollider3DPrepareBudgetLease budget_;
        std::unique_ptr<lux::ecs::Physics3DStaticBatchLease> physics_;
        bool retirement_queued_{false};
        bool retired_{false};
    };

    struct StaticCollider3DBindingComponent final
    {
        std::shared_ptr<StaticCollider3DBinding> binding;
    };

    enum class EStaticCollider3DState : std::uint8_t
    {
        WAITING_CONTENT,
        WAITING_BACKGROUND,
        STAGING,
        READY,
        ACTIVE,
        FAILED
    };

    enum class EStaticCollider3DFailure : std::uint8_t
    {
        NONE,
        INVALID_REFERENCE,
        CONTENT_UNAVAILABLE,
        DECODE_FAILED,
        INVALID_TRANSFORM,
        PREPARE_FAILED
    };

    struct StaticCollider3DStatusComponent final
    {
        EStaticCollider3DState state{
            EStaticCollider3DState::WAITING_CONTENT};
        EStaticCollider3DFailure failure{EStaticCollider3DFailure::NONE};
        std::uint64_t generation{0u};
    };
} // namespace lux::runtime::spatial3d
