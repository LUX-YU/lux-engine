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
            return cxx::unexpected(FrameworkFailure{EFrameworkError::FROZEN, "Service registration is frozen"});
        }
        if (std::ranges::find(entries_, type, &Entry::type) != entries_.end())
        {
            return cxx::unexpected(FrameworkFailure{EFrameworkError::DUPLICATE, "Duplicate service type"});
        }
        entries_.push_back(Entry{type, std::move(factory)});
        return {};
    }

    FrameworkResult<void*> EditorServiceRegistrar::getErased(cxx::TypeToken type, EditorContext& context) noexcept
    {
        if (!frozen_ || closing_)
        {
            return cxx::unexpected(FrameworkFailure{EFrameworkError::NOT_READY, "Service use outside project lifetime"}
            );
        }
        const auto found = std::ranges::find(entries_, type, &Entry::type);
        if (found == entries_.end())
        {
            return cxx::unexpected(FrameworkFailure{EFrameworkError::NOT_FOUND, "Service type is not registered"});
        }
        if (found->instance)
        {
            return found->instance.get();
        }
        if (found->constructing)
        {
            return cxx::unexpected(
                FrameworkFailure{EFrameworkError::RECURSIVE_CONSTRUCTION, "Recursive service factory"}
            );
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
