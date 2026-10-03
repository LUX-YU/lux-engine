#include <lux/engine/editor/workbench/CommandSupport.hpp>
#include <lux/engine/editor/workbench/ViewFactorySupport.hpp>
#include <lux/engine/editor/project/WorkspaceView.hpp>
#include <lux/engine/ui/Element.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace lux::editor::project
{
    struct WorkspaceView::Impl final : lux::ui::Element
    {
        Observe observe_;
        Request request_;
        WorkspaceSnapshot snapshot_;
        std::optional<EditorFailure> observation_failure_, request_failure_;
        std::string label_{"Workspace"};
        bool initialized_{};
        Impl(WorkspaceView& view, Observe observe, Request request)
            : Element(view, lux::ui::ElementId{"workspace"}), observe_(std::move(observe)), request_(std::move(request))
        {
            setStretch({1, 1});
            view.setContent(*this);
        }
        EditorResult<void> request(VWorkspaceIntent intent)
        {
            auto result = request_(std::move(intent));
            request_failure_ = result ? std::nullopt : std::optional{result.error()};
            return result;
        }
        void observe()
        {
            if (!initialized_)
                initialized_ = request(RefreshWorkspace{}).has_value();
            auto result = observe_();
            if (result)
            {
                snapshot_ = std::move(*result);
                observation_failure_.reset();
            }
            else
                observation_failure_ = result.error();
        }
        void draw() noexcept override
        {
            const auto button = [&](const char* label, VWorkspaceIntent intent)
            {
                if (ImGui::Button(label))
                    (void)request(std::move(intent));
            };
            ImGui::InputText("Layout label", &label_);
            button("Save current layout as new", SaveLayout{label_});
            ImGui::SameLine();
            button("Refresh directory", RefreshWorkspace{});
            for (const auto* failure : {&observation_failure_, &request_failure_})
                if (*failure)
                    ImGui::TextWrapped("%s: %s", (*failure)->domain.c_str(), (*failure)->message.c_str());
            for (const auto& message : snapshot_.diagnostics)
                ImGui::TextWrapped("%s", message.c_str());
            for (const auto& diagnostic : snapshot_.catalog.diagnostics)
                ImGui::TextWrapped("%s: %s", diagnostic.file.c_str(), diagnostic.failure.detail.c_str());
            for (const auto& layout : snapshot_.catalog.layouts)
            {
                ImGui::PushID(layout.id.value.c_str());
                ImGui::SeparatorText(layout.label.c_str());
                button("Apply", ApplyLayout{layout.id});
                ImGui::SameLine();
                button("Rename to label", RenameLayout{layout.id, label_});
                ImGui::SameLine();
                button("Delete", RemoveLayout{layout.id});
                ImGui::PopID();
            }
            ImGui::SeparatorText("Content recovery (independent of layouts)");
            button("Record current locations", CaptureRecovery{});
            button("Restore recorded content", RestoreRecovery{});
            button("Import old workspace data", MigrateWorkspace{});
            for (const auto& message : snapshot_.recovery)
                ImGui::TextWrapped("%s", message.c_str());
            ImGui::SeparatorText("Publication results");
            for (const auto& report : snapshot_.publications)
            {
                const auto key = std::to_string(report.ticket.value);
                ImGui::PushID(key.c_str());
                ImGui::TextWrapped("%s", report.label.c_str());
                if (report.result)
                {
                    std::visit(
                        [](const auto& result)
                        {
                            if constexpr (std::same_as<std::decay_t<decltype(result)>, persistence::CommitReceipt>)
                                ImGui::TextUnformatted("Published");
                            else
                                ImGui::TextWrapped("%s", result.failure.detail.c_str());
                        },
                        *report.result
                    );
                    if (report.catalog_failure)
                        ImGui::TextWrapped("Directory refresh failed: %s", report.catalog_failure->c_str());
                    button("Acknowledge", AcknowledgeWorkspace{report.ticket});
                }
                else if (report.unknown)
                {
                    ImGui::TextUnformatted("Unknown publication; target remains reserved.");
                    button("Reconcile", ReconcileWorkspace{report.ticket});
                }
                else
                    ImGui::TextUnformatted("Publication pending");
                ImGui::PopID();
            }
        }
    };
    WorkspaceView::WorkspaceView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        Observe observe,
        Request request
    )
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{"lux.editor.workspace"}, "Workspace"),
          impl_(std::make_unique<Impl>(*this, std::move(observe), std::move(request)))
    {
    }
    WorkspaceView::~WorkspaceView() noexcept = default;
    EditorResult<void> WorkspaceView::request(VWorkspaceIntent intent)
    {
        return impl_->request(std::move(intent));
    }
    const WorkspaceSnapshot& WorkspaceView::snapshot() const noexcept
    {
        return impl_->snapshot_;
    }
    const std::optional<EditorFailure>& WorkspaceView::observationFailure() const noexcept
    {
        return impl_->observation_failure_;
    }
    void WorkspaceView::update() noexcept
    {
        impl_->observe();
    }
} // namespace lux::editor::project

namespace lux::editor::project
{
    namespace
    {
        constexpr commands::CommandDescriptor kCaptureRecovery{
            commands::CommandIdView{"lux.editor.recovery.capture"},
            "Record content locations",
            "Workspace"
        };
        constexpr commands::CommandDescriptor kRestoreRecovery{
            commands::CommandIdView{"lux.editor.recovery.restore"},
            "Restore recorded content",
            "Workspace"
        };
        constexpr commands::CommandDescriptor kCommand{
            commands::CommandIdView{"lux.editor.workspace"},
            "Layouts and Recovery",
            "Window"
        };
        constexpr views::ViewFactoryDescriptor kFactoryDescriptor{
            views::ViewTypeIdView{"lux.editor.workspace"},
            "Workspace",
            cxx::typeToken<std::monostate>()
        };
    } // namespace
    std::shared_ptr<views::ViewFactoryEntry> makeWorkspaceViewFactory(
        WorkspaceView::Observe observe,
        WorkspaceView::Request request
    )
    {
        struct Receivers final
        {
            WorkspaceView::Observe observe;
            WorkspaceView::Request request;
        };
        auto receivers = std::make_shared<Receivers>(std::move(observe), std::move(request));
        return views::ViewFactoryEntry::bind<kFactoryDescriptor>(
            contracts::CodeLease::builtin(),
            [receivers](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView>
            {
                return views::DetachedView{
                    contracts::CodeLease::builtin(),
                    std::make_unique<WorkspaceView>(
                        input.dispatcher(),
                        input.paneId(),
                        [receivers] { return receivers->observe(); },
                        [receivers](VWorkspaceIntent intent) { return receivers->request(std::move(intent)); }
                    )
                };
            }
        );
    }
    std::shared_ptr<commands::CommandEntry> makeWorkspaceCommand(
        commands::CommandEntry::Query query,
        desktop::ToolOpening open
    )
    {
        return workbench::detail::bindToolCommand<kCommand, kFactoryDescriptor>(std::move(query), std::move(open));
    }

    std::vector<std::shared_ptr<commands::CommandEntry>> makeRecoveryCommands(
        commands::CommandEntry::Query query,
        WorkspaceView::Request request
    )
    {
        auto check = std::make_shared<commands::CommandEntry::Query>(std::move(query));
        auto receiver = std::make_shared<WorkspaceView::Request>(std::move(request));
        const auto bind = [&]<const commands::CommandDescriptor & Descriptor>(VWorkspaceIntent intent)
        {
            return workbench::detail::bindCommand<Descriptor>(
                [check](const commands::CommandQuery& input) { return (*check)(input); },
                [receiver,
                 intent = std::move(intent)](const commands::CommandInvocation&) -> commands::CommandResult<void>
                {
                    auto result = (*receiver)(intent);
                    if (!result)
                        return workbench::detail::commandFailure(result.error());
                    return {};
                }
            );
        };
        return {
            bind.template operator()<kCaptureRecovery>(CaptureRecovery{}),
            bind.template operator()<kRestoreRecovery>(RestoreRecovery{})
        };
    }

} // namespace lux::editor::project
