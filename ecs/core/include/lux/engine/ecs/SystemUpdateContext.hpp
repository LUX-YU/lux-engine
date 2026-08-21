#pragma once
/**
 * @file SystemUpdateContext.hpp
 * @brief 系统每帧/装配期看到的那扇窄窗口。
 *
 * 此前 `ISystem::update` 的形参是**裸 registry + dt**。裸 registry 说不出三件事,
 * 而这三件恰好是重构要立住的:
 *
 *  · 结构修改往哪写(观察者不得就地改世界,一律入队 —— 见 `EcsCommandBuffer.hpp`);
 *  · 这一帧是第几帧(诊断与「本帧只做一次」的判据,不必每个系统自己数);
 *  · 谁在发这条命令(生产者身份由 writer 钉死,不靠闭包捕获)。
 *
 * 这不是「再加一个万能上下文」。它只有四个成员,而且都是**借用**:世界的引用、
 * 本系统的 command writer、dt、tick 序号。它不认识渲染、不认识资产、不认识帧是否
 * 开着 —— 系统本来就不该知道后者(设计稿 §3 不变量 3)。
 */

#include <lux/engine/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/ecs/World.hpp>

#include <cstdint>

namespace lux::ecs
{
    /// 装配期(`onAdded`)看到的窗口。
    ///
    /// 为什么这里也要给 writer:系统在 `onAdded` 里连信号并**折入存量**,而折入
    /// 存量本身就是一批命令。它必须在第一次 `update` 之前就能入队 —— 装配收官
    /// (bring-up 的 settle)会紧接着排一次 barrier 把它们发出去。
    /// 与 `SystemUpdateContext` 同理拿 registry 而不是 `World&`:手工驱动的探针与
    /// 可视化 demo 只有一个裸 registry,而全仓零处用得上 World 的其余 API。
    class SystemSetupContext final
    {
    public:
        SystemSetupContext(lux::ecs::Registry& registry,
                           EcsCommandWriter commands) noexcept
            : registry_(&registry), commands_(commands)
        {
        }

        [[nodiscard]] lux::ecs::Registry& registry() const noexcept
        {
            return *registry_;
        }

        [[nodiscard]] const EcsCommandWriter& commands() const noexcept
        {
            return commands_;
        }

    private:
        lux::ecs::Registry* registry_{nullptr};
        EcsCommandWriter           commands_{};
    };

    /// 动态摘除期(`onRemoved`)的窄窗口。
    ///
    /// `Schedule::removeSystem` 在安全点同步调用 `onRemoved`，返回后
    /// 立即销毁节点并推进其 generation。因此摘除期不能暴露
    /// command writer：以该节点为 producer 的延迟命令在 barrier 时必然
    /// 已是 stale。需要的本地清理必须在这个安全点同步完成；
    /// 跨边界资源则应由显式 lease/owner 在更高层先行关闭。
    class SystemRemovalContext final
    {
    public:
        explicit SystemRemovalContext(
            lux::ecs::Registry& registry
        ) noexcept
            : registry_(&registry)
        {
        }

        [[nodiscard]] lux::ecs::Registry& registry() const noexcept
        {
            return *registry_;
        }

    private:
        lux::ecs::Registry* registry_{nullptr};
    };

    /// `ISystem::update` 的唯一形参。
    ///
    /// 拿的是 registry 而不是 `World&`:窄一级,而且**手工驱动的可视化 demo 与测试
    /// 拿得出来** —— 它们只有一个裸 registry,没有 World、没有 Schedule。多一层
    /// `World` 只换来一次 `.registry()` 转发,没有任何系统用得上 World 的其余 API。
    class SystemUpdateContext final
    {
    public:
        SystemUpdateContext(lux::ecs::Registry& registry,
                            EcsCommandWriter commands,
                            float dt, std::uint64_t tick_index) noexcept
            : registry_(&registry), commands_(commands), dt_(dt),
              tick_index_(tick_index)
        {
        }

        /// 手工驱动一个系统(测试 / 可视化 demo:没有 Schedule,也就没有命令分片)。
        /// 这样构造出来的上下文里 `commands()` 是**无效 writer** —— 系统若真去入队,
        /// 会在入队处响亮报错,而不是把命令静默丢掉。
        SystemUpdateContext(lux::ecs::Registry& registry, float dt) noexcept
            : registry_(&registry), dt_(dt)
        {
        }

        [[nodiscard]] lux::ecs::Registry& registry() const noexcept
        {
            return *registry_;
        }

        [[nodiscard]] float dt() const noexcept { return dt_; }

        /// 自本 `Schedule` 建立以来的 tick 计数,从 0 起。诊断与「本帧只做一次」
        /// 的判据;不是渲染帧号(系统看不见帧)。
        [[nodiscard]] std::uint64_t tickIndex() const noexcept
        {
            return tick_index_;
        }

        [[nodiscard]] const EcsCommandWriter& commands() const noexcept
        {
            return commands_;
        }

    private:
        lux::ecs::Registry* registry_{nullptr};
        EcsCommandWriter           commands_{};
        float                      dt_{0.0f};
        std::uint64_t              tick_index_{0};
    };

} // namespace lux::ecs
