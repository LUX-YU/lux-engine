#include <lux/engine/ui/UI.hpp>

#include <array>

int
main()
{
    lux::ui::CommandRouter router;
    std::array contexts{lux::ui::kGlobalContext};
    router.updateRoute(nullptr, contexts);
}
