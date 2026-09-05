#pragma once

#include <lux/engine/simulation/ecs/EcsCommandBuffer.hpp>

namespace lux::simulation
{
    class SimulationBuilder;
    namespace detail
    {
        struct SimulationCommandSlot final
        {
            std::size_t producer{};
            bool active{};
        };
    }

    // Prepared authority borrowed by a System, active only in its declared task/Hook region.
    class SimulationCommandProducer final
    {
    public:
        SimulationCommandProducer() noexcept = default;
        [[nodiscard]] lux::cxx::expected<ecs::EcsCommandWriter, ecs::EcsCommandFailure> begin() const noexcept
        {
            if (commands_ == nullptr || slot_ == nullptr || !slot_->active)
                return lux::cxx::unexpected(ecs::EcsCommandFailure{ecs::EEcsCommandError::STALE_WRITER});
            return commands_->begin(slot_->producer);
        }
    private:
        SimulationCommandProducer(ecs::EcsCommandBuffer* commands, detail::SimulationCommandSlot* slot) noexcept
            : commands_(commands), slot_(slot)
        {}
        ecs::EcsCommandBuffer* commands_{};
        detail::SimulationCommandSlot* slot_{};
        friend class SimulationBuilder;
    };
}
