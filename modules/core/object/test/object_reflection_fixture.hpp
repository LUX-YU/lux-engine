#pragma once

#include <lux/engine/object/ObjectModel.hpp>

namespace lux::object::test
{
    inline int constructions = 0;
    inline int destructions = 0;

    class LUX_OBJECT() ReflectedObject final : public Object<ReflectedObject>
    {
      public:
        ReflectedObject() { ++constructions; }
        ~ReflectedObject() override { ++destructions; }

        LUX_OBJECT_SIGNAL(changed)
        inline static constexpr signal_type<int> changed{"changed"};

        LUX_METHOD(command=true)
        void save() {}

        LUX_METHOD(connectable=true)
        void onChanged(const int& value) { last_value = value; }

        LUX_METHOD(connectable=true)
        void onWrongPayload(const float&) {}

        void publish(int value) { emit(changed, value); }

        int last_value{0};
    };

    class LUX_OBJECT() NonDefaultObject final : public Object<NonDefaultObject>
    {
      public:
        explicit NonDefaultObject(int) {}
    };
}
