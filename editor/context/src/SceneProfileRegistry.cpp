#include <algorithm>
#include <lux/engine/editor/ContextErrors.hpp>
#include <lux/engine/editor/SceneProfileRegistry.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/plugins/SceneRegistrations.hpp>

namespace lux::editor
{
    SceneCreateResult createSelectedScene(const SceneCreateInfo& info) noexcept
    {
        const auto missing = [](std::string_view stage, std::string_view name) noexcept
        {
            return scene::ScenePackageFailure{
                scene::EScenePackageError::INVALID_ARGUMENT,
                {},
                0,
                std::string{name},
                std::string{stage}
            };
        };
        const auto& selected = *info.selection;
        if (!selected.simulation || !selected.systems)
        {
            return cxx::unexpected(missing("Scene selection", "missing system description"));
        }
        for (const auto& name : selected.schemas)
        {
            const auto* schema = info.registrations.components.find(ecs::componentSchemaId(name.name));
            if (!schema)
            {
                return cxx::unexpected(missing("Scene schema", name.name));
            }
            const bool is_not_authored =
                schema->semantic_kind == ecs::EComponentSemanticKind::RUNTIME_DERIVED ||
                schema->snapshot != ecs::EComponentSnapshotPolicy::COPY;
            if (is_not_authored)
            {
                return cxx::unexpected(missing("Scene schema is not authored", name.name));
            }
        }
        // Profiles assemble author data. ProjectScenes validates the actual selected static/dynamic
        // contracts through SceneRuntime before publishing files; do not keep a second partial check here.
        return scene::createScenePackage(
            info.id,
            info.name,
            selected.schemas,
            selected.simulation,
            *selected.systems,
            selected.domains,
            info.registrations.partitions,
            info.stop
        );
    }

    SceneProfileRegistry::SceneProfileRegistry(std::vector<SceneProfileRegistration> entries) noexcept
        : entries_(std::move(entries))
    {
    }

    FrameworkResult<std::reference_wrapper<const SceneProfileRegistration>> SceneProfileRegistry::find(
        std::string_view id
    ) const noexcept
    {
        if (!object::ObjectRuntime::instance().isCurrent())
        {
            return cxx::unexpected(error::Error{Errors::EditorProjectServicesRequireOwnerThread});
        }
        const auto found = std::ranges::find(entries_, id, &SceneProfileRegistration::id);
        if (found == entries_.end())
        {
            return cxx::unexpected(error::Error{Errors::SceneProfileUnknown});
        }
        return std::cref(*found);
    }

    std::span<const SceneProfileRegistration> SceneProfileRegistry::profiles() const noexcept
    {
        // This is a borrow, not a mutable registration snapshot.
        if (!object::ObjectRuntime::instance().isCurrent())
        {
            std::terminate();
        }
        return entries_;
    }
} // namespace lux::editor
