#include <lux/engine/runtime/spatial3d/physics/StaticCollider3DBindingComponent.hpp>

#include <cstdlib>
#include <limits>
#include <utility>

namespace lux::runtime::spatial3d
{
    StaticCollider3DBinding::StaticCollider3DBinding(
        std::uint64_t generation,
        std::shared_ptr<lux::ecs::Physics3DScene> scene,
        lux::runtime::entity_scene::ContentBlobLease content,
        StaticCollider3DPrepareBudgetLease budget,
        std::unique_ptr<lux::ecs::Physics3DStaticBatchLease> physics) noexcept
        : generation_(generation),
          scene_(std::move(scene)),
          content_(std::move(content)),
          budget_(std::move(budget)),
          physics_(std::move(physics))
    {
        if (generation_ == 0u || !scene_ || !budget_ || !physics_ ||
            physics_->active() ||
            (!content_ && physics_->remainingBodies() != 0u))
        {
            std::abort();
        }
    }

    StaticCollider3DBinding::~StaticCollider3DBinding() noexcept
    {
        retire();
    }

    bool StaticCollider3DBinding::active() const noexcept
    {
        return !retired_ && physics_ && physics_->active();
    }

    std::uint64_t StaticCollider3DBinding::generation() const noexcept
    {
        return generation_;
    }

    const lux::ecs::scene_format::ContentBlobRef&
    StaticCollider3DBinding::content() const noexcept
    {
        return content_.reference();
    }

    void StaticCollider3DBinding::publish() noexcept
    {
        if (!physics_ || physics_->active() || retirement_queued_ || retired_)
            std::abort();
        physics_->activate();
    }

    void StaticCollider3DBinding::hide() noexcept
    {
        if (physics_)
            physics_->deactivate();
    }

    bool StaticCollider3DBinding::beginRetirement() noexcept
    {
        if (retirement_queued_ || retired_)
            return false;
        retirement_queued_ = true;
        hide();
        return true;
    }

    void StaticCollider3DBinding::retire() noexcept
    {
        while (!retireSome(std::numeric_limits<std::uint32_t>::max()))
        {}
    }

    bool StaticCollider3DBinding::retireSome(
        std::uint32_t maximum_units) noexcept
    {
        if (retired_)
            return true;
        if (maximum_units == 0u)
            return false;
        if (!retirement_queued_)
            (void)beginRetirement();
        if (physics_ && !physics_->retireSome(maximum_units))
            return false;
        physics_.reset();
        budget_ = {};
        content_ = {};
        scene_.reset();
        retired_ = true;
        return true;
    }

    bool StaticCollider3DBinding::retired() const noexcept
    {
        return retired_;
    }

    std::uint32_t StaticCollider3DBinding::remainingBodies() const noexcept
    {
        return physics_ ? physics_->remainingBodies() : 0u;
    }

    std::uint32_t
    StaticCollider3DBinding::remainingRetirementUnits() const noexcept
    {
        return physics_ ? physics_->remainingRetirementUnits() : 0u;
    }

    std::size_t StaticCollider3DBinding::accountedBytes() const noexcept
    {
        return budget_.accountedBytes();
    }
} // namespace lux::runtime::spatial3d
