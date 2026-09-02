#include <lux/engine/editor/inspector/ComponentEditorBinding.hpp>

#include <algorithm>
#include <new>
#include <utility>

namespace lux::editor::inspector
{
    ComponentEditorBindingTable::ComponentEditorBindingTable(std::vector<ComponentEditorBinding> bindings) noexcept
        : bindings_(std::move(bindings))
    {
    }

    lux::cxx::expected<ComponentEditorBindingTable, ComponentEditorBindingFailure>
    ComponentEditorBindingTable::build(std::vector<ComponentEditorBinding> bindings) noexcept
    {
        for (const auto& binding : bindings)
        {
            const bool invalid = binding.component_type.hash() == 0U || binding.component_type.name().empty() ||
                !binding.schema.valid() || binding.display_name.empty() || binding.draw == nullptr;
            if (invalid)
            {
                return lux::cxx::unexpected(ComponentEditorBindingFailure{
                    EComponentEditorBindingError::INVALID_BINDING,
                    binding.component_type,
                    binding.schema
                });
            }
        }
        try
        {
            std::ranges::sort(bindings, {}, [](const ComponentEditorBinding& value) {
                return value.component_type.hash();
            });
            for (std::size_t index = 1U; index < bindings.size(); ++index)
            {
                const auto& previous = bindings[index - 1U];
                const auto& current = bindings[index];
                if (previous.component_type.hash() == current.component_type.hash())
                {
                    const auto code = previous.component_type.name() == current.component_type.name() ?
                        EComponentEditorBindingError::DUPLICATE_COMPONENT :
                        EComponentEditorBindingError::COMPONENT_TYPE_COLLISION;
                    return lux::cxx::unexpected(ComponentEditorBindingFailure{
                        code,
                        current.component_type,
                        current.schema
                    });
                }
            }
            for (std::size_t first = 0U; first < bindings.size(); ++first)
            {
                for (std::size_t second = first + 1U; second < bindings.size(); ++second)
                {
                    if (bindings[first].schema.hash != bindings[second].schema.hash)
                        continue;
                    const auto code = bindings[first].schema.name == bindings[second].schema.name ?
                        EComponentEditorBindingError::DUPLICATE_SCHEMA :
                        EComponentEditorBindingError::SCHEMA_COLLISION;
                    return lux::cxx::unexpected(ComponentEditorBindingFailure{
                        code,
                        bindings[second].component_type,
                        bindings[second].schema
                    });
                }
            }
            return ComponentEditorBindingTable{std::move(bindings)};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(ComponentEditorBindingFailure{
                EComponentEditorBindingError::ALLOCATION_FAILURE,
                {},
                {}
            });
        }
    }

    const ComponentEditorBinding* ComponentEditorBindingTable::find(lux::cxx::TypeToken component) const noexcept
    {
        const auto found = std::ranges::lower_bound(bindings_, component.hash(), {}, [](const auto& value) {
            return value.component_type.hash();
        });
        if (found == bindings_.end() || found->component_type.hash() != component.hash() ||
            found->component_type.name() != component.name())
        {
            return nullptr;
        }
        return std::addressof(*found);
    }

    std::span<const ComponentEditorBinding> ComponentEditorBindingTable::all() const noexcept
    {
        return bindings_;
    }

    bool ComponentEditorBindingTable::empty() const noexcept
    {
        return bindings_.empty();
    }
} // namespace lux::editor::inspector
