#include <lux/engine/ui/UI.hpp>

int main()
{
    lux::ui::MenuItem item{lux::ui::EMenuItemKind::SEPARATOR, "invalid", {}, {}};
    static_cast<void>(item);
}
