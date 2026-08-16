#pragma once

#include <lux/engine/ecs/physics3d/systems/Physics3DScene.hpp>
#include <lux/engine/ecs/systems/ISystem.hpp>
#include <lux/engine/function/visibility.h>

#include <memory>

namespace lux::ecs
{
    class LUX_FUNCTION_PUBLIC Physics3DSystem final : public ISystem
    {
    public:
        explicit Physics3DSystem(
            std::shared_ptr<Physics3DScene> scene) noexcept
            : scene_(std::move(scene))
        {}

        void update(const SystemUpdateContext& context) override;

    private:
        std::shared_ptr<Physics3DScene> scene_;
    };
} // namespace lux::ecs
