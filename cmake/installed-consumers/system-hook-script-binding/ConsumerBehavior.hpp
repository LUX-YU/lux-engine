#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>

#include <cstdint>

namespace installed_consumer
{
    inline float observed_value{};
    inline std::int32_t observed_event{};

    class LUX_TYPE_INFO(runtime) ConsumerBehavior final
    {
      public:
        LUX_METHOD()
        void onValue(float value) noexcept
        {
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
