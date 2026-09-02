#include <lux/engine/ui/UI.hpp>

int
main()
{
    lux::ui::UISession session;
    auto first = session.beginFrame({{64.0F, 64.0F}, 1.0F / 60.0F});
    auto second = session.beginFrame({{64.0F, 64.0F}, 1.0F / 60.0F});
    static_cast<void>(first);
    static_cast<void>(second);
    return 0;
}
