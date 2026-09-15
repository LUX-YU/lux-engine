#include <algorithm>
#include <imgui.h>
#include <lux/engine/editor/gui/scene/InspectorPane.hpp>
#include <lux/engine/editor/gui/scene/UiMeasurement.hpp>
#include <lux/engine/ui/Frame.hpp>

namespace lux::editor::gui
{
    InspectorPane::InspectorPane(scene::SceneEditor &document, std::string id,
                                 std::shared_ptr<const std::vector<ComponentBinding>> bindings)
        : DocumentPane(document, id, "Inspector"), bindings_(std::move(bindings)),
          interaction_(document, std::move(id)), selection_(document.selection()),
          selection_connection_(document.observeScoped<scene::SceneEditor::selectionChanged>(
              [this](const scene::SelectionNotice &value) noexcept
              {
                  selection_ = value;
                  directory_dirty_ = true;
              })),
          objects_connection_(document.observeScoped<scene::SceneEditor::objectsChanged>(
              [this](editing::Revision) noexcept { directory_dirty_ = true; }))
    {
    }

    void InspectorPane::draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &context)
    {
        LUX_UI_MEASURE(UiMeasurement measurement{"Inspector"});
        context.activateContext(lux::ui::UiContextIdView{id()});
        const auto history = document_.historyView();
        if (const auto reason = document_.writeRestriction(); !reason.empty())
        {
            frame.text(reason);
        }
        frame.textMuted(history && history->history.clean ? "Scene editing" : "Scene editing | Unsaved changes");
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (focused && ImGui::IsKeyPressed(ImGuiKey_Escape) && interaction_.active())
        {
            static_cast<void>(interaction_.finish(document_, false));
            return;
        }
        if (!focused && interaction_.active() && !interaction_.finish(document_, true))
        {
            frame.text(interaction_.error.data());
            return;
        }
        const auto current = document_.selection();
        if (current.revision != selection_.revision)
        {
            selection_ = current;
            directory_dirty_ = true;
        }
        if (directory_dirty_)
        {
            if (!interaction_.finish(document_, true))
            {
                frame.text(interaction_.error.data());
                return;
            }
            interaction_.reset();
            components_.clear();
            LUX_UI_MEASURE(++measurement.directory_rebuilds);
            for (auto &component : document_.components(selection_.object))
            {
                LUX_UI_MEASURE(++measurement.binding_queries);
                const auto binding =
                    std::ranges::lower_bound(*bindings_, component.type.hash(), {},
                                             [](const ComponentBinding &value) { return value.type.hash(); });
                const auto *found =
                    binding != bindings_->end() && binding->type == component.type ? &*binding : nullptr;
                components_.push_back({std::move(component), found});
            }
            directory_dirty_ = false;
        }
        if (!selection_.object.valid())
        {
            frame.textMuted("Select an object in the Outliner");
            return;
        }
        for (auto &row : components_)
        {
            LUX_UI_MEASURE(++measurement.rows);
            const auto &component = row.info;
            const auto *binding = row.binding;
            const auto title = binding ? binding->name.c_str() : component.name.c_str();
            ImGui::SetNextItemOpen(row.open, ImGuiCond_Always);
            const bool open = ImGui::CollapsingHeader(title);
            if (open != row.open && interaction_.finish(document_, true))
            {
                row.open = open;
            }
            if (row.open)
            {
                auto table = frame.table({lux::ui::WidgetIdView{component.name}, 2, false, false, false, 110});
                if (table.visible() && binding)
                {
                    binding->draw(document_, selection_.object, frame, interaction_);
                }
            }
            if (directory_dirty_)
            {
                break;
            }
        }
        if (interaction_.error[0])
        {
            frame.text(interaction_.error.data());
        }
    }

    void InspectorPane::requestClose() noexcept
    {
        DocumentPane::requestClose();
    }

    void InspectorPane::poll(PollBudget &budget)
    {
        if (!closing_ && !visible() && !interaction_.finish(document_, true))
        {
            return;
        }
        if (closing_ && !interaction_.finish(document_, false))
        {
            return;
        }
        DocumentPane::poll(budget);
    }

    EditorResult<void> InspectorPane::finishInteraction()
    {
        if (!interaction_.finish(document_, true))
        {
            const auto &failure = interaction_.failure();
            return lux::cxx::unexpected(EditorFailure{
                failure.code == editing::EEditError::BUSY ? EEditorError::BUSY : EEditorError::INVALID_STATE,
                "inspector.preview", static_cast<std::uint64_t>(failure.code), interaction_.error.data(), failure});
        }
        return {};
    }
} // namespace lux::editor::gui
