#include <lux/engine/simulation/TickGroup.hpp>
#include <lux/engine/simulation/SimulationSystem.hpp>

#include <utility>

namespace lux::simulation
{
    TickGroup::TickGroup() noexcept = default;

    TickGroup::TickGroup(Duration fixed_step) noexcept : fixed_step_(fixed_step) {}

    TickGroup::~TickGroup() noexcept
    {
        while (!systems_.empty())
        {
            systems_.pop_back();
        }
    }

    TickGroup::TickGroup(TickGroup&&) noexcept = default;

    TickGroup& TickGroup::operator=(TickGroup&& other) noexcept
    {
        if (this != &other)
        {
            while (!systems_.empty())
            {
                systems_.pop_back();
            }
            fixed_step_ = other.fixed_step_;
            systems_ = std::move(other.systems_);
        }
        return *this;
    }

    void TickGroup::add(std::unique_ptr<SimulationSystem> system) noexcept
    {
        systems_.push_back(std::move(system));
    }

    std::optional<TickGroup::Duration> TickGroup::fixedStep() const noexcept
    {
        return fixed_step_;
    }

    std::span<const std::unique_ptr<SimulationSystem>> TickGroup::systems() const noexcept
    {
        return systems_;
    }
} // namespace lux::simulation
