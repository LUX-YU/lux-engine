#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/EntityBehavior.hpp>

#include <cstdint>

namespace lux::simulation::test
{
    struct LUX_TYPE_INFO(runtime) BridgeRecord final
    {
        std::int32_t value{};
    };

    inline ecs::Entity observed_self{ecs::NullEntity};
    inline float observed_value{};
    inline std::int32_t observed_record{};

    class LUX_TYPE_INFO(runtime) BridgeBehavior final : public EntityBehavior
    {
    public:
        LUX_METHOD()
        void onValue(float value) noexcept
        {
            observed_self = hostContext().self();
            observed_value = value;
        }

        LUX_METHOD()
        void onRecord(const BridgeRecord& value) noexcept
        {
            observed_record = value.value;
        }

        LUX_METHOD()
        void throwing(float)
        {
        }

        void unmarkedHelper() noexcept
        {
        }
    };

    inline std::int32_t LUX_FUNC() bridgeFreeFunction(std::int32_t value) noexcept
    {
        return value + 1;
    }
}
