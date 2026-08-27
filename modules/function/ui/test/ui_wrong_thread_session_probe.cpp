#include <lux/engine/ui/UI.hpp>

#include <thread>

int
main()
{
    lux::ui::UISession session;
    std::thread foreign([&] { static_cast<void>(session.focusedContexts()); });
    foreign.join();
    return 0;
}
