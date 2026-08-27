#pragma once

#include <lux/engine/core/semantic/SemanticType.hpp>
#include <lux/engine/meta/MetaAnnotations.hpp>

#include <cstdint>

namespace installed_consumer
{
    struct LUX_TYPE_INFO(runtime) CollisionEvent final
    {
        std::int32_t body{};
        float impulse{};
    };

    inline float observed_value{};
    inline CollisionEvent observed_event{};

    class LUX_TYPE_INFO(runtime) ConsumerBehavior final
    {
      public:
        LUX_METHOD()
        void onValue(float value) noexcept
        {
            observed_value += value;
        }

        LUX_METHOD()
        void onEvent(const CollisionEvent& value) noexcept
        {
            observed_event = value;
        }

        void unmarkedHelper() noexcept
        {
        }
    };
}

namespace lux::semantic
{
    template <>
    struct TypeTraits<installed_consumer::CollisionEvent> final
    {
        inline static constexpr std::string_view CanonicalName =
            "consumer.CollisionEvent";
        inline static constexpr std::uint8_t AbiKind =
            static_cast<std::uint8_t>(EAbiKind::STRUCT_REF);
        inline static constexpr std::uint32_t Size =
            sizeof(installed_consumer::CollisionEvent);
        inline static constexpr std::uint32_t Alignment =
            alignof(installed_consumer::CollisionEvent);
    };
}
