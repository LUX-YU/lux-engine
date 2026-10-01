#include <lux/engine/editor/scene/SceneCreationView.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>

namespace lux::editor::scene
{
    struct SceneCreationView::Impl final
    {
        SceneCreationRequests requests_;
        SceneConfigurationResult<void> status_;
        lux::ui::Layout layout_, actions_;
        lux::ui::Choice preset_;
        SceneConfigurationElement form_;
        lux::ui::Button create_;
        lux::ui::Label message_;
        object::Connection created_, preset_changed_;
        std::optional<SceneCreationConfiguration> pending_;
        bool create_requested_{}, preset_requested_{};
        Impl(SceneCreationView& view, SceneConfigurationInputs inputs, SceneCreationRequests requests)
            : requests_(std::move(requests)), layout_(view, lux::ui::ElementId{"content"}),
              actions_(layout_, lux::ui::ElementId{"actions"}, lux::ui::ELayoutType::HORIZONTAL),
              preset_(actions_, lux::ui::ElementId{"preset"}, {{0, "Empty"}, {1, "2D"}, {2, "3D"}}, 0),
              form_(layout_, lux::ui::ElementId{"configuration"}, std::move(inputs), status_),
              create_(actions_, lux::ui::ElementId{"create"}, "Create scene"),
              message_(layout_, lux::ui::ElementId{"status"})
        {
            view.setContent(layout_);
            const auto retain = [&](auto result, object::Connection& connection) {
                if (result)
                    connection = std::move(*result);
                else
                    status_ = cxx::unexpected(SceneConfigurationFailure{
                        ESceneConfigurationError::CONTROL_FAILURE,
                        "scene.creation.connect",
                        0,
                        {},
                        result.error()
                    });
            };
            retain(
                object::LuxObject::connect(
                    &create_,
                    &lux::ui::Button::activated,
                    [this]() noexcept { create_requested_ = true; }
                ),
                created_
            );
            retain(
                object::LuxObject::connect(
                    &preset_,
                    &lux::ui::Choice::edited,
                    [this](auto) noexcept { preset_requested_ = true; }
                ),
                preset_changed_
            );
        }
        void update()
        {
            if (std::exchange(preset_requested_, false))
                status_ = form_.applyPreset(static_cast<ESceneContentPreset>(preset_.value()));
            if (std::exchange(create_requested_, false) && !pending_)
            {
                auto configured = form_.build();
                if (!configured)
                    status_ = cxx::unexpected(configured.error());
                else
                    pending_.emplace(std::move(*configured));
            }
            if (pending_)
            {
                auto accepted = requests_.create ? requests_.create(*pending_)
                                                 : views::ViewResult<void>{cxx::unexpected(views::EViewError::CLOSED)};
                if (accepted)
                {
                    pending_.reset();
                    status_ = {};
                }
                else
                {
                    status_ = cxx::unexpected(SceneConfigurationFailure{
                        ESceneConfigurationError::CONTROL_FAILURE,
                        "scene.creation.request",
                        static_cast<std::uint64_t>(accepted.error())
                    });
                    if (accepted.error() != views::EViewError::BUSY)
                        pending_.reset();
                }
            }
            create_.setEnabled(!pending_);
            form_.setEnabled(!pending_);
            preset_.setEnabled(!pending_);
            message_.setText(status_ ? "" : status_.error().domain + ": " + status_.error().message);
        }
    };
    SceneCreationView::SceneCreationView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        SceneConfigurationInputs inputs,
        SceneCreationRequests requests,
        SceneConfigurationResult<void>& status
    )
        : Pane(dispatcher, id, lux::ui::PaneTypeId{"lux.editor.scene.creation"}, "New scene"),
          impl_(std::make_unique<Impl>(*this, std::move(inputs), std::move(requests)))
    {
        status = impl_->status_;
    }
    SceneCreationView::~SceneCreationView() noexcept = default;
    SceneConfigurationElement& SceneCreationView::configuration() noexcept
    {
        return impl_->form_;
    }
    void SceneCreationView::requestCreate() noexcept
    {
        impl_->create_requested_ = true;
    }
    const SceneConfigurationResult<void>& SceneCreationView::status() const noexcept
    {
        return impl_->status_;
    }
    void SceneCreationView::update() noexcept
    {
        impl_->update();
    }
    SceneConfigurationResult<views::DetachedView> makeSceneCreationView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        SceneConfigurationInputs inputs,
        SceneCreationRequests requests
    )
    {
        SceneConfigurationResult<void> status;
        auto view = std::make_unique<SceneCreationView>(dispatcher, id, std::move(inputs), std::move(requests), status);
        if (!status)
            return cxx::unexpected(status.error());
        return views::DetachedView{contracts::CodeLease::builtin(), std::move(view)};
    }
}
