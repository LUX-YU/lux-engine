#include <algorithm>
#include <lux/cxx/core/scope_exit.hpp>
#include <lux/engine/editor/ContextErrors.hpp>
#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/EditorServices.hpp>
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

    FrameworkResult<void*> EditorServices::getErased(cxx::type_info type, EditorContext& context) noexcept
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
            if (initializing_ && (found->initialize || found->initialize_startup_scene) && !found->initialized)
            {
                return cxx::unexpected(error::Error{Errors::EditorInitializationDependency});
            }
            return found->instance.get();
        }
        if (closing_)
        {
            return cxx::unexpected(error::Error{Errors::EditorServiceUseOutsideProjectLifetime});
        }
        if (found->constructing)
        {
            return cxx::unexpected(error::Error{Errors::EditorRecursiveServiceFactory, {}});
        }
        if (initializing_ && (found->initialize || found->initialize_startup_scene))
        {
            return cxx::unexpected(error::Error{Errors::EditorInitializationDependency});
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

    exec::task<FrameworkResult<void>> EditorServices::initializeProject(EditorContext& context) noexcept
    {
        const auto startup = context.manifest().startup_scene;
        const auto startup_providers = std::ranges::count_if(
            entries_,
            [](const Entry& entry) noexcept { return entry.initialize_startup_scene != nullptr; }
        );
        if (startup && startup_providers != 1)
        {
            co_return cxx::unexpected(
                error::Error{Errors::EditorStartupSceneFailure, {0, static_cast<std::uint64_t>(startup_providers), 0}}
            );
        }
        // Factories resolve their dependencies before any asynchronous initialization starts.
        for (std::size_t index = 0; index < entries_.size(); ++index)
        {
            if (entries_[index].initialize || (startup && entries_[index].initialize_startup_scene))
            {
                auto created = getErased(entries_[index].type, context);
                if (!created)
                {
                    co_return cxx::unexpected(created.error());
                }
            }
        }
        const auto order = construction_order_;
        initializing_ = true;
        cxx::scope_exit finish([this]() noexcept { initializing_ = false; });
        for (auto index : order)
        {
            auto& entry = entries_[index];
            if (entry.initialized)
            {
                continue;
            }
            if (entry.initialize)
            {
                auto prepared = co_await entry.initialize(entry.instance.get());
                if (!prepared)
                {
                    co_return cxx::unexpected(prepared.error());
                }
            }
            if (startup && entry.initialize_startup_scene)
            {
                auto opened = co_await entry.initialize_startup_scene(entry.instance.get(), *startup);
                if (!opened)
                {
                    co_return cxx::unexpected(opened.error());
                }
            }
            entry.initialized = true;
        }
        co_return FrameworkResult<void>{};
    }

    void EditorServices::requestClose() noexcept
    {
        if (closing_)
        {
            return;
        }
        closing_ = true;
        for (auto index : construction_order_)
        {
            auto& entry = entries_[index];
            entry.close_complete = !entry.close;
            if (entry.request_close)
            {
                entry.request_close(entry.instance.get());
            }
        }
    }

    bool EditorServices::closeComplete() const noexcept
    {
        return std::ranges::all_of(construction_order_, [&](auto index) { return entries_[index].close_complete; });
    }

    exec::task<FrameworkResult<void>> EditorServices::closeAsync() noexcept
    {
        requestClose();
        // Factory dependency construction precedes its consumer; borrowed services
        // finish in reverse order before their ordinary destructors execute.
        for (auto position = construction_order_.rbegin(); position != construction_order_.rend(); ++position)
        {
            auto& entry = entries_[*position];
            if (entry.close_complete)
            {
                continue;
            }
            auto result = co_await entry.close(entry.instance.get());
            if (!result)
            {
                co_return cxx::unexpected(result.error());
            }
            entry.close_complete = true;
        }
        co_return FrameworkResult<void>{};
    }

    exec::task<FrameworkResult<void>> EditorServices::recoverClose() noexcept
    {
        for (auto position = construction_order_.rbegin(); position != construction_order_.rend(); ++position)
        {
            auto& entry = entries_[*position];
            if (entry.close_complete)
            {
                continue;
            }
            if (!entry.recover)
            {
                co_return cxx::unexpected(error::Error{Errors::EditorIncompleteServiceLifecycle});
            }
            auto recovered = co_await entry.recover(entry.instance.get());
            if (!recovered)
            {
                co_return cxx::unexpected(recovered.error());
            }
            entry.close_complete = true;
        }
        co_return FrameworkResult<void>{};
    }

    bool EditorServices::dispatch(object::EventView& event) noexcept
    {
        const auto count = construction_order_.size();
        for (std::size_t i = count; i > 0; --i)
        {
            auto& entry = entries_[construction_order_[i - 1]];
            if (entry.event && entry.event(entry.instance.get(), event))
            {
                return true;
            }
        }
        return false;
    }
} // namespace lux::editor
