#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include <lux/engine/object/ObjectModel.hpp>
#include "ObjectTestSignals.hpp"
#include <type_traits>
#include <vector>

static_assert(!std::is_constructible_v<lux::object::SignalIndex, std::size_t>);

namespace
{
    using lux::object::test::fixture::BaseSender;
    using lux::object::test::fixture::DerivedSender;
    using lux::object::test::fixture::IntSender;

    struct Ping final
    {
        int value;
    };

    class EventReceiver final : public lux::object::Object<EventReceiver>
    {
    public:
        int value{0};

    protected:
        void event(lux::object::EventView& view) noexcept override
        {
            if (auto* ping = view.getIf<Ping>())
            {
                value = ping->value;
                view.accept();
            }
        }
    };
} // namespace

int main()
{
    static_assert(noexcept(lux::object::sendEvent(
        std::declval<lux::object::LuxObject&>(),
        std::declval<Ping&>()
    )));
    IntSender sender;
    assert(sender.objectType() == lux::cxx::typeToken<IntSender>());

    std::vector<int> order;
    lux::object::Connection second;
    auto first = sender.observeScoped<IntSender::changed>(
        [&](const int& value) noexcept
        {
            order.push_back(value * 10 + 1);
            second.disconnect();
        }
    );
    assert(first);
    auto second_result =
        sender.observeScoped<IntSender::changed>([&](const int& value) noexcept
                                                 { order.push_back(value * 10 + 2); });
    assert(second_result);
    second = second_result->connection();

    bool installed_late = false;
    lux::object::ScopedConnection late;
    auto installer = sender.observeScoped<IntSender::changed>(
        [&](const int& value) noexcept
        {
            order.push_back(value * 10 + 3);
            if (!installed_late)
            {
                installed_late = true;
                auto result = sender.observeScoped<IntSender::changed>(
                    [&](const int& nested) noexcept
                    { order.push_back(nested * 10 + 4); }
                );
                assert(result);
                late = std::move(*result);
            }
        }
    );
    assert(installer);

    sender.publish(1);
    assert((order == std::vector<int>{11, 13}));
    sender.publish(2);
    assert((order == std::vector<int>{11, 13, 21, 23, 24}));

    DerivedSender derived;
    class BaseReceiver final : public lux::object::Object<BaseReceiver>
    {
    public:
        void receive(const int& value) noexcept { observed = value; }
        int observed{0};
    } base_receiver;
    auto base_connection = derived.observe<
        BaseSender::baseChanged,
        &BaseReceiver::receive,
        lux::object::EDelivery::DIRECT>(base_receiver);
    assert(base_connection);
    derived.publishBase(19);
    assert(base_receiver.observed == 19);

    auto weak = sender.weakRef();
    assert(weak.getOnCurrent() == &sender);

    EventReceiver receiver;
    Ping ping{42};
    assert(lux::object::sendEvent(receiver, ping));
    assert(receiver.value == 42);
}
