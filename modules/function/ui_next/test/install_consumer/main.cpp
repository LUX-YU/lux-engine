#include <lux/engine/ui_next/UiNext.hpp>

int main()
{
    lux::ui::UISession session;
    return session.commandRouter().activeContexts().empty() ? 1 : 0;
}
