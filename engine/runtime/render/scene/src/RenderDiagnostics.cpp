#include <lux/engine/runtime/render/scene/RenderDiagnostics.hpp>

#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/core/RenderErrorEvent.hpp>
#include <lux/engine/function/render/client/core/RenderErrorRegistry.hpp>   // formatRenderError / renderErrorRegistry
#include <lux/engine/ecs/render/RenderBridgeDiagnostics.hpp>
#include <lux/engine/events/DomainEvents.hpp>
#include <lux/engine/log/Log.hpp>

#include <string>
#include <string_view>

namespace lux::runtime
{
    namespace
    {
        /// 日志级别取自错误类型自己声明的 **ERecovery**,而不是一律 Error。
        ///
        /// 这不是省事:自发上报通道里绝大多数是**告知性**的诊断 —— 实测一次编辑器
        /// 会话就有 `frame.shadow_atlas_assignment_churn`(它的文档注释自己写着
        /// "偶发一次是正常的场景变化")、`frame.target_layout_amended` 这类。全部
        /// 记成 Error,编辑器的 toast 会被它们淹没,真正的错误反而看不见 ——
        /// "让错误到达用户"就又变回了"让用户学会忽略"。
        ///
        /// 判据用 ERecovery 而不是新开一个 severity 字段:它已经在每个错误类型上
        /// 声明了,而且问的正是同一个问题 —— **这件事该由谁处理**。
        ///   Bug / Permanent → 出问题了,得有人看        → Error
        ///   NeedsInput      → 输入/配置要改,不是故障    → Warn
        ///   Retryable       → 瞬时,稍后可能自己就好了   → Warn
        [[nodiscard]] lux::log::ELevel levelFor(const lux::render::RenderErrorRegistry& registry,
                                                const lux::render::RenderError&         error)
        {
            const auto desc = registry.find(error.type);
            if (!desc)
                return lux::log::ELevel::Error;   // 认不出的 id:宁可吵一点

            // The validation bridge deliberately carries the foreign Vulkan
            // severity in arg0 because one stable diagnostic type represents
            // info, performance warnings and real validation errors. Recovery
            // remains Bug for policy/telemetry, but the human log outlet must
            // not print severity=1 performance advice with an `[error]` tag.
            if (std::string_view{desc->name} == "validation.layer_report")
            {
                switch (error.args[0])
                {
                case 0: return lux::log::ELevel::Info;
                case 1: return lux::log::ELevel::Warn;
                default: return lux::log::ELevel::Error;
                }
            }

            switch (desc->recovery)
            {
            case lux::render::ERecovery::Bug:
            case lux::render::ERecovery::Permanent:
                return lux::log::ELevel::Error;
            case lux::render::ERecovery::NeedsInput:
            case lux::render::ERecovery::Retryable:
            default:
                return lux::log::ELevel::Warn;
            }
        }
    } // namespace

    void installRenderErrorLogging(lux::render::RenderFrameSession&     session,
                                   lux::events::DomainEvents&          bus,
                                   lux::events::EventPump&         pump,
                                   lux::events::SubscriptionGroup& subs)
    {
        // 发布侧(事件批C):处理器只把 comm 层送达的 POD 翻译成事件 ——
        // 组装层是唯一的翻译处(裁决③);解析/定级/打日志在订阅侧。
        session.setErrorEventHandler(
            [b = &bus](const lux::render::ErrorEventBatchReply& batch)
            {
                if (batch.dropped > 0)
                    b->publish(RenderErrorsDropped{batch.dropped, batch.count});
            },
            [b = &bus](const lux::render::RenderErrorEvent& event)
            {
                b->publish(RenderErrorRaised{event});
            });

        // 订阅侧:格式化 + 定级 + 打日志(帧泵上执行)。
        subs.add(bus.subscribe<RenderErrorsDropped>(
            pump,
            [](const RenderErrorsDropped& e)
            {
                // dropped 必须显示出来:它是"这条通道自己也在丢东西"的唯一证据。
                lux::log::warn("render",
                               "上报环满,丢弃 {} 条诊断事件(本批送达 {} 条)",
                               e.dropped, e.delivered);
            }));
        subs.add(bus.subscribe<RenderErrorRaised>(
            pump,
            [](const RenderErrorRaised& raised)
            {
                const auto& event    = raised.event;
                auto&       registry = lux::render::renderErrorRegistry();
                // 名字在**消费侧**才被解析出来 —— 事件里带的是 id + 实参槽。
                const std::string text = formatRenderError(registry, event.error);

                // occurrences 是通道合并的次数(限流是通道的职责,不是每个失败点
                // 各写一份);scene_index 只在与场景相关时才有意义。
                const lux::log::ELevel level = levelFor(registry, event.error);
                if (event.scene_index == lux::render::RenderErrorEvent::kNoScene)
                    lux::log::logf(level, "render", "{} [×{}, frame {}]",
                                   text, event.occurrences,
                                   static_cast<unsigned long long>(event.frame_serial));
                else
                    lux::log::logf(level, "render", "{} [scene {}, ×{}, frame {}]",
                                   text, event.scene_index, event.occurrences,
                                   static_cast<unsigned long long>(event.frame_serial));
            }));
    }

    void reportUnroutedRenderReplies(const lux::render::RenderFrameSession& session)
    {
        const std::uint64_t n = session.unroutedUnsolicitedReplies();
        if (n == 0)
            return;
        lux::log::warn("render",
                       "{} 条渲染线程自发上报的回复无人接收 —— 装配期漏了 "
                       "installRenderErrorLogging",
                       static_cast<unsigned long long>(n));
    }

    void installRenderBridgeLogging()
    {
        lux::ecs::setRenderBridgeDiagnosticSink(
            [](std::string_view text)
            {
                lux::log::warn("render-bridge", "{}", text);
            });
    }

} // namespace lux::runtime
