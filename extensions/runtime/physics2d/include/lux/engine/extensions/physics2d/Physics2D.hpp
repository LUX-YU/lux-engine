#pragma once

#include <lux/engine/ecs/physics/FixedStepConfig.hpp>
#include <lux/engine/ecs/physics2d/Physics2DConfig.hpp>
#include <lux/engine/extensions/physics2d/visibility.h>

#include <cstdint>
#include <memory>

namespace lux::extensions::physics2d
{
    struct Physics2DContributionConfig final
    {
        lux::ecs::Physics2DConfig physics;
        lux::ecs::FixedStepConfig fixed_step;
    };

    class LUX_PHYSICS2D_EXTENSION_PUBLIC PhysicsWorldApi final
    {
    public:
        [[nodiscard]] std::uint64_t completedSteps() const noexcept;

    private:
        struct State;
        explicit PhysicsWorldApi(std::shared_ptr<State> state) noexcept;
        std::shared_ptr<State> state_;
        friend class Physics2DContributionSystem;
    };
} // namespace lux::extensions::physics2d
