#include <lux/engine/object/ObjectModel.hpp>

#include <cassert>
#include <vector>

namespace
{
    class Sender final : public lux::object::Object<Sender>
    {
      public:
        inline static constexpr signal_type<int> changed{"changed"};

        void publish(int value) { emit(changed, value); }
    };

    class BaseSender : public lux::object::Object<BaseSender>
    {
      public:
        inline static constexpr signal_type<int> base_changed{"base_changed"};
        void publishBase(int value) { emit(base_changed, value); }
    };

    class DerivedSender final
        : public lux::object::Object<DerivedSender, BaseSender>
    {
    };

    struct Ping final { int value; };

    class EventReceiver final : public lux::object::Object<EventReceiver>
    {
      public:
        int value{0};

      protected:
        void event(lux::object::EventView& view) override
        {
            if (auto* ping = view.getIf<Ping>())
            {
                value = ping->value;
                view.accept();
            }
        }
    };
}

int main()
{
    Sender sender;
    assert(sender.objectType() == lux::cxx::typeToken<Sender>());

    std::vector<int> order;
    lux::object::Connection second;
    auto first = sender.observe(Sender::changed, [&](const int& value)
    {
        order.push_back(value * 10 + 1);
        second.disconnect();
    });
    assert(first);
    auto second_result = sender.observe(Sender::changed, [&](const int& value)
    {
        order.push_back(value * 10 + 2);
    });
    assert(second_result);
    second = *second_result;

    bool installed_late = false;
    lux::object::Connection late;
    auto installer = sender.observe(Sender::changed, [&](const int& value)
    {
        order.push_back(value * 10 + 3);
        if (!installed_late)
        {
            installed_late = true;
            auto result = sender.observe(Sender::changed, [&](const int& nested)
            {
                order.push_back(nested * 10 + 4);
            });
            assert(result);
            late = *result;
        }
    });
    assert(installer);

    sender.publish(1);
    assert((order == std::vector<int>{11, 13}));
    sender.publish(2);
    assert((order == std::vector<int>{11, 13, 21, 23, 24}));

    DerivedSender derived;
    int base_value = 0;
    auto base_connection = derived.observe(
        BaseSender::base_changed,
        [&](const int& value) { base_value = value; }
    );
    assert(base_connection);
    derived.publishBase(19);
    assert(base_value == 19);

    auto weak = sender.weakRef();
    assert(weak.get() == &sender);

    EventReceiver receiver;
    Ping ping{42};
    assert(receiver.sendEvent(ping));
    assert(receiver.value == 42);
}
