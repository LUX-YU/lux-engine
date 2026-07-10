#pragma once
#include <lux/engine/meta/LuxObject.hpp>

namespace lux::ecs
{
    /// Base class for all gameplay systems.
    class ISystem
    {
    public:
        virtual ~ISystem() = default;

        /// Called once per frame by World::tick().
        virtual void update(lux::meta::EntityRegistry& registry, float dt) = 0;
    };

} // namespace lux::ecs
