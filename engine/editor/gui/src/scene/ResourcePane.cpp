#include <lux/engine/editor/gui/scene/ResourcePane.hpp>
#include <lux/engine/ui/Frame.hpp>
#include <imgui.h>

namespace lux::editor::gui
{
    ResourcePane::ResourcePane(scene::SceneEditor &document, std::string id)
        : DocumentPane(document, std::move(id), "Resources"),
          resource_connection_(document.observeScoped<scene::SceneEditor::resourcesChanged>(
              [this](std::uint64_t) noexcept { dirty_ = true; }))
    {
    }

    void ResourcePane::draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &context)
    {
        context.activateContext(lux::ui::UiContextIdView{id()});
        if (dirty_)
        {
            snapshot_ = document_.resources();
            dirty_ = false;
        }
        frame.textMuted("Project assets");
        for (const auto &asset : document_.project().manifest().assets)
        {
            frame.text(document_.project().assetName(asset.id));
        }
        if (snapshot_)
        {
            constexpr const char *states[]{"Unreferenced", "Reading",    "Uploading", "Ready",   "Failed",
                                           "Cancelled",    "Superseded", "Releasing", "Released"};
            for (const auto &row : snapshot_->rows)
            {
                frame.text(document_.project().assetName(row.key.mesh));
                ImGui::SameLine();
                frame.textMuted(states[static_cast<std::size_t>(row.state)]);
                if (row.state == scene::ESceneResourceState::FAILED)
                {
                    frame.text("Dependency: " + document_.project().assetName(row.failed_dependency));
                    std::visit(
                        [&frame](const auto &failure)
                        {
                            using Failure = std::remove_cvref_t<decltype(failure)>;
                            if constexpr (std::same_as<Failure, lux::process::asset_loading::AssetLoadFailure>)
                            {
                                frame.text("Asset load " + std::to_string(static_cast<unsigned>(failure.code)) +
                                           ", storage " + std::to_string(static_cast<unsigned>(failure.storage_error)) +
                                           ", decode " + std::to_string(static_cast<unsigned>(failure.decode.code)));
                            }
                            else if constexpr (!std::same_as<Failure, std::monostate>)
                            {
                                frame.text("Request admission " + std::to_string(static_cast<unsigned>(failure)));
                            }
                        },
                        row.failure);
                    ImGui::PushID(static_cast<int>(row.key.sequence));
                    if (frame.smallButton("Retry"))
                    {
                        const auto retried = document_.retryResource(row.key);
                        action_error_ =
                            retried ? ""
                                    : "Retry rejected: " + std::to_string(static_cast<unsigned>(retried.error().code));
                    }
                    ImGui::PopID();
                }
            }
        }
        if (const auto error = document_.diagnostic(); !error.empty())
        {
            frame.text(error);
        }
        if (!action_error_.empty())
        {
            frame.text(action_error_);
        }
    }
} // namespace lux::editor::gui
