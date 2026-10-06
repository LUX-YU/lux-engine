#include <algorithm>
#include <lux/engine/editor/EditorUiRegistrar.hpp>

namespace lux::editor
{
    EditorUiRegistrar::~EditorUiRegistrar() = default;
    FrameworkResult<void> EditorUiRegistrar::registerFactory(std::string type, UiFactory factory) noexcept
    {
        if (frozen_)
        {
            return cxx::unexpected(error::makeError(
                {"lux.editor.ui_registration_is_frozen", "UI registration is frozen", error::ERecovery::PERMANENT}
            ));
        }
        const bool is_invalid_factory = type.empty() || type.find_first_of("\r\n") != type.npos || type.find('\0') != type.npos || !factory;
        if (is_invalid_factory)
        {
            return cxx::unexpected(
                error::makeError({"lux.editor.invalid_ui_factory", "Invalid UI factory", error::ERecovery::PERMANENT})
            );
        }
        if (std::ranges::find(entries_, type, &Entry::type) != entries_.end())
        {
            return cxx::unexpected(
                error::makeError({"lux.editor.duplicate_ui_type", "Duplicate UI type", error::ERecovery::PERMANENT})
            );
        }
        entries_.push_back({std::move(type), std::move(factory)});
        return {};
    }
    FrameworkResult<std::reference_wrapper<UiFactory>> EditorUiRegistrar::findFactory(std::string_view type) noexcept
    {
        if (!frozen_)
        {
            return cxx::unexpected(error::makeError(
                {"lux.editor.ui_registration_is_not_frozen",
                 "UI registration is not frozen",
                 error::ERecovery::PERMANENT}
            ));
        }
        const auto found = std::ranges::find(entries_, type, &Entry::type);
        if (found == entries_.end())
        {
            return cxx::unexpected(
                error::makeError({"lux.editor.unknown_ui_type", "Unknown UI type", error::ERecovery::PERMANENT})
            );
        }
        return std::ref(found->factory);
    }
} // namespace lux::editor
