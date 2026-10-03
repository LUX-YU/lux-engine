#include <lux/engine/editor/project/ResultsView.hpp>
#include <lux/engine/ui/Element.hpp>
#include <imgui.h>

namespace lux::editor::project
{
    struct ResultsView::Impl final : lux::ui::Element
    {
        Observe observe_;
        Request request_;
        ResultsSnapshot snapshot_;
        std::optional<EditorFailure> observation_failure_, request_failure_;
        Impl(ResultsView& view, Observe observe, Request request)
            : Element(view, lux::ui::ElementId{"results"}), observe_(std::move(observe)),
              request_(std::move(request))
        {
            setStretch({1, 1});
            view.setContent(*this);
        }
        EditorResult<void> request(VResultIntent intent)
        {
            auto result = request_(std::move(intent));
            request_failure_ = result ? std::nullopt : std::optional{result.error()};
            return result;
        }
        void observe()
        {
            auto result = observe_();
            if (result)
            {
                snapshot_ = std::move(*result);
                observation_failure_.reset();
            }
            else
                observation_failure_ = result.error(); // Keep the last whole display, never infer empty.
        }
        void draw() noexcept override
        {
            for (const auto* failure : {&observation_failure_, &request_failure_})
                if (*failure)
                    ImGui::TextWrapped("%s: %s", (*failure)->domain.c_str(), (*failure)->message.c_str());
            for (const auto& section : snapshot_.sections)
            {
                ImGui::PushID(section.title.c_str());
                ImGui::SeparatorText(section.title.c_str());
                for (const auto& row : section.rows)
                {
                    ImGui::PushID(row.key.c_str());
                    for (const auto& message : row.messages)
                        ImGui::TextWrapped("%s", message.c_str());
                    for (const auto& action : row.actions)
                        if (ImGui::Button(action.label.c_str()))
                            (void)request(action.intent);
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
        }
    };
    ResultsView::ResultsView(object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id, Observe observe, Request request)
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{"lux.editor.content.results"}, "Content and Operations"),
          impl_(std::make_unique<Impl>(*this, std::move(observe), std::move(request))) {}
    ResultsView::~ResultsView() noexcept = default;
    EditorResult<void> ResultsView::request(VResultIntent intent) { return impl_->request(std::move(intent)); }
    const ResultsSnapshot& ResultsView::snapshot() const noexcept { return impl_->snapshot_; }
    const std::optional<EditorFailure>& ResultsView::observationFailure() const noexcept
    {
        return impl_->observation_failure_;
    }
    void ResultsView::update() noexcept { impl_->observe(); }
}
