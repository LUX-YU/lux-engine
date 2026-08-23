#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include <lux/engine/object/ObjectModel.hpp>
#include <memory>
#include <vector>

namespace
{
    struct MoveOnly final
    {
        explicit MoveOnly(int input) : value(input) {}
        MoveOnly(const MoveOnly&) = delete;
        MoveOnly& operator=(const MoveOnly&) = delete;
        MoveOnly(MoveOnly&&) noexcept = default;
        MoveOnly& operator=(MoveOnly&&) noexcept = default;

        int value{0};
    };

    class Sender final : public lux::object::Object<Sender>
    {
    public:
        static const signal_type<int> changed;
        static const signal_type<MoveOnly> moveOnly;

        void publish(int value) { notify<changed>(value); }
        void publish(MoveOnly& value) { notify<moveOnly>(value); }
    };

    const Sender::signal_type<int> Sender::changed{lux::object::SignalIndex{0}};
    const Sender::signal_type<MoveOnly> Sender::moveOnly{lux::object::SignalIndex{1}};

    class Receiver final : public lux::object::Object<Receiver>
    {
    public:
        void receive(const int& input) { value = input; }
        void receiveMoveOnly(const MoveOnly& input) { value = input.value; }

        int value{0};
    };

    class OtherObject final : public lux::object::Object<OtherObject>
    {
    };

    class SelfDestroyingReceiver final
        : public lux::object::Object<SelfDestroyingReceiver>
    {
    public:
        explicit SelfDestroyingReceiver(std::unique_ptr<SelfDestroyingReceiver>* owner
        ) noexcept
            : owner_(owner)
        {
        }

        void receive(const int&) { owner_->reset(); }

    private:
        std::unique_ptr<SelfDestroyingReceiver>* owner_{nullptr};
    };
} // namespace

int main()
{
    Sender sender;
    auto sender_weak = sender.weakRef();
    assert(sender_weak.getAs<Sender>() == &sender);
    assert(sender_weak.getAs<OtherObject>() == nullptr);

    lux::object::Connection receiver_connection;
    {
        Receiver receiver;
        auto observed = sender.observe<
            Sender::changed,
            &Receiver::receive,
            lux::object::EDelivery::DIRECT>(receiver);
        assert(observed);
        receiver_connection = *observed;
        sender.publish(7);
        assert(receiver.value == 7);
    }
    assert(!receiver_connection.connected());
    sender.publish(8);

    lux::object::Connection sender_connection;
    Receiver surviving_receiver;
    lux::object::ObjectWeakRef expired_sender;
    {
        auto owned_sender = std::make_unique<Sender>();
        expired_sender = owned_sender->weakRef();
        auto observed = owned_sender->observe<
            Sender::changed,
            &Receiver::receive,
            lux::object::EDelivery::DIRECT>(surviving_receiver);
        assert(observed);
        sender_connection = *observed;
    }
    assert(expired_sender.expired());
    assert(!sender_connection.connected());

    Receiver move_receiver;
    auto move_connection = sender.observe<
        Sender::moveOnly,
        &Receiver::receiveMoveOnly,
        lux::object::EDelivery::DIRECT>(move_receiver);
    assert(move_connection);
    MoveOnly move_only{19};
    sender.publish(move_only);
    assert(move_receiver.value == 19);

    std::unique_ptr<SelfDestroyingReceiver> self_destroying;
    self_destroying =
        std::make_unique<SelfDestroyingReceiver>(std::addressof(self_destroying));
    auto self_connection = sender.observe<
        Sender::changed,
        &SelfDestroyingReceiver::receive,
        lux::object::EDelivery::DIRECT>(*self_destroying);
    assert(self_connection);
    sender.publish(23);
    assert(!self_destroying);
    assert(!self_connection->connected());

    std::vector<int> nested;
    auto nested_connection = sender.observeScoped<Sender::changed>(
        [&](const int& value)
        {
            nested.push_back(value);
            if (value == 1)
                sender.publish(2);
        }
    );
    assert(nested_connection);
    sender.publish(1);
    assert((nested == std::vector<int>{1, 2}));
}
