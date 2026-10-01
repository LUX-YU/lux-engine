#include <lux/engine/editor/scene/SceneConfigurationView.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>

namespace lux::editor::scene
{
    namespace
    {
        auto rejected(SceneEditError error)
        {
            return cxx::unexpected(SceneConfigurationFailure{
                ESceneConfigurationError::CONTROL_FAILURE,
                "scene.configuration.session",
                0,
                {},
                std::move(error)
            });
        }
        bool busy(const SceneConfigurationFailure& failure) noexcept
        {
            const auto* error = std::get_if<SceneEditError>(&failure.cause);
            return error && (error->code == ESceneEditError::BUSY || (error->code == ESceneEditError::SESSION &&
                                                                      error->session == sessions::ESessionError::BUSY));
        }
    }
    struct SceneConfigurationView::Impl final
    {
        sessions::TSessionAccess<SceneSession> sessions_;
        SceneConfigurationInputs inputs_;
        std::optional<sessions::TSessionKey<SceneSession>> binding_;
        sessions::ContentStamp base_;
        lux::ui::Layout layout_, actions_;
        lux::ui::Button apply_, revert_;
        lux::ui::Label message_;
        std::unique_ptr<SceneConfigurationElement> form_;
        std::optional<SceneSetConfiguration> pending_;
        SceneConfigurationResult<void> status_;
        object::Connection apply_connection_, revert_connection_;
        bool apply_requested_{}, revert_requested_{};
        Impl(
            SceneConfigurationView& view,
            sessions::TSessionAccess<SceneSession> sessions,
            SceneConfigurationInputs inputs
        )
            : sessions_(sessions), inputs_(std::move(inputs)), layout_(view, lux::ui::ElementId{"body"}),
              actions_(layout_, lux::ui::ElementId{"actions"}, lux::ui::ELayoutType::HORIZONTAL),
              apply_(actions_, lux::ui::ElementId{"apply"}, "Apply configuration"),
              revert_(actions_, lux::ui::ElementId{"revert"}, "Revert draft"),
              message_(layout_, lux::ui::ElementId{"status"})
        {
            view.setContent(layout_);
            const auto connect = [&](auto& button, bool& flag, auto& connection) {
                auto result = object::LuxObject::connect(&button, &lux::ui::Button::activated, [&flag]() noexcept {
                    flag = true;
                });
                if (result)
                    connection = std::move(*result);
                else
                    status_ = cxx::unexpected(SceneConfigurationFailure{
                        ESceneConfigurationError::CONTROL_FAILURE,
                        "scene.configuration.connect",
                        0,
                        {},
                        result.error()
                    });
            };
            connect(apply_, apply_requested_, apply_connection_);
            connect(revert_, revert_requested_, revert_connection_);
        }
        SceneConfigurationResult<void> rebind(sessions::TSessionKey<SceneSession> target)
        {
            if (object::LuxObject::isDispatching())
                return rejected(SceneEditError{ESceneEditError::BUSY});
            // A cross-session binding could release plugin values under a different gate. Keep this pane's
            // binding explicit and stable; the host may create a new complete pane for another session.
            if (binding_ && *binding_ != target)
                return cxx::unexpected(
                    SceneConfigurationFailure{ESceneConfigurationError::INVALID_ARGUMENT, "scene.configuration.binding"}
                );
            auto session = sessions_.read(target);
            if (!session)
                return rejected(SceneEditError{session.error()});
            auto read = session->get().read();
            if (!read)
                return rejected(read.error());
            const auto stamp = session->get().describe().current;
            SceneConfigurationResult<void> result;
            auto loaded = read->withRead([&](const SceneReadView& source) -> SceneEditResult<void> {
                auto candidate = std::make_unique<SceneConfigurationElement>(
                    layout_,
                    lux::ui::ElementId{"configuration"},
                    inputs_,
                    result
                );
                candidate->setVisible(false);
                if (!result)
                    return {};
                result = candidate->load(source.configuration());
                if (!result)
                    return {};
                form_ = std::move(candidate);
                form_->setVisible(true);
                pending_.reset();
                base_ = stamp;
                binding_ = target;
                return {};
            });
            if (!loaded)
                return rejected(loaded.error());
            return result;
        }
        SceneConfigurationResult<void> clear()
        {
            if (!binding_)
            {
                form_.reset();
                return {};
            }
            auto session = sessions_.read(*binding_);
            if (!session)
            {
                if (session.error() != sessions::ESessionError::STALE_SESSION)
                    return rejected(SceneEditError{session.error()});
                form_.reset();
                pending_.reset();
                binding_.reset();
                return {};
            }
            auto read = session->get().read();
            if (!read)
                return rejected(read.error());
            auto cleared = read->withRead([&](const SceneReadView&) -> SceneEditResult<void> {
                form_.reset();
                pending_.reset();
                binding_.reset();
                return {};
            });
            if (!cleared)
                return rejected(cleared.error());
            return {};
        }
        SceneConfigurationResult<void> apply()
        {
            if (!binding_)
                return cxx::unexpected(
                    SceneConfigurationFailure{ESceneConfigurationError::INVALID_ARGUMENT, "scene.configuration.unbound"}
                );
            auto session = sessions_.edit(*binding_);
            if (!session)
                return rejected(SceneEditError{session.error()});
            if (session->get().describe().current != base_)
                return rejected(SceneEditError{ESceneEditError::STALE_CONTENT});
            if (!pending_)
            {
                auto read = session->get().read();
                if (!read)
                    return rejected(read.error());
                SceneConfigurationResult<void> result;
                auto prepared = read->withRead([&](const SceneReadView& source) -> SceneEditResult<void> {
                    auto change = form_->buildEdit(source.configuration());
                    if (change)
                        pending_.emplace(std::move(*change));
                    else
                        result = cxx::unexpected(change.error());
                    return {};
                });
                if (!prepared)
                    return rejected(prepared.error());
                if (!result)
                    return result;
            }
            SceneEditBatch batch{base_, "Scene configuration", {}};
            batch.edits.emplace_back(*pending_);
            auto committed = session->get().apply(std::move(batch));
            if (!committed)
                return rejected(committed.error());
            base_ = committed->content;
            pending_.reset();
            return {};
        }
        void update()
        {
            if (revert_requested_ && binding_)
            {
                status_ = rebind(*binding_);
                if (status_ || !busy(status_.error()))
                    revert_requested_ = false;
            }
            if (apply_requested_)
            {
                status_ = apply();
                if (status_ || !busy(status_.error()))
                    apply_requested_ = false;
            }
            if (form_)
                form_->setEnabled(!pending_);
            message_.setText(status_ ? "" : status_.error().domain);
        }
    };
    SceneConfigurationView::SceneConfigurationView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        sessions::TSessionAccess<SceneSession> sessions,
        SceneConfigurationInputs inputs
    )
        : Pane(dispatcher, id, lux::ui::PaneTypeId{"lux.editor.scene.configuration"}, "Scene configuration"),
          impl_(std::make_unique<Impl>(*this, sessions, std::move(inputs)))
    {}
    SceneConfigurationView::~SceneConfigurationView() noexcept
    {
        if (!impl_->clear())
            std::terminate();
    }
    SceneConfigurationResult<void> SceneConfigurationView::rebind(sessions::TSessionKey<SceneSession> target)
    {
        return impl_->rebind(target);
    }
    SceneConfigurationResult<void> SceneConfigurationView::prepareClose()
    {
        return impl_->clear();
    }
    SceneConfigurationElement* SceneConfigurationView::form() noexcept
    {
        return impl_->form_.get();
    }
    void SceneConfigurationView::requestApply() noexcept
    {
        impl_->apply_requested_ = true;
    }
    void SceneConfigurationView::requestRevert() noexcept
    {
        impl_->revert_requested_ = true;
    }
    const SceneConfigurationResult<void>& SceneConfigurationView::status() const noexcept
    {
        return impl_->status_;
    }
    void SceneConfigurationView::update() noexcept
    {
        impl_->update();
    }
    SceneConfigurationResult<views::DetachedView> makeSceneConfigurationView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        sessions::TSessionAccess<SceneSession> sessions,
        SceneConfigurationInputs inputs,
        sessions::TSessionKey<SceneSession> target
    )
    {
        auto view = std::make_unique<SceneConfigurationView>(dispatcher, id, sessions, std::move(inputs));
        if (!view->status())
            return cxx::unexpected(view->status().error());
        auto bound = view->rebind(target);
        if (!bound)
            return cxx::unexpected(bound.error());
        return views::DetachedView{
            contracts::CodeLease::builtin(),
            std::move(view),
            [](lux::ui::Pane& pane) -> views::ViewCloseResult {
                auto closed = static_cast<SceneConfigurationView&>(pane).prepareClose();
                if (!closed)
                    return cxx::unexpected(views::ViewCloseFailure{
                        closed.error().domain,
                        closed.error().reason,
                        closed.error().message,
                        busy(closed.error())
                    });
                return {};
            }
        };
    }
}
