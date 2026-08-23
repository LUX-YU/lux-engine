#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include <lux/engine/object/ObjectModel.hpp>
#include "ObjectTestSignals.hpp"
#include <memory>

namespace
{
    using lux::object::test::fixture::VoidSender;
} // namespace

int main()
{
    auto sender = std::make_unique<VoidSender>();
    auto connection =
        sender->observeScoped<VoidSender::closing>([&sender]() noexcept
                                                   { sender.reset(); });
    assert(connection);
    sender->publish();
    return 0;
}
