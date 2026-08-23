#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include <condition_variable>
#include <lux/engine/object/ObjectModel.hpp>
#include "ObjectTestSignals.hpp"
#include <memory>
#include <mutex>
#include <thread>

namespace
{
    using lux::object::test::fixture::IntSender;

    class Receiver final : public lux::object::Object<Receiver>
    {
    public:
        explicit Receiver(lux::object::ObjectDispatcherRef dispatcher)
            : Object(std::move(dispatcher))
        {
        }

        void receive(const int& value) noexcept { observed = value; }
        int observed{0};
    };

    struct Ping final
    {
        int value{0};
    };

    class EventReceiver final : public lux::object::Object<EventReceiver>
    {
    public:
        explicit EventReceiver(lux::object::ObjectDispatcherRef dispatcher)
            : Object(std::move(dispatcher))
        {
        }

        int observed{0};

    protected:
        void event(lux::object::EventView& view) override
        {
            if (auto* ping = view.getIf<Ping>())
            {
                observed = ping->value;
                view.accept();
            }
        }
    };

    void runQueuedScenario(bool disconnect, bool destroy_receiver)
    {
        IntSender sender;
        std::mutex mutex;
        std::condition_variable condition;
        Receiver* receiver_pointer = nullptr;
        bool ready = false;
        bool drain = false;
        int observed = -1;
        std::size_t dispatched = 0;

        std::thread receiver_thread(
            [&]
            {
                lux::object::ObjectMessageQueue queue;
                auto receiver = std::make_unique<Receiver>(queue.dispatcherRef());
                {
                    std::scoped_lock lock{mutex};
                    receiver_pointer = receiver.get();
                    ready = true;
                }
                condition.notify_all();
                {
                    std::unique_lock lock{mutex};
                    condition.wait(lock, [&] { return drain; });
                }
                if (destroy_receiver)
                    receiver.reset();
                dispatched = queue.dispatchPending();
                observed = receiver ? receiver->observed : 0;
            }
        );

        {
            std::unique_lock lock{mutex};
            condition.wait(lock, [&] { return ready; });
        }
        auto connection = sender.observe<
            IntSender::changed,
            &Receiver::receive,
            lux::object::EDelivery::AUTO>(*receiver_pointer);
        assert(connection);
        sender.publish(7);
        if (disconnect)
        {
            std::thread disconnect_thread([connection_copy = *connection]() mutable
                                          { connection_copy.disconnect(); });
            disconnect_thread.join();
            assert(!connection->connected());
        }
        {
            std::scoped_lock lock{mutex};
            drain = true;
        }
        condition.notify_all();
        receiver_thread.join();

        assert(dispatched == 1);
        if (disconnect || destroy_receiver)
        {
            assert(observed == 0);
            assert(!connection->connected());
        }
        else
        {
            assert(observed == 7);
        }
    }
} // namespace

int main()
{
    runQueuedScenario(false, false);
    runQueuedScenario(true, false);
    runQueuedScenario(false, true);

    lux::object::ObjectDispatcherRef closed_dispatcher;
    {
        lux::object::ObjectMessageQueue queue;
        closed_dispatcher = queue.dispatcherRef();
        EventReceiver receiver{queue.dispatcherRef()};
        assert(
            lux::object::postEvent(receiver.weakRef(), Ping{31}) ==
            lux::object::EEventPostStatus::POSTED
        );
        assert(queue.dispatchPending() == 1);
        assert(receiver.observed == 31);
    }
    bool invoked = false;
    auto message = lux::object::detail::makeObjectMessage([&] { invoked = true; });
    assert(
        closed_dispatcher.post(std::move(message)) == lux::object::EPostStatus::CLOSED
    );
    assert(!invoked);
}
