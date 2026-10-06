#include <algorithm>
#include <lux/engine/editor/EditorServiceRegistrar.hpp>
#include <lux/engine/editor/FrameworkErrors.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>

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
            return cxx::unexpected(error::Error{Errors::EditorServiceRegistrationIsFrozen, {}});
        }
        if (std::ranges::find(entries_, type, &Entry::type) != entries_.end())
        {
            return cxx::unexpected(error::Error{Errors::EditorDuplicateServiceType, {}});
        }
        entries_.push_back(Entry{type, std::move(factory)});
        return {};
    }

    FrameworkResult<void*> EditorServiceRegistrar::getErased(cxx::TypeToken type, EditorContext& context) noexcept
    {
        if (!object::ObjectRuntime::instance().isCurrent())
        {
            return cxx::unexpected(error::Error{Errors::EditorProjectServicesRequireOwnerThread});
        }
        if (!frozen_ || closing_)
        {
            return cxx::unexpected(error::Error{Errors::EditorServiceUseOutsideProjectLifetime, {}});
        }
        const auto found = std::ranges::find(entries_, type, &Entry::type);
        if (found == entries_.end())
        {
            return cxx::unexpected(error::Error{Errors::EditorServiceTypeIsNotRegistered, {}});
        }
        if (found->instance)
        {
            return found->instance.get();
        }
        if (found->constructing)
        {
            return cxx::unexpected(error::Error{Errors::EditorRecursiveServiceFactory, {}});
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
