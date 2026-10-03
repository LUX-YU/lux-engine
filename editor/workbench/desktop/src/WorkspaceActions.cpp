#include <lux/engine/editor/desktop/WorkspaceActions.hpp>
#include <lux/engine/editor/desktop/ViewHost.hpp>
#include <lux/engine/editor/workspace/WorkspaceChanges.hpp>
#include <algorithm>
#include <random>
#include <thread>
#include <uuid.h>

namespace lux::editor::desktop
{
    namespace
    {
        template<class Error> auto failure(std::string domain, const Error& cause)
        {
            auto code = EEditorError::SOURCE_FAILURE;
            if constexpr (requires { cause.code == decltype(cause.code)::BUSY; })
            {
                if (cause.code == decltype(cause.code)::BUSY)
                    code = EEditorError::BUSY;
            }
            else if constexpr (requires { cause == Error::BUSY; })
            {
                if (cause == Error::BUSY)
                    code = EEditorError::BUSY;
            }
            if constexpr (requires { cause.retryable; })
                if (cause.retryable)
                    code = EEditorError::BUSY;
            return cxx::unexpected(EditorFailure{code, std::move(domain), 0, {}, cause});
        }
    }
    struct WorkspaceActions::Impl final
    {
        ViewHost& host_;
        workspace::WorkspaceStore& store_;
        workspace::WorkspaceChanges& changes_;
        object::ObjectDispatcherRef dispatcher_;
        const std::thread::id owner_{std::this_thread::get_id()};
        bool dispatching_{};
        Impl(
            ViewHost& host, workspace::WorkspaceStore& store, workspace::WorkspaceChanges& changes,
            object::ObjectDispatcherRef dispatcher
        ) : host_(host), store_(store), changes_(changes), dispatcher_(dispatcher)
        {}
        struct Dispatch final
        {
            bool& active;
            explicit Dispatch(bool& value) noexcept : active(value) { active = true; }
            ~Dispatch() { active = false; }
            Dispatch(const Dispatch&) = delete;
            Dispatch& operator=(const Dispatch&) = delete;
        };
        EditorResult<void> admission(bool publication = false) const
        {
            if (owner_ != std::this_thread::get_id())
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "layout.owner-thread"});
            if (dispatching_)
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "layout.dispatch"});
            if (publication && !changes_.hasCapacity())
                return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "workspace.results"});
            return {};
        }
        EditorResult<void> save(std::string label)
        {
            if (auto ready = admission(true); !ready)
                return ready;
            const Dispatch scope{dispatching_};
            std::mt19937 random{std::random_device{}()};
            workspace::LayoutId id{uuids::to_string(uuids::uuid_random_generator{random}())};
            std::erase(id.value, '-');
            auto layout = host_.captureLayout(id, std::move(label));
            if (!layout)
                return failure("workspace.capture", layout.error());
            return changes_.save(*layout);
        }
        EditorResult<void> commit(workspace::DockLayout layout, const views::ViewFactorySnapshot& catalog)
        {
            auto create_input = [&](views::ViewTypeId type, lux::ui::PaneId id)
                -> views::ViewFactoryResult<views::ViewFactoryInput> {
                const auto entries = catalog.entries();
                const auto factory = std::ranges::find_if(entries, [&](const auto& entry) {
                    return entry->descriptor().type == type.view();
                });
                if (factory == entries.end())
                    return cxx::unexpected(views::ViewFactoryFailure{
                        views::EViewFactoryError::NOT_FOUND, "layout.view"
                    });
                const auto make_input = [&](auto value) {
                    using Value = decltype(value);
                    return views::ViewFactoryInput{
                        dispatcher_, id, contracts::CodeLease::builtin(),
                        cxx::typeToken<Value>(), std::make_shared<const Value>(std::move(value))
                    };
                };
                if ((*factory)->descriptor().binding_type == cxx::typeToken<views::ContentViewInput>())
                    return make_input(views::ContentViewInput{});
                return make_input(std::monostate{});
            };
            auto prepared = host_.prepareLayout(std::move(layout), catalog, create_input);
            if (!prepared)
                return failure("layout.prepare", prepared.error());
            auto committed = host_.commit(*prepared);
            if (!committed)
                return failure("layout.commit", committed.error());
            return {};
        }
        EditorResult<void> apply(workspace::DockLayout layout, const views::ViewFactorySnapshot& catalog)
        {
            if (auto ready = admission(); !ready)
                return ready;
            const Dispatch scope{dispatching_};
            return commit(std::move(layout), catalog);
        }
        EditorResult<void> apply(const workspace::LayoutId& id, const views::ViewFactorySnapshot& catalog)
        {
            if (auto ready = admission(true); !ready)
                return ready;
            const Dispatch scope{dispatching_};
            auto layout = store_.readLayout(id);
            if (!layout)
                return failure("workspace.read", layout.error());
            auto applied = commit(std::move(layout->value), catalog);
            if (!applied)
                return applied;
            // Host notifications have returned. They may have accepted another publication;
            // the activity revalidates capacity and file version without rolling back the UI.
            return changes_.select(id);
        }
    };
    WorkspaceActions::WorkspaceActions(
        ViewHost& host, workspace::WorkspaceStore& store, workspace::WorkspaceChanges& changes,
        object::ObjectDispatcherRef dispatcher
    ) : impl_(std::make_unique<Impl>(host, store, changes, dispatcher))
    {}
    WorkspaceActions::~WorkspaceActions() = default;
    EditorResult<void> WorkspaceActions::save(std::string label) { return impl_->save(std::move(label)); }
    EditorResult<void> WorkspaceActions::apply(const workspace::LayoutId& id, const views::ViewFactorySnapshot& catalog)
    {
        return impl_->apply(id, catalog);
    }
    EditorResult<void> WorkspaceActions::apply(workspace::DockLayout layout, const views::ViewFactorySnapshot& catalog)
    {
        return impl_->apply(std::move(layout), catalog);
    }
}
