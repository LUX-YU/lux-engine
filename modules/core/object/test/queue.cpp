#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/object/ObjectEvent.hpp>
#include <array>
#include <cassert>
#include <atomic>
#include <memory>
#include <thread>
#include <cstdio>
#include <semaphore>

using namespace lux::object;
namespace
{
    class Sender : public lux::object::LuxObject
    {
    public:
        using LuxObject::LuxObject;
        TSignal<int> changed{*this};
        TSignal<> activated{*this};
        TSignal<std::unique_ptr<int>> owned{*this};
        using LuxObject::emit;
    };
    class Receiver final : public lux::object::LuxObject
    {
    public:
        using LuxObject::LuxObject;
        int received{};
        void accept(const int& value) noexcept
        {
            received += value;
        }
    };
    ObjectMessageQueue queue(std::size_t capacity)
    {
        auto made = ObjectMessageQueue::create(capacity);
        assert(made);
        return std::move(*made);
    }
    void fixedBatch()
    {
        auto messages = queue(3);
        const auto dispatcher = messages.dispatcherRef();
        std::array<int, 8> values{};
        std::size_t count{};
        for (int value = 1; value <= 3; ++value)
            assert(detail::post(dispatcher, detail::makeMessage([&, value]() noexcept {
                                    values[count++] = value;
                                    if (value == 1)
                                        assert(detail::post(dispatcher, detail::makeMessage([&]() noexcept {
                                                                values[count++] = 4;
                                                            })) == detail::EPostStatus::POSTED);
                                })) == detail::EPostStatus::POSTED);
        auto retained = detail::makeMessage([&]() noexcept { values[count++] = 5; });
        assert(detail::post(dispatcher, std::move(retained)) == detail::EPostStatus::FULL && retained);
        assert(messages.dispatchPending(0) == 0);
        assert(messages.dispatchPending(2) == 2 && count == 2 && values[0] == 1 && values[1] == 2);
        assert(messages.dispatchPending() == 1 && values[2] == 3); // Finish the old batch first.
        assert(detail::post(dispatcher, std::move(retained)) == detail::EPostStatus::POSTED && !retained);
        assert(messages.dispatchPending() == 2 && values[3] == 4 && values[4] == 5);
        const auto stats = messages.statistics();
        assert(
            stats.pending == 0 && stats.high_water == 3 && stats.posted == 5 && stats.inline_posted == 5 &&
            stats.full == 1
        );
        messages.close();
        auto closed = detail::makeMessage([]() noexcept {});
        assert(detail::post(dispatcher, std::move(closed)) == detail::EPostStatus::CLOSED && closed);
    }
    void closeDuringDelivery()
    {
        auto messages = queue(4);
        int calls{};
        auto dispatcher = messages.dispatcherRef();
        assert(detail::post(dispatcher, detail::makeMessage([&]() noexcept {
                                ++calls;
                                messages.close();
                            })) == detail::EPostStatus::POSTED);
        assert(detail::post(dispatcher, detail::makeMessage([&]() noexcept {
                                ++calls;
                            })) == detail::EPostStatus::POSTED);
        assert(messages.dispatchPending() == 1 && calls == 1 && messages.statistics().pending == 0);
    }
    void partialBroadcast()
    {
        auto sending = queue(2), receiving = queue(1), other = queue(2);
        Sender sender(sending.dispatcherRef());
        Receiver first(receiving.dispatcherRef()), second(other.dispatcherRef());
        auto a = lux::object::LuxObject::connect(
            std::addressof(sender),
            &Sender::changed,
            std::addressof(first),
            &Receiver::accept,
            EDelivery::QUEUED
        );
        auto b = lux::object::LuxObject::connect(
            std::addressof(sender),
            &Sender::changed,
            std::addressof(second),
            &Receiver::accept,
            EDelivery::QUEUED
        );
        assert(a && b);
        auto initial = sender.emit(sender.changed, 1);
        assert(initial.complete() && initial.queued == 2);
        const auto partial = sender.emit(sender.changed, 2);
        assert(!partial.complete() && partial.full == 1 && partial.queued == 1);
        assert(receiving.dispatchPending() == 1 && other.dispatchPending() == 2);
        assert(first.received == 1 && second.received == 3); // No automatic rebroadcast duplicates.
        receiving.close();
        const auto closed = sender.emit(sender.changed, 4);
        assert(closed.closed == 1 && closed.queued == 1 && !a->connected());
        assert(other.dispatchPending() == 1 && second.received == 7);
        auto handle = std::move(*b);
        std::thread foreign([&] { handle.disconnect(); });
        foreign.join();
        assert(!handle.connected());
        assert(sender.emit(sender.changed, 8).complete()); // Reclaims a logical foreign disconnect.
        assert(other.dispatchPending() == 0 && second.received == 7);
    }
    void concurrentProducers()
    {
        auto messages = queue(32);
        const auto dispatcher = messages.dispatcherRef();
        std::atomic<unsigned> remaining{4};
        std::array<unsigned, 4> expected{};
        std::array<std::thread, 4> producers;
        for (unsigned producer{}; producer < 4; ++producer)
            producers[producer] = std::thread([&, producer] {
                for (unsigned value{}; value < 300; ++value)
                {
                    auto message =
                        detail::makeMessage([&, producer, value]() noexcept { assert(expected[producer]++ == value); });
                    while (detail::post(dispatcher, std::move(message)) == detail::EPostStatus::FULL)
                    {
                        assert(message);
                        std::this_thread::yield();
                    }
                }
                --remaining;
            });
        while (remaining.load() || messages.statistics().pending)
        {
            static_cast<void>(messages.dispatchPending(7));
            std::this_thread::yield();
        }
        for (auto& producer : producers)
            producer.join();
        for (auto count : expected)
            assert(count == 300);
        const auto stats = messages.statistics();
        assert(stats.posted == 1200 && stats.inline_posted == 1200 && stats.high_water <= 64);
        std::printf(
            "Queue MPSC: peak=%zu posted=%zu inline=%zu full=%zu\n",
            stats.high_water,
            stats.posted,
            stats.inline_posted,
            stats.full
        );
    }

    void connectionLifetime()
    {
        auto messages = queue(8);
        struct Derived final : Sender
        {
            using Sender::Sender;
        };
        Derived sender(messages.dispatcherRef());
        Receiver receiver(messages.dispatcherRef());
        {
            auto connection = LuxObject::connect(&sender, &Sender::changed, &receiver, &Receiver::accept);
            assert(connection && sender.emit(sender.changed, 3).direct == 1 && receiver.received == 3);
            auto moved = std::move(*connection);
            assert(!connection->connected() && moved.connected());
        }
        assert(sender.emit(sender.changed, 2).direct == 0 && receiver.received == 3);
        assert(!LuxObject::connect(&sender, &Sender::changed, static_cast<Receiver*>(nullptr), &Receiver::accept));
        auto noncopy = LuxObject::connect(&sender, &Sender::owned, [](const std::unique_ptr<int>& value) noexcept {
            assert(*value == 7);
        });
        assert(noncopy && sender.emit(sender.owned, std::make_unique<int>(7)).direct == 1);
        auto rejected = LuxObject::connect(
            &sender,
            &Sender::owned,
            &receiver,
            [](const std::unique_ptr<int>&) noexcept {},
            EDelivery::QUEUED
        );
        assert(!rejected && rejected.error() == EConnectError::PAYLOAD_NOT_QUEUEABLE);
        Connection retired;
        {
            auto temporary = std::make_unique<Receiver>(messages.dispatcherRef());
            auto made =
                LuxObject::connect(&sender, &Sender::changed, temporary.get(), &Receiver::accept, EDelivery::QUEUED);
            assert(made);
            retired = std::move(*made);
            assert(sender.emit(sender.changed, 9).queued == 1);
        }
        assert(!retired.connected());
        // Reuse the storage while the old queued message and handle still exist.
        auto fresh = LuxObject::connect(&sender, &Sender::changed, &receiver, &Receiver::accept, EDelivery::QUEUED);
        assert(fresh && sender.emit(sender.changed, 5).queued == 1);
        retired.disconnect();
        assert(messages.dispatchPending() == 2 && receiver.received == 8);
        Connection sender_first;
        {
            Sender temporary(messages.dispatcherRef());
            auto made =
                LuxObject::connect(&temporary, &Sender::changed, &receiver, &Receiver::accept, EDelivery::QUEUED);
            assert(made);
            sender_first = std::move(*made);
            assert(temporary.emit(temporary.changed, 100).queued == 1);
        }
        assert(!sender_first.connected() && messages.dispatchPending() == 1 && receiver.received == 8);
    }

    void changesDuringEmit()
    {
        auto messages = queue(8);
        Sender sender(messages.dispatcherRef());
        Connection first, second, added;
        unsigned outer{}, later{};
        bool nested{};
        auto made = LuxObject::connect(&sender, &Sender::activated, [&]() noexcept {
            ++outer;
            second.disconnect();
            if (!nested)
            {
                nested = true;
                auto pending = LuxObject::connect(&sender, &Sender::activated, [&]() noexcept { ++later; });
                assert(pending);
                added = std::move(*pending);
                assert(sender.emit(sender.activated).direct == 1);
            }
        });
        assert(made);
        first = std::move(*made);
        auto suppressed = LuxObject::connect(&sender, &Sender::activated, []() noexcept { assert(false); });
        assert(suppressed);
        second = std::move(*suppressed);
        assert(sender.emit(sender.activated).direct == 1 && outer == 2 && later == 0);
        assert(sender.emit(sender.activated).direct == 2 && outer == 3 && later == 1);
        first.disconnect();
        added.disconnect();
        auto resource = std::make_shared<int>(1);
        std::weak_ptr<int> observed = resource;
        auto self = LuxObject::connect(&sender, &Sender::activated, [resource, &first, &observed]() noexcept {
            first.disconnect();
            assert(!observed.expired()); // Callable survives its own disconnect until invoke returns.
        });
        assert(self);
        first = std::move(*self);
        resource.reset();
        assert(sender.emit(sender.activated).direct == 1 && observed.expired());
    }

    void crossThreadEndpoints()
    {
        auto sending = queue(1);
        Sender sender(sending.dispatcherRef());
        std::binary_semaphore ready{0}, deliver{0}, finished{0}, retire{0};
        Receiver* endpoint{};
        unsigned received{};
        std::thread worker([&] {
            auto receiving = queue(8);
            Receiver receiver(receiving.dispatcherRef());
            endpoint = &receiver;
            ready.release();
            deliver.acquire();
            assert(receiving.dispatchPending() == 1);
            received = receiver.received;
            finished.release();
            retire.acquire();
        });
        ready.acquire();
        auto direct = LuxObject::connect(&sender, &Sender::changed, endpoint, &Receiver::accept, EDelivery::DIRECT);
        assert(!direct && direct.error() == EConnectError::DIRECT_CROSS_AFFINITY);
        auto connected = LuxObject::connect(&sender, &Sender::changed, endpoint, &Receiver::accept);
        assert(connected && sender.emit(sender.changed, 11).queued == 1);
        deliver.release();
        finished.acquire();
        assert(received == 11);
        assert(detail::post(sending.dispatcherRef(), detail::makeMessage([]() noexcept {
                            })) == detail::EPostStatus::POSTED);
        // Foreign endpoint cancellation cannot be lost because the sender's business queue is full.
        retire.release();
        worker.join();
        assert(!connected->connected());
        assert(sending.dispatchPending() == 1 && sender.emit(sender.changed, 1).queued == 0);
    }

}
int main()
{
    assert(!ObjectMessageQueue::create(0));
    fixedBatch();
    closeDuringDelivery();
    partialBroadcast();
    concurrentProducers();
    connectionLifetime();
    changesDuringEmit();
    crossThreadEndpoints();
}
