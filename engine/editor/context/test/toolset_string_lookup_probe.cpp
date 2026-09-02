#include <lux/engine/editor/Toolset.hpp>

int main()
{
    lux::editor::Toolset toolset;
    static_cast<void>(toolset.resolve("material.compiler"));
    return 0;
}
