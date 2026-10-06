#include <algorithm>
#include <lux/engine/editor/EditorUiRegistrar.hpp>
#include <lux/engine/editor/FrameworkErrors.hpp>

namespace lux::editor
{
    EditorUiRegistrar::~EditorUiRegistrar() = default;
    FrameworkResult<void> EditorUiRegistrar::registerFactory(std::string type, UiFactory factory) noexcept
    {
        if (frozen_)
        {
            return cxx::unexpected(error::Error{Errors::EditorUiRegistrationIsFrozen, {}});
        }
        const bool is_invalid_factory =
            type.empty() || type.find_first_of("\r\n") != type.npos || type.find('\0') != type.npos || !factory;
        if (is_invalid_factory)
        {
            return cxx::unexpected(error::Error{Errors::EditorInvalidUiFactory, {}});
        }
        if (std::ranges::find(entries_, type, &Entry::type) != entries_.end())
        {
            return cxx::unexpected(error::Error{Errors::EditorDuplicateUiType, {}});
        }
        entries_.push_back({std::move(type), std::move(factory)});
        return {};
    }
    FrameworkResult<EditorUiRegistrar::FactoryRef> EditorUiRegistrar::resolveFactory(std::string_view type) noexcept
    {
        if (!frozen_)
        {
            return cxx::unexpected(error::Error{Errors::EditorUiRegistrationIsNotFrozen, {}});
        }
        const auto found = std::ranges::find(entries_, type, &Entry::type);
        if (found == entries_.end())
        {
            return cxx::unexpected(error::Error{Errors::EditorUnknownUiType, {}});
        }
        return FactoryRef{found->factory};
    }
} // namespace lux::editor
