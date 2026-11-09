#include <lux/engine/editor/BuiltinValueTypes.value_editor.hpp>
#include <lux/engine/editor/ComponentEditors3D.hpp>
#include <lux/engine/editor/EditorComposition.hpp>

namespace lux::editor
{
    namespace
    {
        template <class T> ComponentEditor componentEditor() noexcept
        {
            return {
                {},
                cxx::make_basic_type_info<T>(),
                +[](void* draft) noexcept { editValue(ValueEditOptions{}, *static_cast<T*>(draft)); }
            };
        }
    } // namespace

    FrameworkResult<void> registerComponentEditors3D(EditorComposition& composition) noexcept
    {
        const ComponentEditor editors[]{
            componentEditor<ecs::Transform3D>(),
            componentEditor<scene::ViewComponent>(),
            componentEditor<ecs::Mesh3D>(),
            componentEditor<ecs::MaterialComponent>(),
            componentEditor<ecs::Light3D>()
        };
        for (const auto& editor : editors)
        {
            if (auto added = composition.registerComponentEditor(editor); !added)
            {
                return added;
            }
        }
        return {};
    }
} // namespace lux::editor
