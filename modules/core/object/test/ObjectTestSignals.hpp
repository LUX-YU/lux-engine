#pragma once

#include <lux/engine/object/ObjectModel.hpp>

namespace lux::object::test::fixture
{
    struct MoveOnly final
    {
        explicit MoveOnly(int input) noexcept : value(input) {}
        MoveOnly(const MoveOnly&) = delete;
        MoveOnly& operator=(const MoveOnly&) = delete;
        MoveOnly(MoveOnly&&) noexcept = default;
        MoveOnly& operator=(MoveOnly&&) noexcept = default;

        int value{0};
    };

    class LUX_OBJECT() IntSender final : public Object<IntSender>
    {
    public:
        static const signal_type<int> changed;

        void publish(int value) { notify<changed>(value); }
    };

    class LUX_OBJECT() IntReceiver final : public Object<IntReceiver>
    {
    public:
        LUX_METHOD(connectable = true)
        void receive(const int& value) noexcept { observed += value; }

        std::uint64_t observed{0};
    };

    class LUX_OBJECT() BaseSender : public Object<BaseSender>
    {
    public:
        static const signal_type<int> baseChanged;

        void publishBase(int value) { notify<baseChanged>(value); }
    };

    class LUX_OBJECT() DerivedSender final
        : public Object<DerivedSender, BaseSender>
    {
    };

    class LUX_OBJECT() MultiSender final : public Object<MultiSender>
    {
    public:
        static const signal_type<int> changed;
        static const signal_type<MoveOnly> moveOnly;

        void publish(int value) { notify<changed>(value); }
        void publish(MoveOnly& value) { notify<moveOnly>(value); }
    };

    class LUX_OBJECT() VoidSender final : public Object<VoidSender>
    {
    public:
        static const signal_type<> closing;

        void publish() { notify<closing>(); }
    };
} // namespace lux::object::test::fixture
