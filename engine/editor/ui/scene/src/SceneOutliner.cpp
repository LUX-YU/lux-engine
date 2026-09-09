#include <lux/engine/editor/ui/scene/SceneWorkspace.hpp>
#include <algorithm>
#include <unordered_map>
namespace lux::editor::ui
{
    struct SceneOutliner::Impl final
    {
        sessions::SceneSession &session;
        sessions::SceneOutlineRef snapshot;
        std::string filter;
        std::vector<std::string> row_ids;
        std::vector<std::vector<std::size_t>> children;
        std::vector<std::size_t> roots;
        bool flat{}, hierarchy_cycle{};
        explicit Impl(sessions::SceneSession &source) : session(source)
        {
        }
        void refresh(sessions::SceneOutlineRef next)
        {
            if (snapshot == next)
                return;
            std::vector<std::string> ids;
            std::vector<std::vector<std::size_t>> links(next->rows.size());
            std::vector<std::size_t> top;
            const auto count = next->rows.size();
            std::vector<std::size_t> parents(count, count);
            std::vector<std::uint8_t> color(count);
            std::unordered_map<lux::simulation::ecs::Entity, std::size_t> indices;
            indices.reserve(count);
            ids.reserve(count);
            top.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
                indices.emplace(next->rows[i].target.entity, i);
            for (std::size_t i = 0; i < next->rows.size(); ++i)
            {
                const auto &row = next->rows[i];
                ids.push_back(std::to_string(entt::to_integral(row.target.entity)));
                if (!row.parent || row.parent->session != row.target.session)
                    continue;
                const auto parent = indices.find(row.parent->entity);
                if (parent != indices.end())
                    parents[i] = parent->second;
            }
            bool cycle{};
            // Break one view-only edge per cycle. Every entity remains reachable; source data is unchanged.
            for (std::size_t i = 0; i < count; ++i)
            {
                auto at = i;
                while (at < count && color[at] == 0)
                {
                    color[at] = 1;
                    at = parents[at];
                }
                if (at < count && color[at] == 1)
                {
                    parents[at] = count;
                    cycle = true;
                }
                at = i;
                while (at < count && color[at] == 1)
                {
                    color[at] = 2;
                    at = parents[at];
                }
            }
            for (std::size_t i = 0; i < count; ++i)
            {
                if (parents[i] == count)
                    top.push_back(i);
                else
                    links[parents[i]].push_back(i);
            }
            snapshot = std::move(next);
            row_ids = std::move(ids);
            children = std::move(links);
            roots = std::move(top);
            hierarchy_cycle = cycle;
        }
        void drawRow(lux::ui::Frame &frame, std::size_t index, std::size_t depth)
        {
            if (depth > snapshot->rows.size() || depth > 512)
            {
                frame.textMuted("Hierarchy depth limit: use Flat view to see remaining entities");
                return;
            }
            const auto &row = snapshot->rows[index];
            const auto selected = session.selection().current;
            const bool filtering = !filter.empty();
            if (filtering && row.label.find(filter) == std::string::npos)
                return;
            auto tree =
                frame.treeRow({lux::ui::WidgetIdView{row_ids[index]}, row.label, selected && *selected == row.target,
                               flat || filtering || children[index].empty(), true});
            if (tree.activated())
                static_cast<void>(session.select(row.target));
            if (tree.open() && !filtering && !flat)
                for (const auto child : children[index])
                    drawRow(frame, child, depth + 1);
        }
    };
    SceneOutliner::SceneOutliner(lux::object::ObjectDispatcherRef dispatcher, lux::ui::PaneId id,
                                 sessions::SceneSession &session)
        : Object(std::move(dispatcher), std::move(id), lux::ui::PaneTypeId{"lux.scene.outliner"}, "Scene Outline"),
          impl_(std::make_unique<Impl>(session))
    {
    }
    SceneOutliner::~SceneOutliner() noexcept = default;
    void SceneOutliner::draw(lux::ui::Frame &frame, lux::ui::PaneDrawContext &context)
    {
        context.activateContext(lux::ui::UiContextIdView{id().name()});
        static_cast<void>(frame.inputText("Filter", impl_->filter));
        static_cast<void>(frame.checkbox("Flat view", impl_->flat));
        auto outline = impl_->session.readOutline();
        if (!outline)
        {
            frame.textMuted("Scene query is not ready");
            return;
        }
        try
        {
            impl_->refresh(*outline);
        }
        catch (const std::bad_alloc &)
        {
            frame.textMuted("Unable to prepare outline rows");
            return;
        }
        if (impl_->hierarchy_cycle)
            frame.textMuted("Cyclic hierarchy: showing all entities");
        if (impl_->flat || !impl_->filter.empty())
            for (std::size_t i = 0; i < impl_->snapshot->rows.size(); ++i)
                impl_->drawRow(frame, i, 0);
        else
            for (const auto root : impl_->roots)
                impl_->drawRow(frame, root, 0);
        if (impl_->snapshot->rows.empty())
            frame.textMuted("Scene has no entities");
    }
} // namespace lux::editor::ui
