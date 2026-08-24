#include <lux/engine/ui/UI.hpp>

int main()
{
    lux::ui::ToolbarItem item{lux::ui::EToolbarItemKind::SEPARATOR, {}};
    static_cast<void>(item);
}
