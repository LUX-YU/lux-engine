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
                for (std::size_t index{}; index < state.catalog.assets.size(); ++index)
                {
                    const auto& row = state.catalog.assets[index];
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
        ProjectCatalogAccess query;
        AssetOpenRequests requests;
        ProjectCatalog catalog;
        std::optional<AssetReference> pending;
        std::optional<VProjectQueryFailure> failure;
        Impl(ProjectView& view, ProjectCatalogAccess access, AssetOpenRequests open)
            : content(view, *this), query(access), requests(open)
        {}
        ProjectQueryResult<void> refresh()
        {
            if (!query)
                return lux::cxx::unexpected(VProjectQueryFailure{EProjectQueryError::UNBOUND});
            auto version = query.version(query.owner);
            if (!version)
                return lux::cxx::unexpected(version.error());
            if (*version == catalog.version)
                return {};
            auto read = query.read(query.owner);
            if (!read)
                return lux::cxx::unexpected(read.error());
            catalog = std::move(*read);
            return {};
        }
        ProjectQueryResult<void> open(AssetReference reference)
        {
            if (!query)
                return lux::cxx::unexpected(VProjectQueryFailure{EProjectQueryError::UNBOUND});
            auto valid = query.resolve(query.owner, reference, 0);
            if (!valid)
                return lux::cxx::unexpected(valid.error());
            return requests.request(reference);
        }
    };
    ProjectView::ProjectView(
        object::ObjectDispatcherRef dispatcher,
        lux::ui::PaneId id,
        ProjectCatalogAccess query,
        AssetOpenRequests requests
    )
        : Pane(dispatcher, std::move(id), lux::ui::PaneTypeId{"lux.editor.project"}, "Project"),
          impl_(std::make_unique<Impl>(*this, query, requests))
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
        auto result = impl_->open(reference);
        if (!result)
            impl_->failure = result.error();
        return result;
    }
    const ProjectCatalog& ProjectView::catalog() const noexcept
    {
        return impl_->catalog;
    }
    const std::optional<VProjectQueryFailure>& ProjectView::status() const noexcept
    {
        return impl_->failure;
    }
    void ProjectView::update() noexcept
    {
        static_cast<void>(refresh());
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
        ProjectCatalogAccess query,
        AssetOpenRequests requests
    )
    {
        return {
            contracts::CodeLease::builtin(),
            std::make_unique<ProjectView>(dispatcher, std::move(id), query, requests)
        };
    }
}
