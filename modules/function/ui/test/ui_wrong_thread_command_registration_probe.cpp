#include <lux/engine/ui/UI.hpp>

#include <thread>
#include <utility>

namespace
{
    class Receiver final : public lux::object::Object<Receiver>
    {
    public:
        void invoke() noexcept
        {
        }
    };
} // namespace

int
main()
{
    lux::ui::CommandRouter router;
    Receiver receiver;
    auto command = router.defineCommand({lux::ui::UiCommandId{"probe.command"}, "Probe"});
    if (!command)
        return 0;
    auto registered = router.bindGlobal<&Receiver::invoke>(*command, receiver);
    if (!registered)
        return 0;

    std::thread foreign([registration = std::move(*registered)]() mutable { registration.reset(); });
    foreign.join();
    return 0;
}
