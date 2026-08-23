#include <lux/engine/object/ObjectModel.hpp>

#include <cassert>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{
    class Sender final : public lux::object::Object<Sender>
    {
      public:
        inline static constexpr signal_type<int> changed{"changed"};
        void publish(int value) { emit(changed, value); }
    };

    class Receiver final : public lux::object::Object<Receiver>
    {
      public:
        void receive(const int& value) { observed = value; }
        int observed{0};
    };
}

int main()
{
    Sender sender;
    std::mutex mutex;
    std::condition_variable condition;
    Receiver* receiver_ptr = nullptr;
    lux::object::ObjectDispatcher* dispatcher_ptr = nullptr;
    bool ready = false;
    bool drain = false;

    std::thread thread([&]
    {
        lux::object::ObjectDispatcher dispatcher;
        Receiver receiver;
        receiver.setDispatcher(&dispatcher);
        {
            std::scoped_lock lock{mutex};
            receiver_ptr = &receiver;
            dispatcher_ptr = &dispatcher;
            ready = true;
        }
        condition.notify_all();
        {
            std::unique_lock lock{mutex};
            condition.wait(lock, [&] { return drain; });
        }
        assert(dispatcher.dispatchPending() == 1);
        assert(receiver.observed == 7);
    });

    {
        std::unique_lock lock{mutex};
        condition.wait(lock, [&] { return ready; });
    }
    auto connection = sender.observe(
        Sender::changed,
        *receiver_ptr,
        &Receiver::receive,
        lux::object::EDelivery::AUTO
    );
    assert(connection);
    sender.publish(7);
    {
        std::scoped_lock lock{mutex};
        drain = true;
    }
    condition.notify_all();
    thread.join();
    static_cast<void>(dispatcher_ptr);
}
