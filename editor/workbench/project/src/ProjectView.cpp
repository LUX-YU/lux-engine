#include <exception>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/editor/workbench/CommandSupport.hpp>
#include <lux/engine/editor/workbench/ViewFactorySupport.hpp>
#include <lux/engine/ui/Element.hpp>

namespace lux::editor::project
{
    namespace
    {
        constexpr commands::CommandDescriptor kCommand{
            commands::CommandIdView{"lux.editor.assets"},
            "Assets",
            "Window"
        };
        constexpr views::ViewFactoryDescriptor kFactoryDescriptor{
            views::ViewTypeIdView{"lux.editor.project"},
            "Assets",
            cxx::typeToken<std::monostate>()
        };
    } // namespace
    namespace
    {
        constexpr services::ServiceDependency kDependencies[]{
            {services::ServiceNameView{"lux.editor.project.catalog"},
             1,
             cxx::typeToken<ProjectCatalogModel>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT},
            {services::ServiceNameView{"lux.editor.project.open"},
             1,
             cxx::typeToken<ProjectView::Open>(),
             services::EDependencyKind::BORROWED,
             services::EDependencyScope::ROOT,
             {},
             {},
             true}
        };
    } // namespace
    desktop::UiResult<std::unique_ptr<lux::ui::Pane>> ProjectView::createConfigured(
        services::ServiceResolver& resolver,
        const desktop::UiCreateInfo& input
    )
    {
        const bool has_content = !input.content.sessions.empty();
        const bool has_configuration = !input.configuration.bytes.empty();
        const bool is_invalid_input = has_content || has_configuration;
        if (is_invalid_input)
        {
            return cxx::unexpected(desktop::UiFailure{
                desktop::EUiError::INVALID_CONFIGURATION,
                "project.window",
                0,
                "The project window accepts no author binding or configuration payload"
            });
        }
        auto receiver = resolver.require<Open>(1);
        const bool has_receiver_failure = !receiver && receiver.error().code != services::EServiceError::NOT_FOUND;
        if (has_receiver_failure)
        {
            return cxx::unexpected(desktop::UiFailure{
                desktop::EUiError::DEPENDENCY,
                "lux.editor.project.open",
                static_cast<std::uint64_t>(receiver.error().code),
                receiver.error().detail
            });
        }
        const bool is_empty_receiver = receiver && !receiver->get();
        if (is_empty_receiver)
        {
            return cxx::unexpected(
                desktop::UiFailure{desktop::EUiError::INVALID_CONFIGURATION, "lux.editor.project.open"}
            );
        }
        auto catalog = resolver.require<ProjectCatalogModel>(0);
        if (!catalog)
        {
            return cxx::unexpected(desktop::UiFailure{
                desktop::EUiError::DEPENDENCY,
                "project.catalog",
                static_cast<std::uint64_t>(catalog.error().code),
                catalog.error().detail
            });
        }
        auto pane = std::make_unique<ProjectView>(input.dispatcher, input.instance, catalog->get());
        if (receiver)
        {
            auto connection = object::LuxObject::connect(
                pane.get(),
                &ProjectView::openRequested,
                [receiver = *receiver](const AssetReference& value) noexcept { receiver.get()(value); }
            );
            if (!connection)
            {
                return cxx::unexpected(desktop::UiFailure{
                    desktop::EUiError::FACTORY_FAILURE,
                    "lux.editor.project.open",
                    static_cast<std::uint64_t>(connection.error())
                });
            }
            pane->request_connection_ = std::move(*connection);
        }
        return pane;
    }
    constinit const desktop::UiDescriptor kProjectView{
        .type = kFactoryDescriptor.type,
        .label = kFactoryDescriptor.label,
        .dependencies = kDependencies,
        .create = ProjectView::createConfigured
    };
    struct ProjectView::Impl final
    {
        struct Content final : lux::ui::Element
        {
            Impl& state;
            std::string filter;
            explicit Content(ProjectView& parent, Impl& owner)
                : Element(parent, lux::ui::ElementId{"catalog"}), state(owner)
            {
                setStretch({1, 1});
            }
            void draw() noexcept override
            {
                ImGui::InputTextWithHint("##filter", "Filter assets", &filter);
                ImGui::Separator();
                if (state.failure)
                {
                    ImGui::TextUnformatted("Catalog request failed; retaining the previous entries.");
                }
                // The list is a display projection; opening/dragging keeps the exact revision shown here.
                for (std::size_t index{}; index < state.catalog.assets().size(); ++index)
                {
                    const auto& row = state.catalog.assets()[index];
                    if (!filter.empty() && row.path.find(filter) == std::string::npos)
                    {
                        continue;
                    }
                    ImGui::PushID(static_cast<int>(index));
                    if (ImGui::Selectable(row.path.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    {
                        state.pending = state.catalog.reference(row.id);
                    }
                    if (ImGui::BeginDragDropSource())
                    {
                        const auto reference = state.catalog.reference(row.id);
                        ImGui::SetDragDropPayload(kAssetReferencePayload, &reference, sizeof(reference));
                        ImGui::TextUnformatted(row.path.c_str());
                        ImGui::EndDragDropSource();
                    }
                    ImGui::PopID();
                }
            }
        } content;
        ProjectCatalogModel& query;
        ProjectCatalogSnapshot catalog;
        std::optional<AssetReference> pending;
        std::optional<VProjectQueryFailure> failure;
        bool refresh_requested{true};
        object::Connection changes;
        Impl(ProjectView& view, ProjectCatalogModel& access) : content(view, *this), query(access)
        {
            auto connected = object::LuxObject::connect(
                &query,
                &ProjectCatalogModel::changed,
                [this](std::uint64_t) noexcept { refresh_requested = true; }
            );
            if (!connected)
            {
                std::terminate();
            }
            changes = std::move(*connected);
        }
        ProjectQueryResult<void> refresh()
        {
            auto version = query.version();
            if (!version)
            {
                return lux::cxx::unexpected(version.error());
            }
            if (*version == catalog.version())
            {
                return {};
            }
            auto read = query.snapshot();
            if (!read)
            {
                return lux::cxx::unexpected(read.error());
            }
            catalog = std::move(*read);
            return {};
        }
    };
    ProjectView::ProjectView(object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id, ProjectCatalogModel& query)
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{kFactoryDescriptor.type.name()}, "Project"),
          impl_(std::make_unique<Impl>(*this, query))
    {
        if (!setContent(impl_->content))
        {
            std::terminate(); // Fixed content in a detached Pane.
        }
        static_cast<void>(refresh());
    }
    ProjectView::~ProjectView() noexcept = default;
    ProjectQueryResult<void> ProjectView::refresh()
    {
        auto result = impl_->refresh();
        if (!result)
        {
            impl_->failure = result.error();
        }
        else
        {
            impl_->failure.reset();
        }
        return result;
    }
    ProjectQueryResult<void> ProjectView::requestOpen(AssetReference reference)
    {
        auto resolved = impl_->query.resolve(reference, 0);
        if (!resolved)
        {
            impl_->failure = resolved.error();
            return lux::cxx::unexpected(resolved.error());
        }
        // Intent only. The explicitly connected receiver owns admission and its result.
        const auto delivered = emit(openRequested, reference);
        if (!delivered.complete())
        {
            // A partial broadcast must not be retried: some recipients already received the intent.
            const auto error = delivered.closed ? EProjectQueryError::CLOSED : EProjectQueryError::CAPACITY;
            impl_->failure = error;
            return lux::cxx::unexpected(VProjectQueryFailure{error});
        }
        return {};
    }
    const ProjectCatalogSnapshot& ProjectView::catalog() const noexcept
    {
        return impl_->catalog;
    }
    const std::optional<VProjectQueryFailure>& ProjectView::status() const noexcept
    {
        return impl_->failure;
    }
    void ProjectView::update() noexcept
    {
        const auto revision = impl_->query.version();
        const bool needs_refresh =
            impl_->refresh_requested || !revision || *revision != impl_->catalog.version() || impl_->failure;
        if (needs_refresh)
        {
            impl_->refresh_requested = false;
            static_cast<void>(refresh());
        }
        if (impl_->pending)
        {
            auto result = requestOpen(*impl_->pending);
            const auto* error = result ? nullptr : std::get_if<EProjectQueryError>(&result.error());
            const bool is_busy = error && *error == EProjectQueryError::BUSY;
            if (!is_busy)
            {
                impl_->pending.reset();
            }
        }
    }
} // namespace lux::editor::project

namespace lux::editor::project
{
    std::shared_ptr<views::ViewFactoryEntry> makeProjectViewFactory(
        ProjectCatalogModel& catalog,
        cxx::move_only_function<void(const AssetReference&)> open
    )
    {
        auto receiver = std::make_shared<cxx::move_only_function<void(const AssetReference&)>>(std::move(open));
        return views::ViewFactoryEntry::bind<kFactoryDescriptor>(
            lux::object::CodeLease::builtin(),
            [&catalog, receiver](const views::ViewFactoryInput& input) -> views::ViewFactoryResult<views::DetachedView>
            {
                views::DetachedView view{
                    object::CodeLease::builtin(),
                    std::make_unique<ProjectView>(input.dispatcher(), input.paneId(), catalog)
                };
                auto connected = workbench::detail::connectIntent(view, &ProjectView::openRequested, receiver);
                if (!connected)
                {
                    return cxx::unexpected(connected.error());
                }
                return view;
            }
        );
    }
    std::shared_ptr<commands::CommandEntry> makeAssetsCommand(
        commands::CommandEntry::Query query,
        desktop::ToolOpening open
    )
    {
        return workbench::detail::bindToolCommand<kCommand, kFactoryDescriptor>(std::move(query), std::move(open));
    }

} // namespace lux::editor::project
