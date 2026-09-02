#pragma once

#include <lux/engine/editor/inspector/visibility.h>
#include <lux/engine/simulation/ecs/ComponentSchemaId.hpp>
#include <lux/engine/simulation/ecs/Registry.hpp>

#include <lux/cxx/compile_time/TypeToken.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace lux::editor::inspector
{
    struct InspectorContext;

    struct ComponentEditorBinding final
    {
        using DrawFn = bool (*)(
            simulation::ecs::Registry& registry,
            simulation::ecs::Entity entity,
            InspectorContext& context
        ) noexcept;

        lux::cxx::TypeToken component_type;
        simulation::ecs::ComponentSchemaId schema;
        std::string_view display_name;
        DrawFn draw{};
        std::shared_ptr<const void> code_lifetime;
    };

    enum class EComponentEditorBindingError : std::uint8_t
    {
        INVALID_BINDING,
        DUPLICATE_COMPONENT,
        COMPONENT_TYPE_COLLISION,
        DUPLICATE_SCHEMA,
        SCHEMA_COLLISION,
        ALLOCATION_FAILURE,
    };

    struct ComponentEditorBindingFailure final
    {
        EComponentEditorBindingError code{EComponentEditorBindingError::INVALID_BINDING};
        lux::cxx::TypeToken component_type;
        simulation::ecs::ComponentSchemaId schema;
    };

    class LUX_EDITOR_INSPECTOR_PUBLIC ComponentEditorBindingTable final
    {
    public:
        ComponentEditorBindingTable() noexcept = default;

        [[nodiscard]] static lux::cxx::expected<ComponentEditorBindingTable, ComponentEditorBindingFailure>
        build(std::vector<ComponentEditorBinding> bindings) noexcept;

        [[nodiscard]] const ComponentEditorBinding* find(lux::cxx::TypeToken component) const noexcept;
        [[nodiscard]] std::span<const ComponentEditorBinding> all() const noexcept;
        [[nodiscard]] bool empty() const noexcept;

    private:
        explicit ComponentEditorBindingTable(std::vector<ComponentEditorBinding> bindings) noexcept;

        std::vector<ComponentEditorBinding> bindings_;
    };
} // namespace lux::editor::inspector
