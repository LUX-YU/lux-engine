#pragma once
/**
 * @file RenderDiagnostics.hpp
 * @brief 渲染侧诊断的**出口装配** —— 把库产生的诊断接到 lux::log（§7.1 通道 ②）。
 *
 * 为什么是一个共享的装配函数、而不是每个宿主各写一遍:
 *
 * 库（render / ecs::render_core）刻意不决定文字打到哪 —— 这是 no_terminal_io
 * 门禁存在的理由。代价是"接线"成了宿主的义务,而**义务一旦分散就会漂**:
 * 渲染桥的诊断出口就已经漂过 —— 编辑器给了 stderr、game_host 给了 lux::log、
 * Android 一个都没装,于是同一条诊断在三个宿主里有三种命运。
 * 自发错误上报通道更彻底:`RenderFrameSession::setErrorEventHandler` 全仓**零调用点**,
 * 16 处 reportError 每帧发生、每帧打包、每帧被丢 —— 比 stderr 还差。
 *
 * 所以出口在这里定义一次,宿主只负责在装配期调用。放在 engine/runtime/render/scene 是
 * 因为它是唯一同时链接 render 客户端、ecs::render_core 与 core::log 的层,
 * 而三个宿主（编辑器 / player+Android / launcher）都在它之上。
 */

#include <lux/engine/runtime/render/scene/visibility.h>
#include <lux/engine/function/render/client/core/RenderErrorEvent.hpp>   // 事件载荷(POD)

#include <cstdint>

namespace lux::render { class RenderFrameSession; }
namespace lux::events
{
    class DomainEvents;
    class EventPump;
    class SubscriptionGroup;
}

namespace lux::runtime
{
    /// 渲染线程自发上报的一条错误(既发生事实;统一事件系统段C)。载荷是
    /// comm 层送达的 POD 原样 —— 名字/文本在**订阅端**才解析(错误注册表)。
    /// 定义在 runtime_render_scene:字段类型可见、且三宿主共用的最低层(条例②)。
    struct RenderErrorRaised
    {
        lux::render::RenderErrorEvent event;
    };

    /// 上报环满,渲染线程丢弃过诊断事件 —— 通道自己在丢东西的唯一证据。
    struct RenderErrorsDropped
    {
        std::uint32_t dropped;     ///< 环满丢掉的条数
        std::uint32_t delivered;   ///< 本批送达的条数
    };

    /// 把渲染线程**自发**上报的错误接到统一事件总线 + `lux::log`
    /// (category "render")。
    ///
    /// 事件批C 的形状:处理器只剩「发布」一件事(RenderErrorRaised /
    /// RenderErrorsDropped);解析错误名 + 定级 + 打日志变成 @p pump 上的
    /// 订阅者(订阅句柄进 @p subs)—— 三宿主的装配漂移面收敛成一次调用。
    ///
    /// 每个 RenderFrameSession 在装配期调用一次,且必须在第一次 pumpReplies 之前 ——
    /// 之前到达的事件因为没有处理器会被丢弃(并计入
    /// `RenderFrameSession::unroutedUnsolicitedReplies()`)。
    ///
    /// 处理器在**调用 pumpReplies 的那个线程**上回调(宿主主线程);publish
    /// 本身任意线程安全,不额外约束。
    LUX_RUNTIME_RENDER_SCENE_PUBLIC void installRenderErrorLogging(
        lux::render::RenderFrameSession&     session,
        lux::events::DomainEvents&          bus,
        lux::events::EventPump&         pump,
        lux::events::SubscriptionGroup& subs);

    /// 关闭期自证:若 @p session 丢过没人处理的自发回复,报一句。
    ///
    /// 恒 0 才是正常。非零只有一个含义 —— 某个宿主的装配期漏了
    /// installRenderErrorLogging。刻意不用 assert:实机跑的 RelWithDebInfo 带
    /// NDEBUG,assert 恰好在唯一重要的配置里消失。
    LUX_RUNTIME_RENDER_SCENE_PUBLIC void reportUnroutedRenderReplies(const lux::render::RenderFrameSession& session);

    /// 把 ecs 渲染桥的诊断文本接到 `lux::log`（category "render-bridge", warn 级）。
    ///
    /// 必须在任何桥开始工作**之前**装好 —— 之后只读（桥的回复延续跑在泵回复的
    /// 线程上,运行期改它就是数据竞争）。
    LUX_RUNTIME_RENDER_SCENE_PUBLIC void installRenderBridgeLogging();

} // namespace lux::runtime
