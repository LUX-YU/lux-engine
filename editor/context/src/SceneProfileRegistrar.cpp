#include <algorithm>
#include <lux/engine/editor/FrameworkErrors.hpp>
#include <lux/engine/editor/ProjectManifest.hpp>
#include <lux/engine/editor/SceneProfileRegistrar.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>

namespace lux::editor
{
    FrameworkResult<void> SceneProfileRegistrar::registerProfile(SceneProfileRegistration entry) noexcept
    {
        if (!object::ObjectRuntime::instance().isCurrent())
        {
            return cxx::unexpected(error::Error{Errors::EditorProjectServicesRequireOwnerThread});
        }
        if (frozen_)
        {
            return cxx::unexpected(error::Error{Errors::SceneProfileFrozen});
        }
        const bool invalid_name = entry.display_name.empty() ||
                                  entry.display_name.find_first_of("\r\n") != std::string::npos ||
                                  entry.display_name.find('\0') != std::string::npos;
        const bool invalid_entry = !isCanonicalProjectName(entry.id) || invalid_name || !entry.create;
        if (invalid_entry)
        {
            return cxx::unexpected(error::Error{Errors::SceneProfileInvalid});
        }
        for (std::size_t i{}; i < entry.capabilities.size(); ++i)
        {
            const auto& capability = entry.capabilities[i];
            const bool duplicate = std::find(entry.capabilities.begin(), entry.capabilities.begin() + i, capability) !=
                                   entry.capabilities.begin() + i;
            if (!isCanonicalProjectName(capability) || duplicate)
            {
                return cxx::unexpected(error::Error{Errors::SceneProfileInvalid});
            }
        }
        if (std::ranges::find(entries_, entry.id, &SceneProfileRegistration::id) != entries_.end())
        {
            return cxx::unexpected(error::Error{Errors::SceneProfileDuplicate});
        }
        entries_.push_back(std::move(entry));
        return {};
    }
    FrameworkResult<std::reference_wrapper<const SceneProfileRegistration>> SceneProfileRegistrar::find(
        std::string_view id
    ) const noexcept
    {
        if (!object::ObjectRuntime::instance().isCurrent())
        {
            return cxx::unexpected(error::Error{Errors::EditorProjectServicesRequireOwnerThread});
        }
        if (!frozen_)
        {
            return cxx::unexpected(error::Error{Errors::SceneProfileNotFrozen});
        }
        const auto found = std::ranges::find(entries_, id, &SceneProfileRegistration::id);
        if (found == entries_.end())
        {
            return cxx::unexpected(error::Error{Errors::SceneProfileUnknown});
        }
        return std::cref(*found);
    }
    std::span<const SceneProfileRegistration> SceneProfileRegistrar::profiles() const noexcept
    {
        // This is a borrow, not a mutable registration snapshot during assembly.
        if (!object::ObjectRuntime::instance().isCurrent() || !frozen_)
        {
            std::terminate();
        }
        return entries_;
    }
} // namespace lux::editor
