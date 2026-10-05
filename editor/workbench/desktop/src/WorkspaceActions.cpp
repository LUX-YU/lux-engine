#include <algorithm>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/desktop/WorkspaceActions.hpp>
#include <lux/engine/editor/workspace/WorkspaceChanges.hpp>
#include <lux/engine/ui/Root.hpp>
#include <random>
#include <thread>
#include <uuid.h>

namespace lux::editor::desktop
{
    namespace
    {
        template <class Error> auto failure(std::string domain, const Error& cause)
        {
            auto code = EEditorError::SOURCE_FAILURE;
            if constexpr (requires { cause.code == decltype(cause.code)::BUSY; })
            {
                if (cause.code == decltype(cause.code)::BUSY)
                {
                    code = EEditorError::BUSY;
                }
            }
            else if constexpr (requires { cause == Error::BUSY; })
            {
                if (cause == Error::BUSY)
                {
                    code = EEditorError::BUSY;
                }
            }
            if constexpr (requires { cause.retryable; })
            {
                if (cause.retryable)
                {
                    code = EEditorError::BUSY;
                }
            }
            return cxx::unexpected(EditorFailure{code, std::move(domain), 0, {}, cause});
        }
    } // namespace
    struct WorkspaceActions::Impl final
    {
        lux::ui::Root& root_;
        UiRegistry& windows_;
        services::ServiceScope& scope_;
        workspace::WorkspaceStore& store_;
        workspace::WorkspaceChanges& changes_;
        const std::thread::id owner_{std::this_thread::get_id()};
        bool dispatching_{};
        Impl(
            lux::ui::Root& root,
            UiRegistry& windows,
            services::ServiceScope& scope,
            workspace::WorkspaceStore& store,
            workspace::WorkspaceChanges& changes
        )
            : root_(root), windows_(windows), scope_(scope), store_(store), changes_(changes)
        {
        }
        struct Dispatch final
        {
            bool& active;
            explicit Dispatch(bool& value) noexcept : active(value)
            {
                active = true;
            }
            ~Dispatch()
            {
                active = false;
            }
            Dispatch(const Dispatch&) = delete;
            Dispatch& operator=(const Dispatch&) = delete;
        };
        EditorResult<void> admission(bool publication = false) const
        {
            if (owner_ != std::this_thread::get_id())
            {
                return cxx::unexpected(EditorFailure{EEditorError::INVALID_STATE, "layout.owner-thread"});
            }
            if (changes_.migrationPending())
            {
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "workspace.migration"});
            }
            if (dispatching_)
            {
                return cxx::unexpected(EditorFailure{EEditorError::BUSY, "layout.dispatch"});
            }
            if (publication && !changes_.hasCapacity())
            {
                return cxx::unexpected(EditorFailure{EEditorError::CAPACITY, "workspace.results"});
            }
            return {};
        }
        EditorResult<void> save(std::string label)
        {
            if (auto ready = admission(true); !ready)
            {
                return ready;
            }
            const Dispatch scope{dispatching_};
            std::mt19937 random{std::random_device{}()};
            workspace::LayoutId id{uuids::to_string(uuids::uuid_random_generator{random}())};
            std::erase(id.value, '-');
            auto layout = windows_.captureLayout(root_, id, std::move(label));
            if (!layout)
            {
                return failure("workspace.capture", layout.error());
            }
            return changes_.save(*layout);
        }
        EditorResult<void> commit(workspace::DockLayout layout)
        {
            auto committed = windows_.applyLayout(root_, scope_, std::move(layout));
            if (!committed)
            {
                return failure("layout.commit", committed.error());
            }
            return {};
        }
        EditorResult<void> apply(workspace::DockLayout layout)
        {
            if (auto ready = admission(); !ready)
            {
                return ready;
            }
            const Dispatch scope{dispatching_};
            return commit(std::move(layout));
        }
        EditorResult<void> apply(const workspace::LayoutId& id)
        {
            if (auto ready = admission(true); !ready)
            {
                return ready;
            }
            const Dispatch scope{dispatching_};
            auto layout = store_.readLayout(id);
            if (!layout)
            {
                return failure("workspace.read", layout.error());
            }
            auto applied = commit(std::move(layout->value));
            if (!applied)
            {
                return applied;
            }
            // Root notifications have returned. They may have accepted another publication;
            // the activity revalidates capacity and file version without rolling back the UI.
            return changes_.select(id);
        }
    };
    WorkspaceActions::WorkspaceActions(
        lux::ui::Root& root,
        UiRegistry& windows,
        services::ServiceScope& scope,
        workspace::WorkspaceStore& store,
        workspace::WorkspaceChanges& changes
    )
        : impl_(std::make_unique<Impl>(root, windows, scope, store, changes))
    {
    }
    WorkspaceActions::~WorkspaceActions() = default;
    EditorResult<void> WorkspaceActions::save(std::string label)
    {
        return impl_->save(std::move(label));
    }
    EditorResult<void> WorkspaceActions::apply(const workspace::LayoutId& id)
    {
        return impl_->apply(id);
    }
    EditorResult<void> WorkspaceActions::apply(workspace::DockLayout layout)
    {
        return impl_->apply(std::move(layout));
    }
} // namespace lux::editor::desktop
