#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include <lux/engine/object/ObjectModel.hpp>
#include <memory>

namespace
{
    class Sender final : public lux::object::Object<Sender>
    {
    public:
        static const signal_type<> closing;
        void publish() { notify<closing>(); }
    };

    const Sender::signal_type<> Sender::closing{lux::object::SignalIndex{0}};
} // namespace

int main()
{
    auto sender = std::make_unique<Sender>();
    auto connection =
        sender->observeScoped<Sender::closing>([&sender] { sender.reset(); });
    assert(connection);
    sender->publish();
    return 0;
}
