#include <lux/engine/editor/scene/RunInspectorFields.hpp>

namespace lux::editor::scene
{
    namespace
    {
        bool ended(const RunFailure& failure) noexcept
        {
            const auto* error = std::get_if<ERunError>(&failure.cause);
            return error && (*error == ERunError::INVALID_ID || *error == ERunError::STOPPED);
        }
    }
    RunInspectorFields::RunInspectorFields(
        RunStore& runs,
        Target target,
        simulation::ecs::ComponentSchema schema,
        Copy copy,
        project::ProjectCatalogModel* catalog
    )
        : runs_(runs), target_(target), schema_(std::move(schema)), copy_(copy), catalog_(catalog)
    {}
    RunInspectorFields::~RunInspectorFields() noexcept
    {
        if (!release())
            std::terminate();
    }
    RunInspectorFields::Status RunInspectorFields::withRead(cxx::function_ref<Status()> action)
    {
        const auto inspect = [&](const simulation::ecs::Registry&,
                                 const std::optional<editing::HistorySnapshot>&) -> Status {
            // Generated component copy/destruction is a foreign code containment boundary.
            try
            {
                return action();
            }
            catch (const std::bad_alloc&)
            {
                std::terminate();
            }
            catch (...)
            {
                return constructionFailure();
            }
        };
        return runs_.withInspection(target_, inspect);
    }
    const void* RunInspectorFields::component(Target target, cxx::TypeToken type) const noexcept
    {
        return target == target_ && type == schema_.cpp_type ? display_.get() : nullptr;
    }
    std::string_view RunInspectorFields::writeRestriction() const noexcept
    {
        if (!history_)
            return "Pause the run to edit components.";
        return status_ ? std::string_view{} : "The field has a pending error; cancel or retry explicitly.";
    }
    RunInspectorFields::Status RunInspectorFields::refresh()
    {
        const auto inspect = [&](const simulation::ecs::Registry& registry,
                                 const std::optional<editing::HistorySnapshot>& history) -> Status {
            if (active())
            {
                const bool same = history && history_ && history->current == history_->current &&
                                  history->revision == history_->revision;
                if (!same)
                    return cxx::unexpected(RunFailure{editing::makeEditFailure(editing::EEditError::STALE_BASE)});
                return {};
            }
            const auto* source = schema_.operations.get(registry, target_.entity);
            if (!source)
                return cxx::unexpected(RunFailure{ERunError::INVALID_ID});
            try
            {
                auto copy = copy_(source);
                if (!copy)
                    return constructionFailure();
                display_ = std::move(copy);
                history_ = history;
                ++version_;
                return {};
            }
            catch (const std::bad_alloc&)
            {
                std::terminate();
            }
            catch (...)
            {
                return constructionFailure();
            }
        };
        return status_ = runs_.withInspection(target_, inspect);
    }
    RunInspectorFields::Status RunInspectorFields::update()
    {
        if (structural_)
        {
            if (!structural_())
                return status_;
            const auto drop = [&]() -> Status {
                structural_ = {};
                return {};
            };
            status_ = withRead(drop);
            if (!status_)
                return status_;
        }
        auto checked = refresh();
        if (!checked)
            return checked;
        if (commit_)
        {
            if (!history_ || !pending_)
                return constructionFailure();
            const auto edit = [&](SceneEditing& editing) -> Status {
                auto operation = std::move(pending_);
                auto applied = operation(editing);
                if (!applied)
                    pending_ = std::move(operation);
                if (applied)
                    clear(); // Callback payloads retire under the same RunStore dispatch.
                return applied;
            };
            status_ = runs_.withEditing(target_, history_->current, history_->revision, edit);
            if (!status_)
                return status_;
            return refresh();
        }
        return {};
    }
    void RunInspectorFields::clear() noexcept
    {
        pending_ = {};
        original_.reset();
        field_.clear();
        commit_ = false;
    }
    RunInspectorFields::Status RunInspectorFields::cancel()
    {
        const auto clear = [&]() -> Status {
            this->clear();
            structural_ = {};
            display_.reset();
            history_.reset();
            ++version_;
            return {};
        };
        auto result = withRead(clear);
        if (!result && ended(result.error()))
            return clear();
        return result;
    }
    RunInspectorFields::Status RunInspectorFields::release()
    {
        if (!display_ && !pending_ && !structural_)
            return {};
        return cancel();
    }
    bool RunInspectorFields::finish()
    {
        if (active())
            commit_ = true;
        status_ = update();
        return status_.has_value();
    }
    bool RunInspectorFields::queueEdit(cxx::move_only_function<bool()> action)
    {
        if (structural_)
            return reject(editing::EEditError::BUSY);
        structural_ = std::move(action);
        return true;
    }
    bool RunInspectorFields::reject(editing::EEditError error)
    {
        status_ = cxx::unexpected(RunFailure{editing::makeEditFailure(error)});
        return false;
    }
    void RunInspectorFields::fail(const char* message)
    {
        message_ = message;
        static_cast<void>(reject(editing::EEditError::INVALID_ARGUMENT));
    }
    RunInspectorFields::Status RunInspectorFields::connectionFailure(object::EConnectError error)
    {
        if (error == object::EConnectError::ALLOCATION_FAILURE)
            std::terminate();
        return cxx::unexpected(RunFailure{editing::makeEditFailure(
            editing::EEditError::CONTRACT_VIOLATION,
            static_cast<std::uint64_t>(error),
            "run.inspector.connect"
        )});
    }
    RunInspectorFields::Status RunInspectorFields::constructionFailure()
    {
        return cxx::unexpected(
            RunFailure{editing::makeEditFailure(editing::EEditError::CONTRACT_VIOLATION, 0, "run.inspector.component")}
        );
    }
}
