#pragma once
/**
 * @file SceneGraphCache.hpp
 * @brief 一个场景的编译渲染图 + 图基础设施 + 图相关退休。
 *
 * 从 RenderScene 拆出的第一个组件(见 .internal 分层设计 §RenderScene 拆解)。
 * 一句话职责:**持有本场景的编译图,并在目标布局的格式键变化时重编**。
 *
 * 拆出来的东西:
 *   - RGVulkanResourceAllocator / RGVulkanRecorder(每场景一份的图基础设施)
 *   - SceneGraphState(编译图 + 上次布局 + 诊断一次性开关)
 *   - compileGraphTemplate 的全部实现(事务式 build-then-commit)
 *   - 两条与图配对的退休 FIFO(retired_graphs_ / retired_view_resources_),
 *     以及它们的老化回收 —— 三条 FIFO 里的两条自然归到这里,剩下的
 *     pending_destroys_(视图)属于视图管理,不在本组件。
 *
 * 为什么视图要以 span 传进来:提交点必须把所有视图的 RGResourceState 收走
 * (它们是按旧图分配的),但**视图的所有权属于场景**,不属于图缓存。传 span 而
 * 不是让本类持有视图,是为了让"谁拥有视图"这件事保持唯一答案。
 *
 * 线程:与 RenderScene 一致 —— 仅渲染线程。
 */

#include <lux/engine/function/render/client/RenderTargetLayout.hpp>
#include <lux/engine/function/render/graph/RGForwardDecls.hpp>         // RGResourceHandle
#include <lux/engine/function/render/client/core/RenderErrorEvent.hpp> // kNoScene
#include <lux/engine/render/core/GraphInvalidation.hpp>

#include <cstdint>
#include <deque>
#include <iosfwd>
#include <memory>
#include <functional>
#include <span>
#include <string>

namespace lux::render
{
    class RenderContext;
    class RGBuilder;
    class RGVulkanRecorder;
    class RGVulkanResourceAllocator;
    class SceneDomainDescriptorSets;
    struct RGCompiledGraph;
    struct RGGraphDescription;
    struct RGResourceState;
    struct View;

    // ─────────────────────────────────────────────────────────────────────
    //  SceneGraphState — 编译图 + 重编所需的旁路状态
    // ─────────────────────────────────────────────────────────────────────
    struct SceneGraphState
    {
        std::unique_ptr<RGCompiledGraph> graph;
        RGResourceHandle final_color_handle{};
        bool valid{false};
        RenderTargetLayout last_layout; ///< stored for recompilation
        /// 上次因 format_key 差异重编时的帧序号。同帧内二次 format_key 重编 =
        /// 多层 target 格式异构。
        ///
        /// (原先这里还有 render_skip_warned / format_key_thrash_warned 两个
        ///  「说过一次就不再说」的标志。它们随对应的 cerr 一起删掉了:上报通道
        ///  按同键合并计数,持续发生的问题会一直带着累加的 occurrences 出现,
        ///  而 warned-once 会让它在诊断面板上看着像已经好了。)
        uint64_t last_format_key_compile_serial{0};
    };

    // ─────────────────────────────────────────────────────────────────────
    //  SceneGraphCache
    // ─────────────────────────────────────────────────────────────────────
    class SceneGraphCache
    {
    public:
        /// @param debug_name 场景调试名;本类所有诊断(编译失败、布局告警、转储)
        ///                   都要点名场景,故构造时取一次,不必逐调用传。
        SceneGraphCache(RenderContext& ctx, std::string debug_name);

        ~SceneGraphCache();

        /// 所属场景的槽位下标,由 RenderScene::setSceneId 回填 —— 图编译诊断经
        /// 自发上报通道出去时要说清是**哪个**场景的图。
        void setSceneIndex(std::uint32_t index) noexcept
        {
            scene_index_ = index;
        }

        SceneGraphCache(const SceneGraphCache&) = delete;
        SceneGraphCache& operator=(const SceneGraphCache&) = delete;
        SceneGraphCache(SceneGraphCache&&) = delete;
        SceneGraphCache& operator=(SceneGraphCache&&) = delete;

        // ── 访问器(RenderScene 一行转发,故 Renderer/comm 零改动) ──────
        [[nodiscard]] SceneGraphState& state() noexcept
        {
            return state_;
        }
        [[nodiscard]] const SceneGraphState& state() const noexcept
        {
            return state_;
        }
        [[nodiscard]] RGVulkanResourceAllocator& allocator() noexcept;
        [[nodiscard]] RGVulkanRecorder& recorder() noexcept;

        /// 标记为失效,下一帧重编。
        void invalidate(EGraphInvalidationReason reason) noexcept;

        [[nodiscard]] const SceneGraphTelemetry& telemetry() const noexcept
        {
            return telemetry_;
        }
        [[nodiscard]] const std::deque<SceneGraphCompileSample>& compileHistory() const noexcept
        {
            return compile_history_;
        }

        // ── 编译 ────────────────────────────────────────────────────────
        /**
         * @brief 依据目标布局(重)编译本场景的图模板。
         *
         * 事务式 build-then-commit:先在局部把新图编出来,**完全不触碰**当前在用的
         * 图与视图资源;编译失败则原样返回,场景继续用上一份好图(旧实现曾先退休旧图
         * 再编,失败即黑屏)。
         *
         * @param contribute_passes  把本场景该出的 pass 声明进给定 builder。
         *                    本类**不认识特性** —— 它只要求"往这个 builder 里
         *                    声明 pass",谁去声明、按什么顺序,是场景的事。
         * @param all_views   本场景的全部视图;提交点会收走它们按旧图分配的
         *                    RGResourceState。视图所有权同样属于场景。
         */
        using PassContribution = std::function<void(RGBuilder&)>;

        void compile(
            const RenderTargetLayout& layout,
            std::uint32_t required_target_slots,
            const PassContribution& contribute_passes,
            std::span<View* const> all_views,
            SceneDomainDescriptorSets* domain_sets,
            std::uint64_t frame_serial
        );

        // ── 退休 ────────────────────────────────────────────────────────
        /// 退休一份按 @p source_graph 分配的每视图资源(Renderer 在换图/改尺寸时调用)。
        void retireViewResourceState(RGResourceState&& state, const RGGraphDescription* source_graph);

        /// 每帧老化回收。**顺序敏感**:必须先回收视图资源再回收图 ——
        /// RetiredViewResources::source_graph 指向那些图。
        ///
        /// @param frame_id         首次见到的条目用它打戳(其最后一次 GPU 使用的上界)。
        /// @param completed_serial **栅栏证实**的完成水位(FrameDriver::gpuCompletedSerial)。
        ///                         条目的戳记被它越过才释放。原实现用
        ///                         "frame_id - retire_frame >= fif" 算术,会高估完成度
        ///                         —— 序号在不提交的 tick 上照样前进。
        void collectRetired(uint64_t frame_id, uint64_t completed_serial);

        /// 把当前编译图的可读转储写入 @p os(未编译时写一行说明)。
        void dump(std::ostream& os) const;

        /// 显式清理(顺序敏感);析构亦调用,可重复调用。
        void shutdown();

    private:
        /// 收走所有视图的 RGResourceState 进退休队列,配上它们所属的图描述。
        void retireAllViewResources(std::span<View* const> views, const RGGraphDescription* source_graph) noexcept;

        struct RetiredGraph
        {
            std::unique_ptr<RGCompiledGraph> graph;
            uint64_t retire_frame{0};
        };

        /// 延迟的每视图资源清理(物理图像 + 录制上下文)。
        struct RetiredViewResources
        {
            std::unique_ptr<RGResourceState> state;
            uint64_t retire_frame{0};
            /// 分配这些资源时所用的图描述,归还池(deallocateToPool)需要它。
            /// 生命期安全:collectRetired 里视图资源**先于**图被处理,故指针有效。
            const RGGraphDescription* source_graph{nullptr};
        };

        RenderContext& ctx_;
        std::string debug_name_;
        std::uint32_t scene_index_{RenderErrorEvent::kNoScene};
        std::unique_ptr<RGVulkanResourceAllocator> allocator_;
        std::unique_ptr<RGVulkanRecorder> recorder_;
        SceneGraphState state_;
        std::deque<RetiredGraph> retired_graphs_;
        std::deque<RetiredViewResources> retired_view_resources_;
        SceneGraphTelemetry telemetry_{};
        std::deque<SceneGraphCompileSample> compile_history_;
    };

} // namespace lux::render
