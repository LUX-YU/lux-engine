#include "ObjectTestSignals.hpp"

#include <thread>

namespace
{
    using lux::object::test::fixture::IntSender;

    class Receiver final : public lux::object::Object<Receiver>
    {
    public:
        void receive(const int&) noexcept
        {
        }
    };
} // namespace

int
main()
{
    lux::object::ObjectMessageQueue queue;
    IntSender sender{queue.dispatcherRef()};
    Receiver receiver;
    auto observed = sender.observe<IntSender::changed, &Receiver::receive, lux::object::EDelivery::DIRECT>(receiver);
    if (!observed)
        return 0;

    queue.close();
    auto connection = std::move(*observed);
    std::thread foreign([&] { connection.disconnect(); });
    foreign.join();
    return 0;
}
