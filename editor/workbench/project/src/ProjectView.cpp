#include <lux/engine/editor/project/ProjectView.hpp>
#include <lux/engine/ui/Element.hpp>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace lux::editor::project
{
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
                    ImGui::TextUnformatted("Catalog request failed; retaining the previous entries.");
                // The list is a display projection; opening/dragging keeps the exact revision shown here.
                for (std::size_t index{}; index < state.catalog.assets().size(); ++index)
                {
                    const auto& row = state.catalog.assets()[index];
                    if (!filter.empty() && row.path.find(filter) == std::string::npos)
                        continue;
                    ImGui::PushID(static_cast<int>(index));
                    if (ImGui::Selectable(row.path.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        state.pending = state.catalog.reference(row.id);
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
            auto connected =
                object::LuxObject::connect(&query, &ProjectCatalogModel::changed, [this](std::uint64_t) noexcept {
                    refresh_requested = true;
                });
            if (!connected)
                std::terminate();
            changes = std::move(*connected);
        }
        ProjectQueryResult<void> refresh()
        {
            auto version = query.version();
            if (!version)
                return lux::cxx::unexpected(version.error());
            if (*version == catalog.version())
                return {};
            auto read = query.snapshot();
            if (!read)
                return lux::cxx::unexpected(read.error());
            catalog = std::move(*read);
            return {};
        }
    };
    ProjectView::ProjectView(object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id, ProjectCatalogModel& query)
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{"lux.editor.project"}, "Project"),
          impl_(std::make_unique<Impl>(*this, query))
    {
        setContent(impl_->content);
        static_cast<void>(refresh());
    }
    ProjectView::~ProjectView() noexcept = default;
    ProjectQueryResult<void> ProjectView::refresh()
    {
        auto result = impl_->refresh();
        if (!result)
            impl_->failure = result.error();
        else
            impl_->failure.reset();
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
                impl_->pending.reset();
        }
    }
    views::DetachedView makeProjectView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        ProjectCatalogModel& query
    )
    {
        return {contracts::CodeLease::builtin(), std::make_unique<ProjectView>(dispatcher, std::move(id), query)};
    }
}
