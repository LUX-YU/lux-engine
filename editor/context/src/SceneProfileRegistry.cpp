#include <algorithm>
#include <lux/engine/editor/ContextErrors.hpp>
#include <lux/engine/editor/SceneProfileRegistry.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>

namespace lux::editor
{
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
