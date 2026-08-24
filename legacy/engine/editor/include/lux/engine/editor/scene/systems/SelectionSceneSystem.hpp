#pragma once
/**
 * @file SelectionSceneSystem.hpp
 * @brief Editor scene system: 把「谁被选中了」翻译成实体上的高亮标签。
 *
 * 选中的根 + 它子树里的每个网格实体挂 `lux::ecs::HighlightedComponent`（W1-B 把
 * 点选提升到根，而根自己可能没有网格）。网格子系统读那个标签，把
 * `kInstanceFlagHighlight` 折进 per-instance flags。
 *
 * ── 它现在**一行渲染代码都没有**（阶段 5）────────────────────────────────
 *
 * 此前它有两半，各自越过 ECS 直接对渲染说话：
 *  · 前半把选中集 `setHighlighted` 推进 `RenderSystem`（阶段 5a：改成写标签）；
 *  · 后半自己建 `LineListProxy` 上传调试线段（阶段 5b：那是渲染子系统的形状，
 *    归 `lux::ecs::DebugLineSubsystem`）。
 * 剩下的就是一个纯粹读宿主状态、写世界的系统。
 */

#include <lux/engine/ecs/systems/ISystem.hpp>

#include <memory>
#include <vector>

#include <lux/engine/ecs/Registry.hpp>   // entity_id

namespace lux::editor
{
    class Selection;

    class SelectionSceneSystem final : public lux::ecs::ISystem
    {
    public:
        /// @p selection: the editor's shared Selection — HOST state, injected
        /// here since it left the shared tick context. May be null (highlight
        /// then publishes an empty set).
        explicit SelectionSceneSystem(Selection* selection);
        ~SelectionSceneSystem() override;

        /// 跑在 `kPhasePreRender`:高亮标签必须在渲染子系统读它之前写好。
        void update(const lux::ecs::SystemUpdateContext& ctx) override;

    private:
        Selection*                                  selection_{nullptr};

        /// 门禁自动选中(LUX_EDITOR_AUTO_SELECT_FRAME):帧计数与一次性标志。
        /// 高亮条件链的"运行"分支需要 CI 覆盖——门禁场景无交互,靠它把
        /// 选中路径带进验证(审查文档 5.5 教训三)。
        int  auto_select_countdown_{-2};   ///< -2 = env 未读;-1 = 关闭
        bool auto_select_done_{false};

        /// 差集写入的临时缓冲(成员而非局部:每帧复用,不反复分配)。
        /// 边遍历 `view<HighlightedComponent>` 边 `remove` 会让迭代失效,所以先收后删。
        std::vector<lux::ecs::Entity> stale_;
    };

} // namespace lux::editor
