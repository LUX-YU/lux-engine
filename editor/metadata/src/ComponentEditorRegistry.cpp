#include <lux/engine/editor/metadata/ComponentEditorRegistry.hpp>

#include <algorithm>

namespace lux::editor
{
    struct ComponentEditorRegistry::Impl final
    {
        simulation::ecs::ComponentSchemaSet schemas;
        std::vector<ComponentEditorRegistration> registrations;
    };

    ComponentEditorRegistry::ComponentEditorRegistry(std::shared_ptr<const Impl> impl) noexcept : impl_(std::move(impl))
    {}

    EditorResult<ComponentEditorRegistry> ComponentEditorRegistry::create(
        simulation::ecs::ComponentSchemaSet schemas,
        std::vector<ComponentEditorRegistration> registrations
    ) noexcept
    {
        std::ranges::sort(registrations, {}, [](const auto& entry) { return entry.type.hash(); });
        std::uint64_t previous{};
        bool has_previous{};
        for (const auto& entry : registrations)
        {
            const auto* schema = schemas.find(entry.type);
            const bool invalid_schema =
                !schema || schema->semantic_kind == simulation::ecs::EComponentSemanticKind::RUNTIME_DERIVED;
            const bool invalid_editor =
                entry.name.empty() || !entry.create || entry.provider.id.empty() || entry.provider.version == 0;
            const bool duplicate = has_previous && previous == entry.type.hash();
            if (invalid_schema || invalid_editor || duplicate)
                return lux::cxx::unexpected(EditorFailure{
                    EEditorError::INVALID_ARGUMENT,
                    "component.editor.registration",
                    entry.type.hash(),
                    entry.name
                });
            previous = entry.type.hash();
            has_previous = true;
        }
        return ComponentEditorRegistry(std::make_shared<const Impl>(Impl{std::move(schemas), std::move(registrations)})
        );
    }

    const ComponentEditorRegistration* ComponentEditorRegistry::find(lux::cxx::TypeToken type) const noexcept
    {
        const auto entries = all();
        const auto found =
            std::ranges::lower_bound(entries, type.hash(), {}, [](const auto& entry) { return entry.type.hash(); });
        return found != entries.end() && found->type == type ? std::addressof(*found) : nullptr;
    }

    std::span<const ComponentEditorRegistration> ComponentEditorRegistry::all() const noexcept
    {
        return impl_ ? std::span<const ComponentEditorRegistration>{impl_->registrations}
                     : std::span<const ComponentEditorRegistration>{};
    }
}
