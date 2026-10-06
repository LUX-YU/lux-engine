#include <exception>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/project/WorkspaceView.hpp>
#include <lux/engine/editor/workbench/CommandSupport.hpp>
#include <lux/engine/ui/Element.hpp>

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
        constexpr services::ServiceDependency kDependencies[]{
            {services::ServiceNameView{"lux.editor.workspace.observe"},
             1,
             cxx::typeToken<WorkspaceView::Observe>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.workspace.request"},
             1,
             cxx::typeToken<WorkspaceView::Request>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT}
        };
        desktop::UiResult<std::unique_ptr<lux::ui::Pane>> createView(
            services::ServiceResolver& resolver,
            const desktop::UiCreateInfo& input
        )
        {
            const bool has_content = !input.content.sessions.empty();
            const bool has_configuration = !input.configuration.bytes.empty();
            const bool is_invalid_input = has_content || has_configuration;
            if (is_invalid_input)
            {
                return cxx::unexpected(desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "workspace.input"});
            }
            auto observe = resolver.require<WorkspaceView::Observe>(0);
            auto request = resolver.require<WorkspaceView::Request>(1);
            const bool is_missing_observer = !observe;
            const bool is_missing_receiver = !request;
            const bool has_missing_dependency = is_missing_observer || is_missing_receiver;
            if (has_missing_dependency)
            {
                const auto& error = !observe ? observe.error() : request.error();
                return cxx::unexpected(desktop::UiFailure{
                    desktop::EUiError::DEPENDENCY,
                    "workspace.receiver",
                    static_cast<std::uint64_t>(error.code),
                    error.detail
                });
            }
            const bool is_empty_observer = !observe->get();
            const bool is_empty_receiver = !request->get();
            const bool is_invalid_receiver = is_empty_observer || is_empty_receiver;
            if (is_invalid_receiver)
            {
                return cxx::unexpected(
                    desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "workspace.receiver"}
                );
            }
            // Exact synchronous borrows. The declared providers outlive every window in this scope.
            return std::make_unique<WorkspaceView>(
                input.dispatcher,
                input.instance,
                [observer = &observe->get()] { return (*observer)(); },
                [receiver = &request->get()](VWorkspaceIntent value) { return (*receiver)(std::move(value)); }
            );
        }
    } // namespace
    constinit const desktop::UiDescriptor kWorkspaceView{
        .type = views::ViewTypeIdView{"lux.editor.workspace"},
        .label = "Workspace",
        .dependencies = kDependencies,
        .create = createView
    };
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
            if (!view.setContent(*this))
            {
                std::terminate(); // Fixed content in a detached Pane.
            }
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
            {
                initialized_ = request(RefreshWorkspace{}).has_value();
            }
            auto result = observe_();
            if (result)
            {
                snapshot_ = std::move(*result);
                observation_failure_.reset();
            }
            else
            {
                observation_failure_ = result.error();
            }
        }
        void draw() noexcept override
        {
            const auto button = [&](const char* label, VWorkspaceIntent intent)
            {
                if (ImGui::Button(label))
                {
                    (void)request(std::move(intent));
                }
            };
            ImGui::InputText("Layout label", &label_);
            button("Save current layout as new", SaveLayout{label_});
            ImGui::SameLine();
            button("Refresh directory", RefreshWorkspace{});
            for (const auto* failure : {&observation_failure_, &request_failure_})
            {
                if (*failure)
                {
                    ImGui::TextWrapped("%s: %s", (*failure)->domain.c_str(), (*failure)->message.c_str());
                }
            }
            for (const auto& message : snapshot_.diagnostics)
            {
                ImGui::TextWrapped("%s", message.c_str());
            }
            for (const auto& diagnostic : snapshot_.catalog.diagnostics)
            {
                ImGui::TextWrapped("%s: %s", diagnostic.file.c_str(), diagnostic.failure.detail.c_str());
            }
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
            {
                ImGui::TextWrapped("%s", message.c_str());
            }
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
                            {
                                ImGui::TextUnformatted("Published");
                            }
                            else
                            {
                                ImGui::TextWrapped("%s", result.failure.detail.c_str());
                            }
                        },
                        *report.result
                    );
                    if (report.catalog_failure)
                    {
                        ImGui::TextWrapped("Directory refresh failed: %s", report.catalog_failure->c_str());
                    }
                    button("Acknowledge", AcknowledgeWorkspace{report.ticket});
                }
                else if (report.unknown)
                {
                    ImGui::TextUnformatted("Unknown publication; target remains reserved.");
                    button("Reconcile", ReconcileWorkspace{report.ticket});
                }
                else
                {
                    ImGui::TextUnformatted("Publication pending");
                }
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
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{kWorkspaceView.type.name()}, "Workspace"),
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
    std::shared_ptr<commands::CommandEntry> makeWorkspaceCommand(
        commands::CommandEntry::Query query,
        desktop::ToolOpening open
    )
    {
        return workbench::detail::bindToolCommand<kCommand, kWorkspaceView>(std::move(query), std::move(open));
    }

    std::vector<std::shared_ptr<commands::CommandEntry>> makeRecoveryCommands(
        commands::CommandEntry::Query query,
        WorkspaceView::Request request
    )
    {
        auto check = std::make_shared<commands::CommandEntry::Query>(std::move(query));
        auto receiver = std::make_shared<WorkspaceView::Request>(std::move(request));
        const auto bind = [&] < const commands::CommandDescriptor & Descriptor > (VWorkspaceIntent intent)
        {
            return workbench::detail::bindCommand<Descriptor>(
                [check](const commands::CommandQuery& input) { return (*check)(input); },
                [receiver,
                 intent = std::move(intent)](const commands::CommandInvocation&) -> commands::CommandResult<void>
                {
                    auto result = (*receiver)(intent);
                    if (!result)
                    {
                        return workbench::detail::commandFailure(result.error());
                    }
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
