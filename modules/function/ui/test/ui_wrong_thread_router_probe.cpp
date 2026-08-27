#include <lux/engine/ui/UI.hpp>

#include <thread>

int
main()
{
    lux::ui::CommandRouter router;
    std::thread foreign([&] { static_cast<void>(router.findCommand(lux::ui::UiCommandIdView{"missing"})); });
    foreign.join();
    return 0;
}
