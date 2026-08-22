#pragma once
// ============================================================================
//  DebugLineSubsystem.hpp — 把保留式的调试线段推上 LineList feature。
//
//  形状与别的渲染子系统一致：读状态 → 发渲染命令。它读的不是组件而是
//  `lux::ecs::debugdraw::lines()` 那个保留式 CPU 存储（脚本 `lux_debug_draw_line`
//  写进去，主线程契约见 DebugDraw.hpp）—— 这不影响它是不是子系统：子系统的定义
//  是「每帧读某处状态，把差异推给渲染线程」，状态存在组件里还是存在一个进程级
//  存储里，是那份状态自己的事。
//
//  ── 它从哪来（阶段 5）────────────────────────────────────────────────────
//
//  此前这段代码在编辑器的 `SelectionSceneSystem::onPostRenderableUpdate` 里：
//  一个**宿主的**场景系统，自己建 `LineListProxy`、自己发上传命令。于是
//  「读状态 → 发渲染命令」这件事在引擎里有两种做法，取决于谁写的。现在只有一种。
//
//  ⚠️ **每帧只许有一次 uploadLines。** 服务端的 transient 缓冲是 last-writer-wins
//     （`chunk_id` 被忽略），本帧任何第二次上传都会**静默抹掉**前一次的全部内容。
//     新的线段来源必须**并进这里**，不许自己上传。空 vector = 清空。
// ============================================================================

#include <span>
#include <vector>

#include <lux/engine/ecs/DebugDraw.hpp>
#include <lux/engine/ecs/render/RenderStage.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/function/render/client/RenderRequest.hpp>   // 本头持 RenderRequest 成员(此前经旧缓存头搭车)
#include <lux/engine/function/render/client/genops/LineListOperation.ops.hpp>
#include <lux/engine/function/render/client/features/gizmo/GizmoVertex.hpp>

namespace lux::ecs
{
    /// ★ 批 B3 起它是一个**普通的 schedule node**(`ISystem`),不再是
    ///   `RenderSystem` 里 vector 中的一项。
    ///
    ///   它是最适合打头阵的形状:transient(不留跨帧的远端对象、不钉资产引用、
    ///   没有在途 create),所以既不需要观察者、也不需要 releaseRefs —— 迁移只是
    ///   把 `tick(reg, ctx)` 改成 `update(SystemUpdateContext&)`,渲染绑定由构造
    ///   注入(与 CameraViewSubsystem 同款),而不是每帧由调度循环递进来。
    class DebugLineSubsystem final : public RenderStage
    {
    public:
        DebugLineSubsystem() = default;

        /// 线段走 LineList 那个 transient feature。名字是注册名，不是类型名 ——
        /// 曾经写成 "LineList" 导致 attach 静默漏掉（阶段 3 的诊断抓到的）。
        [[nodiscard]] std::span<const std::string_view>
        requiredFeatures() const noexcept override
        {
            static const std::string_view kFeatures[] = { "LineListTransient" };
            return kFeatures;
        }

        void extract(RenderSubsystemContext& uctx) override
        {
            auto& ctx = uctx.render();
            const auto ops = ctx.features().ops<lux::render::LineListOperationIds>("LineListTransient");
            if (!ops.valid()) return;   // feature 没挂 → 整条 no-op

            const auto lines = lux::ecs::debugdraw::lines();
            // 没线段且上一帧也没有 → 什么都不发。**有过**就得发一次空的把它清掉，
            // 否则 transient 缓冲里上一帧的内容会一直留着（它是保留式的，直到被
            // 下一次上传覆盖）。
            if (lines.empty() && !had_lines_) return;
            had_lines_ = !lines.empty();

            verts_.clear();
            verts_.reserve(lines.size() * 2);
            for (const auto& line : lines)
            {
                verts_.push_back(lux::render::GizmoVertex::make(
                    line.from[0], line.from[1], line.from[2],
                    line.color[0], line.color[1], line.color[2]));
                verts_.push_back(lux::render::GizmoVertex::make(
                    line.to[0], line.to[1], line.to[2],
                    line.color[0], line.color[1], line.color[2]));
            }

            lux::render::UploadLineListPayload up{};
            up.scene_id     = ctx.scene();
            up.chunk_id     = 0u;
            up.vertex_count = static_cast<std::uint32_t>(verts_.size());
            lux::render::LineListProxy proxy(ctx.session(), ops);
            (void)proxy.uploadLines(up,
                std::as_bytes(std::span<const lux::render::GizmoVertex>(verts_.data(), verts_.size())),
                alignof(lux::render::GizmoVertex));
        }

    private:
        std::vector<lux::render::GizmoVertex> verts_;
        /// 上一帧发过非空内容没有 —— 决定「现在空了」要不要补一次清空上传。
        bool had_lines_{false};
    };

} // namespace lux::ecs
