#include "PluginBinding.hpp"
#include "PluginComponent.hpp"

#include <lux/engine/editor/inspector/FirstPartyComponentEditors.hpp>

#include <test/EditorHiddenComponent.component_editor.hpp>

#include <cassert>
#include <vector>

int main()
{
    namespace inspector = lux::editor::inspector;

    const auto plugin = inspector::test::pluginBinding();
    auto table = inspector::ComponentEditorBindingTable::build(std::vector{plugin});
    assert(table.has_value());
    assert(table->find(lux::cxx::typeToken<inspector::test::PluginComponent>()) != nullptr);
    assert(table->find(lux::cxx::typeToken<int>()) == nullptr);

    auto duplicate = inspector::ComponentEditorBindingTable::build(std::vector{plugin, plugin});
    assert(!duplicate.has_value());
    assert(duplicate.error().code == inspector::EComponentEditorBindingError::DUPLICATE_COMPONENT);

    auto type_collision_binding = plugin;
    type_collision_binding.component_type = lux::cxx::TypeToken{
        plugin.component_type.hash(),
        "test.synthetic-type-collision"
    };
    auto type_collision = inspector::ComponentEditorBindingTable::build(
        std::vector{plugin, type_collision_binding}
    );
    assert(!type_collision);
    assert(type_collision.error().code == inspector::EComponentEditorBindingError::COMPONENT_TYPE_COLLISION);

    auto duplicate_schema_binding = plugin;
    duplicate_schema_binding.component_type = lux::cxx::typeToken<int>();
    auto duplicate_schema = inspector::ComponentEditorBindingTable::build(
        std::vector{plugin, duplicate_schema_binding}
    );
    assert(!duplicate_schema);
    assert(duplicate_schema.error().code == inspector::EComponentEditorBindingError::DUPLICATE_SCHEMA);

    auto invalid_binding = plugin;
    invalid_binding.draw = nullptr;
    auto invalid = inspector::ComponentEditorBindingTable::build(std::vector{invalid_binding});
    assert(!invalid);
    assert(invalid.error().code == inspector::EComponentEditorBindingError::INVALID_BINDING);

    auto first_party = inspector::buildFirstPartyComponentEditorBindings();
    assert(first_party.has_value());
    assert(first_party->all().size() == 5U);
    assert(inspector::generated::hiddenComponentEditorBindings().empty());
    return 0;
}
