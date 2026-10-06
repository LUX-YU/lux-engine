#include <algorithm>
#include <lux/engine/editor/EditorUiRegistrar.hpp>

namespace lux::editor
{
    EditorUiRegistrar::~EditorUiRegistrar() = default;
    FrameworkResult<void> EditorUiRegistrar::registerFactory(UiTypeId type, UiFactory factory) noexcept
    {
        if (frozen_)
        {
            return cxx::unexpected(FrameworkFailure{EFrameworkError::FROZEN, "UI registration is frozen"});
        }
        const bool is_invalid_factory = !type.isValid() || !factory;
        if (is_invalid_factory)
        {
            return cxx::unexpected(FrameworkFailure{EFrameworkError::INVALID_DESCRIPTION, "Invalid UI factory"});
        }
        if (std::ranges::find(entries_, type, &Entry::type) != entries_.end())
        {
            return cxx::unexpected(FrameworkFailure{EFrameworkError::DUPLICATE, "Duplicate UI type"});
        }
        entries_.push_back({std::move(type), std::move(factory)});
        return {};
    }
    FrameworkResult<std::reference_wrapper<UiFactory>> EditorUiRegistrar::findFactory(const UiTypeId& type) noexcept
    {
        if (!frozen_)
        {
            return cxx::unexpected(FrameworkFailure{EFrameworkError::NOT_READY, "UI registration is not frozen"});
        }
        const auto found = std::ranges::find(entries_, type, &Entry::type);
        if (found == entries_.end())
        {
            return cxx::unexpected(FrameworkFailure{EFrameworkError::NOT_FOUND, "Unknown UI type"});
        }
        return std::ref(found->factory);
    }
} // namespace lux::editor
