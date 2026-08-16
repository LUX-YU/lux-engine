#include <lux/engine/editor/scene/systems/SelectionSceneSystem.hpp>

#include <lux/engine/editor/app/LuxEditor.hpp>   // EditorRenderInfra (feature_catalog)
#include <lux/engine/editor/app/Selection.hpp>   // Selection::entity

#include <lux/engine/ecs/render/components/HighlightedComponent.hpp>   // 高亮改成实体标签(阶段 5)
#include <lux/engine/ecs/render/components/3d/MeshComponent.hpp>  // auto-select gate coverage
#include <lux/engine/ecs/HierarchyIndex.hpp>                    // subtree walk (highlight whole object)


#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <vector>

namespace lux::editor
{
    SelectionSceneSystem::SelectionSceneSystem(Selection* selection)
        : selection_(selection) {}
    SelectionSceneSystem::~SelectionSceneSystem() = default;

    // -------------------------------------------------------------------------
    void SelectionSceneSystem::update(const lux::ecs::SystemUpdateContext& ctx)
    {
        auto& reg = ctx.registry();

        // ── 门禁自动选中 ─────────────────────────────────────────────────
        // LUX_EDITOR_AUTO_SELECT_FRAME=N:第 N 帧(等场景加载完)若尚无
        // 选中,自动选中场景中第一个 mesh 实体——把高亮条件链的"运行"
        // 分支带进门禁验证(此前门禁无选中,链恒跳过,回归藏在盲区;
        // 见审查文档 5.5)。仅自动化用,未设置该环境变量时零行为。
        if (auto_select_countdown_ == -2)
        {
            const char* v = std::getenv("LUX_EDITOR_AUTO_SELECT_FRAME");
            auto_select_countdown_ = v ? std::atoi(v) : -1;
        }
        if (auto_select_countdown_ > 0)
            --auto_select_countdown_;
        if (auto_select_countdown_ == 0 && !auto_select_done_ && selection_ &&
            selection_->entity() == lux::meta::null_entity)
        {
            for (auto e : reg.view<lux::ecs::MeshComponent>())
            {
                selection_->select(&reg, e);
                auto_select_done_ = true;
                std::fprintf(stderr, "[SelectionSceneSystem] auto-selected entity %u "
                             "(LUX_EDITOR_AUTO_SELECT_FRAME gate coverage)\n",
                             static_cast<uint32_t>(e));
                break;
            }
        }

        // ── 高亮：**写世界，不推渲染系统**（阶段 5）───────────────────────
        //
        // 此前这里算出一个 `unordered_set<entity_id>` 交给
        // `ctx.renderable.setHighlighted(...)` —— 渲染系统为编辑器的「选中」这个
        // 概念开了一条具名入口，还在上下文里存着一份世界状态的镜像。现在编辑器
        // 只管给实体挂 / 摘 `HighlightedComponent`，网格子系统 `all_of` 一下就行；
        // 渲染侧不再认识「选中」，本系统也不再需要够得着 `RenderSystem`。
        std::unordered_set<lux::meta::entity_id> selected;

        const lux::meta::entity_id sel =
            selection_ ? selection_->entity() : lux::meta::null_entity;
        if (sel != lux::meta::null_entity && reg.valid(sel))
        {
            // Highlight the WHOLE object: the selected root plus every descendant
            // mesh entity (W1-B promotes picks to the root, which may itself be
            // meshless). The HierarchyIndex walks the subtree cycle-safely.
            auto& hierarchy = lux::ecs::hierarchyIndex(reg);
            hierarchy.forEachInSubtree(sel,
                [&](lux::meta::entity_id e) { selected.insert(e); });
        }

        // 差集写入：只加该加的、只摘该摘的。全量重推（先清空再重挂）会让
        // `on_construct` / `on_destroy` 每帧各响一次，把观察者变成每帧噪音源。
        for (auto e : reg.view<lux::ecs::HighlightedComponent>())
            if (!selected.count(e)) stale_.push_back(e);
        for (auto e : stale_) reg.remove<lux::ecs::HighlightedComponent>(e);
        stale_.clear();
        for (auto e : selected)
            if (reg.valid(e) && !reg.all_of<lux::ecs::HighlightedComponent>(e))
                reg.emplace<lux::ecs::HighlightedComponent>(e);
    }

    // ★ onPostRenderableUpdate 整个删了(阶段 5b)。它此前干的是「把保留式调试线段
    //   推上 LineList」—— 一个宿主的场景系统自己建 LineListProxy、自己发上传命令。
    //   那件事的形状就是渲染子系统,现在归 `lux::ecs::DebugLineSubsystem`,由 2D/3D
    //   包注册。本系统于是**一行渲染代码都没有了**,纯写世界。

} // namespace lux::editor
