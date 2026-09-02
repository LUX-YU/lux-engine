#include <lux/engine/editor/EditorContext.hpp>

int main()
{
    auto& context = lux::editor::EditorContext::instance();
    static_cast<void>(context);
    return 0;
}
