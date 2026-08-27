#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/EntityBehavior.hpp>
#include <lux/engine/simulation/ScriptMountDescription.hpp>

#include <cstdint>

namespace installed_consumer
{
    inline lux::simulation::ecs::Entity observed_self{lux::simulation::ecs::NullEntity};
    inline float observed_value{};
    inline std::int32_t observed_event{};
    inline std::uint32_t constructs{};
    inline std::uint32_t starts{};
    inline std::uint32_t stops{};

    class LUX_TYPE_INFO(runtime) ConsumerBehavior final : public lux::simulation::EntityBehavior
    {
    public:
        LUX_METHOD()
        void construct() noexcept
        {
            observed_self = hostContext().self();
            ++constructs;
        }

        LUX_METHOD()
        void start() noexcept
        {
            ++starts;
        }

        LUX_METHOD()
        void stop(lux::simulation::EBehaviorStopReason) noexcept
        {
            ++stops;
        }

        LUX_METHOD()
        void onValue(float value) noexcept
        {
            observed_self = hostContext().self();
            observed_value += value;
        }

        LUX_METHOD()
        void onEvent(const std::int32_t& value) noexcept
        {
            observed_event = value;
        }

        void unmarkedHelper() noexcept
        {
        }
    };
}
