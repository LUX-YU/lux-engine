#include "emptyModules.modules.hpp"
#include <cassert>

int main()
{
    const auto selected = module_fixture::emptyModules();
    assert(selected.empty());
    auto modules = lux::editor::extensions::loadStaticEditorModules(selected);
    assert(modules && modules->empty());
}
