#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/scripting/ScriptBackend.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>

#include <cstddef>
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
    inline std::int32_t observed_lifecycle_value{};
    inline lux::simulation::script::EScriptEndPlayReason observed_end_reason{};
    inline std::size_t constructed_objects{};
    inline std::size_t destroyed_objects{};

    class LUX_TYPE_INFO(runtime) BridgeBehavior final
    {
    public:
        BridgeBehavior() noexcept
        {
            ++constructed_objects;
        }

        ~BridgeBehavior() noexcept
        {
            ++destroyed_objects;
        }

        LUX_METHOD()
        void admitToGameplay() noexcept
        {
            lifecycle_value_ = 10;
        }

        LUX_METHOD()
        void onValue(float value) noexcept
        {
            observed_value = value;
            ++lifecycle_value_;
        }

        LUX_METHOD()
        void onRecord(const BridgeRecord& value) noexcept
        {
            observed_record = value.value;
        }

        LUX_METHOD()
        void leaveGameplay(lux::simulation::script::EScriptEndPlayReason reason) noexcept
        {
            observed_lifecycle_value = lifecycle_value_;
            observed_end_reason = reason;
        }

        LUX_METHOD()
        void throwing(float)
        {
        }

        void unmarkedHelper() noexcept
        {
        }


    private:
        std::int32_t lifecycle_value_{};
    };

    inline std::int32_t LUX_FUNC() bridgeFreeFunction(std::int32_t value) noexcept
    {
        return value + 1;
    }
}
