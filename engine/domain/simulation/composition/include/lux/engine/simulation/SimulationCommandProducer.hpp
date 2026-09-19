#pragma once

#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>

namespace lux::simulation
{
    class SimulationBuilder;
    namespace detail
    {
        struct SimulationCommandSlot final
        {
            ecs::EcsCommandBuffer *commands{};
            std::size_t producer{};
            bool active{};
        };
    } // namespace detail

    // Prepared authority borrowed by a System, active only in its declared task/Hook region.
    class SimulationCommandProducer final
    {
      public:
        SimulationCommandProducer() noexcept = default;
        [[nodiscard]] lux::cxx::expected<ecs::EcsCommandWriter, ecs::EcsCommandFailure> begin(
            ecs::EEcsCommandPolicy policy = ecs::EEcsCommandPolicy::ABORT_BATCH) const noexcept
        {
            if (slot_ == nullptr || slot_->commands == nullptr || !slot_->active)
            {
                return lux::cxx::unexpected(ecs::EcsCommandFailure{ecs::EEcsCommandError::STALE_WRITER});
            }
            return slot_->commands->begin(slot_->producer, policy);
        }

      private:
        SimulationCommandProducer(ecs::EcsCommandBuffer *commands, detail::SimulationCommandSlot *slot) noexcept
            : slot_(slot)
        {
            slot_->commands = commands;
        }
        detail::SimulationCommandSlot *slot_{};
        friend class SimulationBuilder;
    };
} // namespace lux::simulation
