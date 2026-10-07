#include <algorithm>
#include <lux/engine/editor/EditorServices.hpp>
#include <lux/engine/editor/FrameworkErrors.hpp>
#include <lux/engine/object/ObjectRuntime.hpp>

namespace lux::editor
{
    EditorServices::EditorServices(std::vector<Entry> entries) noexcept : entries_(std::move(entries))
    {
        construction_order_.reserve(entries_.size());
    }
    EditorServices::~EditorServices() noexcept
    {
        // Remove lookup access before any user destructor runs, without another closing state.
        auto entries = std::move(entries_);
        entries_.clear();
        for (auto index = construction_order_.rbegin(); index != construction_order_.rend(); ++index)
        {
            entries[*index].instance.reset();
        }
    }
    FrameworkResult<void*> EditorServices::getErased(cxx::TypeToken type, EditorContext& context) noexcept
    {
        if (!object::ObjectRuntime::instance().isCurrent())
        {
            return cxx::unexpected(error::Error{Errors::EditorProjectServicesRequireOwnerThread});
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
