#include <algorithm>
#include <lux/engine/editor/EditorServiceRegistrar.hpp>

namespace lux::editor
{
    EditorServiceRegistrar::~EditorServiceRegistrar()
    {
        closing_ = true;
        for (auto position = construction_order_.rbegin(); position != construction_order_.rend(); ++position)
        {
            entries_[*position].instance.reset();
        }
    }

    FrameworkResult<void> EditorServiceRegistrar::registerErased(cxx::TypeToken type, Factory factory) noexcept
    {
        if (frozen_)
        {
            return cxx::unexpected(error::makeError(
                {"lux.editor.service_registration_is_frozen",
                 "Service registration is frozen",
                 error::ERecovery::PERMANENT}
            ));
        }
        if (std::ranges::find(entries_, type, &Entry::type) != entries_.end())
        {
            return cxx::unexpected(error::makeError(
                {"lux.editor.duplicate_service_type", "Duplicate service type", error::ERecovery::PERMANENT}
            ));
        }
        entries_.push_back(Entry{type, std::move(factory)});
        return {};
    }

    FrameworkResult<void*> EditorServiceRegistrar::getErased(cxx::TypeToken type, EditorContext& context) noexcept
    {
        if (!frozen_ || closing_)
        {
            return cxx::unexpected(error::makeError(
                {"lux.editor.service_use_outside_project_lifetime",
                 "Service use outside project lifetime",
                 error::ERecovery::PERMANENT}
            ));
        }
        const auto found = std::ranges::find(entries_, type, &Entry::type);
        if (found == entries_.end())
        {
            return cxx::unexpected(error::makeError(
                {"lux.editor.service_type_is_not_registered",
                 "Service type is not registered",
                 error::ERecovery::PERMANENT}
            ));
        }
        if (found->instance)
        {
            return found->instance.get();
        }
        if (found->constructing)
        {
            return cxx::unexpected(error::makeError(
                {"lux.editor.recursive_service_factory", "Recursive service factory", error::ERecovery::PERMANENT}
            ));
        }
        found->constructing = true;
        auto result = found->factory(context);
        found->constructing = false;
        if (!result)
        {
            return cxx::unexpected(std::move(result.error()));
        }
        found->instance = std::move(*result);
        construction_order_.push_back(static_cast<std::size_t>(found - entries_.begin()));
        return found->instance.get();
    }
} // namespace lux::editor
