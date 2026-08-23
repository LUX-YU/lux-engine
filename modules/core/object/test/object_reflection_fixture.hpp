#pragma once

#include <utility>

#include <lux/engine/object/ObjectModel.hpp>

namespace lux::object::test
{
    inline int constructions = 0;
    inline int destructions = 0;

    class LUX_OBJECT() ReflectedObject final : public Object<ReflectedObject>
    {
    public:
        ReflectedObject() { ++constructions; }
        explicit ReflectedObject(ObjectDispatcherRef dispatcher)
            : Object(std::move(dispatcher))
        {
            ++constructions;
        }
        ~ReflectedObject() override { ++destructions; }

        static const signal_type<int> changed;
        static const signal_type<> saved;

        LUX_METHOD(command = true)
        void save() {}

        LUX_METHOD(connectable = true)
        void onChanged(const int& value) { last_value = value; }

        LUX_METHOD(connectable = true)
        void onWrongPayload(const float&) {}

        LUX_METHOD(connectable = true)
        void onSaved() { ++save_count; }

        void publish(int value) { notify<changed>(value); }
        void publishSaved() { notify<saved>(); }

        int last_value{0};
        int save_count{0};
    };

    class LUX_OBJECT() NonDefaultObject final : public Object<NonDefaultObject>
    {
    public:
        explicit NonDefaultObject(int) {}
    };

    class LUX_OBJECT() ReflectedBase : public Object<ReflectedBase>
    {
    public:
        static const signal_type<int> baseChanged;
    };

    class LUX_OBJECT() ReflectedDerived final
        : public Object<ReflectedDerived, ReflectedBase>
    {
    public:
        static const signal_type<int> derivedChanged;
    };
} // namespace lux::object::test
