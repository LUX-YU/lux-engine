#include <lux/engine/editor/ui/scene/SceneWorkspace.hpp>
#include <lux/engine/editor/sessions/scene/SceneResourceStatus.hpp>
namespace lux::editor::ui
{
    namespace
    {
        std::string_view label(sessions::ESceneResourceState state) noexcept
        {
            using State = sessions::ESceneResourceState;
            switch (state)
            {
            case State::UNREFERENCED:
                return "No asset reference";
            case State::READING:
                return "Reading / decoding";
            case State::UPLOADING:
                return "Uploading to GPU";
            case State::READY:
                return "Resources ready";
            case State::FAILED:
                return "Failed";
            case State::CANCELLED:
                return "Cancelled";
            case State::SUPERSEDED:
                return "Superseded";
            case State::RELEASING:
                return "Releasing";
            case State::RELEASED:
                return "Released";
            }
            return "Unknown state";
        }
    } // namespace
    struct SceneResourcesPane::Impl final
    {
        sessions::SceneSession &session;
        std::shared_ptr<const sessions::SceneResourceSnapshot> displayed;
        std::optional<sessions::SceneFailure> retry_failure;
        explicit Impl(sessions::SceneSession &source) : session(source)
        {
        }
    };
    SceneResourcesPane::SceneResourcesPane(lux::object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id,
                                           sessions::SceneSession &session)
        : Object(std::move(dispatcher), std::move(id), lux::ui::PaneTypeId{"lux.scene.resources"},
                 "Resources / History"),
          impl_(std::make_unique<Impl>(session))
    {
    }
    SceneResourcesPane::~SceneResourcesPane() noexcept = default;
    void SceneResourcesPane::draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &context)
    {
        context.activateContext(lux::ui::UiContextIdView{id().name()});
        frame.textMuted("Live inspection has no editable content history");
        auto snapshot = impl_->session.readResources();
        if (!snapshot)
        {
            frame.textMuted("Resource status is not ready");
            return;
        }
        impl_->displayed = std::move(*snapshot);
        if (impl_->displayed->rows.empty())
            frame.textMuted("No visual resource requests in this scene");
        for (const auto &row : impl_->displayed->rows)
        {
            const auto key = std::to_string(row.key.sequence);
            auto id = frame.id(lux::ui::WidgetIdView{key});
            frame.text("Entity " + std::to_string(entt::to_integral(row.key.target.entity)) + " | " +
                       std::string(label(row.state)));
            if (row.asset_failure)
                frame.textMuted("Asset error: " + std::to_string(static_cast<unsigned>(row.asset_failure->code)));
            if (row.process_failure)
                frame.textMuted("Process error: " + std::to_string(static_cast<unsigned>(*row.process_failure)));
            if (row.upload_failure)
                frame.textMuted("Upload error: " + std::to_string(static_cast<unsigned>(*row.upload_failure)));
            if (row.backend_status)
                frame.textMuted("Backend status: " + std::to_string(row.backend_status));
            const bool retryable = row.state == sessions::ESceneResourceState::FAILED ||
                                   row.state == sessions::ESceneResourceState::CANCELLED;
            if (retryable && frame.smallButton("Retry"))
            {
                auto result = impl_->session.retryResources(row.key);
                impl_->retry_failure = result ? std::nullopt : std::optional{result.error()};
            }
        }
        if (impl_->retry_failure)
            frame.textMuted("Retry is waiting for the previous request to release its resources");
    }
} // namespace lux::editor::ui
