#include <exception>
#include <imgui.h>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/project/ResultsView.hpp>
#include <lux/engine/editor/workbench/CommandSupport.hpp>
#include <lux/engine/editor/workbench/ViewFactorySupport.hpp>
#include <lux/engine/ui/Element.hpp>

namespace lux::editor::project
{
    namespace
    {
        constexpr commands::CommandDescriptor kCommand{
            commands::CommandIdView{"lux.editor.content.results"},
            "Content and Operations",
            "Window"
        };
        constexpr views::ViewFactoryDescriptor kFactoryDescriptor{
            views::ViewTypeIdView{"lux.editor.content.results"},
            "Content and Operations",
            cxx::typeToken<std::monostate>()
        };
        constexpr services::ServiceDependency kDependencies[]{
            {services::ServiceNameView{"lux.editor.results.observe"},
             1,
             cxx::typeToken<ResultsView::Observe>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.results.request"},
             1,
             cxx::typeToken<ResultsView::Request>(),
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
                return cxx::unexpected(desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "results.input"});
            }
            auto observe = resolver.require<ResultsView::Observe>(0);
            auto request = resolver.require<ResultsView::Request>(1);
            const bool is_missing_observer = !observe;
            const bool is_missing_receiver = !request;
            const bool has_missing_dependency = is_missing_observer || is_missing_receiver;
            if (has_missing_dependency)
            {
                const auto& error = !observe ? observe.error() : request.error();
                return cxx::unexpected(desktop::UiFailure{
                    desktop::EUiError::DEPENDENCY,
                    "results.receiver",
                    static_cast<std::uint64_t>(error.code),
                    error.detail
                });
            }
            const bool is_empty_observer = !observe->get();
            const bool is_empty_receiver = !request->get();
            const bool is_invalid_receiver = is_empty_observer || is_empty_receiver;
            if (is_invalid_receiver)
            {
                return cxx::unexpected(desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "results.receiver"}
                );
            }
            // Exact synchronous borrows. The declared providers outlive every window in this scope.
            return std::make_unique<ResultsView>(
                input.dispatcher,
                input.instance,
                [observer = &observe->get()] { return (*observer)(); },
                [receiver = &request->get()](VResultIntent value) { return (*receiver)(std::move(value)); }
            );
        }
    } // namespace
    constinit const desktop::UiDescriptor kResultsView{
        .type = kFactoryDescriptor.type,
        .label = kFactoryDescriptor.label,
        .dependencies = kDependencies,
        .create = createView
    };
    struct ResultsView::Impl final : lux::ui::Element
    {
        Observe observe_;
        Request request_;
        ResultsSnapshot snapshot_;
        std::optional<EditorFailure> observation_failure_, request_failure_;
        Impl(ResultsView& view, Observe observe, Request request)
            : Element(view, lux::ui::ElementId{"results"}), observe_(std::move(observe)), request_(std::move(request))
        {
            setStretch({1, 1});
            if (!view.setContent(*this))
            {
                std::terminate(); // Fixed content in a detached Pane.
            }
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
            {
                observation_failure_ = result.error(); // Keep the last whole display, never infer empty.
            }
        }
        void draw() noexcept override
        {
            for (const auto* failure : {&observation_failure_, &request_failure_})
            {
                if (*failure)
                {
                    ImGui::TextWrapped("%s: %s", (*failure)->domain.c_str(), (*failure)->message.c_str());
                }
            }
            for (const auto& section : snapshot_.sections)
            {
                ImGui::PushID(section.title.c_str());
                ImGui::SeparatorText(section.title.c_str());
                for (const auto& row : section.rows)
                {
                    ImGui::PushID(row.key.c_str());
                    for (const auto& message : row.messages)
                    {
                        ImGui::TextWrapped("%s", message.c_str());
                    }
                    for (const auto& action : row.actions)
                    {
                        if (ImGui::Button(action.label.c_str()))
                        {
                            (void)request(action.intent);
                        }
                    }
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
        }
    };
    ResultsView::ResultsView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        Observe observe,
        Request request
    )
        : Pane(
              dispatcher,
              std::move(id),
              lux::ui::PaneTypeId{kFactoryDescriptor.type.name()},
              "Content and Operations"
          ),
          impl_(std::make_unique<Impl>(*this, std::move(observe), std::move(request)))
    {
    }
    ResultsView::~ResultsView() noexcept = default;
    EditorResult<void> ResultsView::request(VResultIntent intent)
    {
        return impl_->request(std::move(intent));
    }
    const ResultsSnapshot& ResultsView::snapshot() const noexcept
    {
        return impl_->snapshot_;
    }
    const std::optional<EditorFailure>& ResultsView::observationFailure() const noexcept
    {
        return impl_->observation_failure_;
    }
    void ResultsView::update() noexcept
    {
        impl_->observe();
    }
} // namespace lux::editor::project

namespace lux::editor::project
{
    std::shared_ptr<views::ViewFactoryEntry> makeResultsViewFactory(
        ResultsView::Observe observe,
        ResultsView::Request request
    )
    {
        struct Receivers final
        {
            ResultsView::Observe observe;
            ResultsView::Request request;
        };
        auto receivers = std::make_shared<Receivers>(std::move(observe), std::move(request));
        return views::ViewFactoryEntry::bind<kFactoryDescriptor>(
            lux::object::CodeLease::builtin(),
            [receivers](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView>
            {
                return views::DetachedView{
                    lux::object::CodeLease::builtin(),
                    std::make_unique<ResultsView>(
                        input.dispatcher(),
                        input.paneId(),
                        [receivers] { return receivers->observe(); },
                        [receivers](VResultIntent intent) { return receivers->request(std::move(intent)); }
                    )
                };
            }
        );
    }
    std::shared_ptr<commands::CommandEntry> makeResultsCommand(
        commands::CommandEntry::Query query,
        desktop::ToolOpening open
    )
    {
        return workbench::detail::bindToolCommand<kCommand, kFactoryDescriptor>(std::move(query), std::move(open));
    }

} // namespace lux::editor::project
