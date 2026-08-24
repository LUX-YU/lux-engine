#include <lux/engine/ui/UI.hpp>

int main()
{
    lux::ui::CommandRouter router;
    lux::ui::CommandHandle handle;
    static_cast<void>(router.command(handle));
}
