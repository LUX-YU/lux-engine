#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include <lux/engine/object/ObjectModel.hpp>
#include "ObjectTestSignals.hpp"
#include <memory>
#include <vector>

namespace
{
    using lux::object::test::fixture::MoveOnly;
    using lux::object::test::fixture::MultiSender;

    class Receiver final : public lux::object::Object<Receiver>
    {
    public:
        void receive(const int& input) noexcept { value = input; }
        void receiveMoveOnly(const MoveOnly& input) noexcept { value = input.value; }

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

        void receive(const int&) noexcept { owner_->reset(); }

    private:
        std::unique_ptr<SelfDestroyingReceiver>* owner_{nullptr};
    };
} // namespace

int main()
{
    MultiSender sender;
    auto sender_weak = sender.weakRef();
    assert(sender_weak.getAsOnCurrent<MultiSender>() == &sender);
    assert(sender_weak.getAsOnCurrent<OtherObject>() == nullptr);

    lux::object::Connection receiver_connection;
    {
        Receiver receiver;
        auto observed = sender.observe<
            MultiSender::changed,
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
        auto owned_sender = std::make_unique<MultiSender>();
        expired_sender = owned_sender->weakRef();
        auto observed = owned_sender->observe<
            MultiSender::changed,
            &Receiver::receive,
            lux::object::EDelivery::DIRECT>(surviving_receiver);
        assert(observed);
        sender_connection = *observed;
    }
    assert(expired_sender.expired());
    assert(!sender_connection.connected());

    Receiver move_receiver;
    auto move_connection = sender.observe<
        MultiSender::moveOnly,
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
        MultiSender::changed,
        &SelfDestroyingReceiver::receive,
        lux::object::EDelivery::DIRECT>(*self_destroying);
    assert(self_connection);
    sender.publish(23);
    assert(!self_destroying);
    assert(!self_connection->connected());

    std::vector<int> nested;
    auto nested_connection = sender.observeScoped<MultiSender::changed>(
        [&](const int& value) noexcept
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
