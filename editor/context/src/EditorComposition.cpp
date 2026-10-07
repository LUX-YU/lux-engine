#include <algorithm>
#include <lux/engine/editor/EditorComposition.hpp>
#include <lux/engine/editor/ProjectManifest.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>

namespace lux::editor
{
    FrameworkResult<void> EditorComposition::registerErased(
        cxx::TypeToken type,
        EditorServices::Factory factory
    ) noexcept
    {
        if (std::ranges::find(services_, type, &EditorServices::Entry::type) != services_.end())
        {
            return cxx::unexpected(error::Error{Errors::EditorDuplicateServiceType, {}});
        }
        services_.push_back(EditorServices::Entry{type, std::move(factory)});
        return {};
    }

    FrameworkResult<void> EditorComposition::registerUiFactory(std::string type, UiFactory factory) noexcept
    {
        const bool is_invalid_factory =
            type.empty() || type.find_first_of("\r\n") != type.npos || type.find('\0') != type.npos || !factory;
        if (is_invalid_factory)
        {
            return cxx::unexpected(error::Error{Errors::EditorInvalidUiFactory, {}});
        }
        if (std::ranges::find(ui_, type, &EditorUiRegistry::Factory::type_) != ui_.end())
        {
            return cxx::unexpected(error::Error{Errors::EditorDuplicateUiType, {}});
        }
        ui_.push_back(EditorUiRegistry::Factory{std::move(type), std::move(factory)});
        return {};
    }
    FrameworkResult<void> EditorComposition::registerSceneProfile(SceneProfileRegistration entry) noexcept
    {
        if (!object::ObjectRuntime::instance().isCurrent())
        {
            return cxx::unexpected(error::Error{Errors::EditorProjectServicesRequireOwnerThread});
        }
        const bool invalid_name = entry.display_name.empty() ||
                                  entry.display_name.find_first_of("\r\n") != std::string::npos ||
                                  entry.display_name.find('\0') != std::string::npos;
        const bool invalid_entry = !isCanonicalSceneName(entry.id) || invalid_name || !entry.create;
        if (invalid_entry)
        {
            return cxx::unexpected(error::Error{Errors::SceneProfileInvalid});
        }
        for (std::size_t i{}; i < entry.capabilities.size(); ++i)
        {
            const auto& capability = entry.capabilities[i];
            const bool duplicate = std::find(entry.capabilities.begin(), entry.capabilities.begin() + i, capability) !=
                                   entry.capabilities.begin() + i;
            if (!isCanonicalSceneName(capability) || duplicate)
            {
                return cxx::unexpected(error::Error{Errors::SceneProfileInvalid});
            }
        }
        if (std::ranges::find(scene_profiles_, entry.id, &SceneProfileRegistration::id) != scene_profiles_.end())
        {
            return cxx::unexpected(error::Error{Errors::SceneProfileDuplicate});
        }
        scene_profiles_.push_back(std::move(entry));
        return {};
    }
} // namespace lux::editor
