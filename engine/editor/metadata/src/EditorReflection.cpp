#include <lux/engine/editor/metadata/EditorReflection.hpp>
#include <lux/engine/meta/Meta.hpp>

namespace lux::editor
{
std::shared_ptr<const void> acquireEditorReflection()
{
    struct Lifetime final
    {
        bool owns{!meta::ReflectionRegistry::initialized()};
        Lifetime() { if (owns) meta::ReflectionRegistry::initRegistry(); }
        ~Lifetime() { if (owns) meta::ReflectionRegistry::destroyRegistry(); }
    };
    static std::weak_ptr<const void> active;
    auto result = active.lock();
    if (!result)
    {
        result = std::make_shared<Lifetime>();
        active = result;
    }
    return result;
}
}
