#pragma once
#include <lux/engine/ecs/SystemUpdateContext.hpp>
#include <lux/engine/ecs/TypeToken.hpp>
#include <lux/engine/meta/LuxObject.hpp>

#include <cstdint>
#include <span>

namespace lux::ecs
{
    class World;

    /// Allocation-free wakeup used by a system whose bounded close is waiting
    /// for an external completion.  The composition root owns `context` and
    /// must outlive every installed system until closeComplete() is true.
    /// Stateless and owner-tick-only systems may ignore the sink.
    struct SystemCloseProgressSink final
    {
        void* context{nullptr};
        void (*notify_fn)(void*) noexcept{nullptr};

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return context != nullptr && notify_fn != nullptr;
        }

        void notify() const noexcept
        {
            if (*this)
                notify_fn(context);
        }
    };

    using SystemType = TypeToken;

    template <class System>
    [[nodiscard]] constexpr SystemType systemType() noexcept
    {
        return typeToken<System>();
    }

    [[nodiscard]] constexpr bool sameSystemType(
        SystemType lhs, SystemType rhs) noexcept
    {
        return sameTypeToken(lhs, rhs);
    }

    /// Base class for all gameplay systems.
    ///
    /// ── 顺序怎么表达 ────────────────────────────────────────────────────────
    ///
    /// 两级，粗的那级是**相位**（`addSystem` 的参数，见 `SystemPhase.hpp`），细的
    /// 那级是下面这三个声明：一个系统可以说出「我要在 X 之后 / 之前」，X 由
    /// `systemType<X>()` 标识。`Schedule` 把它们拓扑排序，**只在装配变动时排一次**。
    ///
    /// 为什么两级都要：相位回答的是「这一步属于帧的哪个阶段」（输入 → 变换 → 模拟
    /// → 渲染），一个新系统作者答得上来；而「我要紧跟在某个具体系统后面」是**局部
    /// 的、成对的**知识，用相位数字表达就会变成 `kPhasePreTransform + 10` 这种东西
    /// —— 那个 `+10` 曾经真的存在（编辑器相机系统要压过 `CameraViewSubsystem` 的
    /// syncAspect），它把一条二元约束编码成了一个全局坐标，谁再插一个 `+5` 就静默
    /// 打破它。
    ///
    /// 精度：显式的 before/after **压过**相位。两者矛盾时（A 在 B 之后，而 A 的相位
    /// 更早）以显式声明为准 —— 显式的那条是作者写下的意图，相位只是默认值。
    ///
    /// 这些声明与 access/removal capability 都是**装配描述**，不是
    /// 运行期状态。Schedule 在任何 `onAdded` 之前把整批描述冻结进稳定 slot；之后
    /// 修改系统成员不会改变拓扑，也不会让预检与执行看到两张不同的图。
    ///
    /// 引用一个不存在的类型、或者声明出环，都会被 `Schedule::compile()` 报出来
    /// （装配期一次，不是每帧）。**不要**指望它们静默生效。
    class ISystem : public IEcsCommandProducer
    {
    public:
        virtual ~ISystem() = default;

        // ── 生命周期(驻留重构批1,设计稿 J5)──────────────────────────
        //
        // Schedule 是系统容器与拥有者:`Schedule::addSystem` 收下系统后**立即**调
        // `onAdded` —— 系统在这里注册自己的观察者/回调,并**折入存量**
        // (处理世界里已存在的相关组件;新装配时存量为空,动态激活时不为空
        //  —— 两种情形同一条路径,J9-1)。调用时机由世界担保,系统作者只
        // 回答「被加进来时做什么」,不操心「什么时候被加进来」—— 这正是
        // 懒初始化 ensure 形状的反面(J7)。
        //
        // `onRemoved` 是从**活着的世界**里摘除时的逆过程(注销世界级表项
        // 等;订阅经 RAII 成员自然退)。它只拿 `SystemRemovalContext`,
        // 没有一个会随 owner 立即失效的 command writer。世界整体拆解
        // **不**逐个调它 ——
        // 那时一切随析构走,两种语义不混；Schedule 必须先于 World 析构。
        //
        // 形参是 `SystemSetupContext` 而不是裸 `World&`:连信号时要**折入存量**,
        // 而折入存量是一批结构命令,得有地方写。上下文把本系统的 command writer
        // 一并交进来,系统存下它,之后装的观察者在任意时刻都能入队。

        /// addSystem 收下本系统后立即调(装配期/安全点;不得在 tick 中)。
        virtual void onAdded(const SystemSetupContext& /*setup*/) {}

        /// removeSystem 摘除前调(同上时机约束)。
        virtual void onRemoved(const SystemRemovalContext& /*removal*/) {}

        /// Dynamic removal is opt-in. Most systems borrow scene services or
        /// own remote scene state whose lifetime is closed by normal Schedule
        /// teardown. Opt-in means `onRemoved()` fully detaches the system;
        /// Schedule then destroys it immediately at the same safe point.
        [[nodiscard]] virtual bool supportsDynamicRemoval() const noexcept
        {
            return false;
        }

        /// Begin a bounded, owner-thread close. Implementations stop admitting
        /// new work here, but continue using ordinary update/barrier safe
        /// points until closeComplete() becomes true. The default describes a
        /// stateless system which is already quiescent. Schedule-wide close
        /// and a containing contribution may both request the same system;
        /// implementations must therefore be idempotent and retain at most
        /// one progress sink/subscription.
        virtual void requestClose() noexcept {}
        /// The sink is deliberately a narrow POD instead of a sender/runtime
        /// dependency: systems which own an AsyncScope can retain it and wake
        /// Scene close when that scope reaches its terminal state.  Existing
        /// synchronous implementations only override requestClose().
        virtual void requestClose(SystemCloseProgressSink /*progress*/)
            noexcept
        {
            requestClose();
        }
        [[nodiscard]] virtual bool closeComplete() const noexcept
        {
            return true;
        }
        /// True only when another Schedule tick can make synchronous owner
        /// progress. False means the system is either complete or waiting for
        /// an external completion which will wake the composition root.
        [[nodiscard]] virtual bool closeNeedsOwnerTick() const noexcept
        {
            return false;
        }

        using Type         = SystemType;
        using ResourceType = TypeToken;

        enum class AccessMode : std::uint8_t
        {
            Read,
            Write,
        };

        struct ResourceAccess
        {
            ResourceType resource{};
            AccessMode mode{AccessMode::Read};
        };

        /// A complete declaration allows the schedule compiler to place this
        /// system beside non-conflicting systems in a future parallel batch.
        /// The conservative default is incomplete/exclusive. `structural`
        /// covers entity creation/destruction and component add/remove, which
        /// cannot be inferred from component read/write tokens alone.
        struct AccessDeclaration
        {
            std::span<const ResourceAccess> resources{};
            bool                            complete{false};
            bool                            structural{true};
        };

        template <class Resource>
        [[nodiscard]] static constexpr ResourceAccess reads() noexcept
        {
            return ResourceAccess{typeToken<Resource>(), AccessMode::Read};
        }

        template <class Resource>
        [[nodiscard]] static constexpr ResourceAccess writes() noexcept
        {
            return ResourceAccess{typeToken<Resource>(), AccessMode::Write};
        }

        /// 本系统的前置依赖(按类型引用):这些系统必须**也在**同一 Schedule,
        /// 否则 `Schedule::compile()` 把缺失报进 SortReport(装配末尾一次,
        /// 响亮失败而非静默跳过 —— J5)。与 runsAfter 的区别:runsAfter 只
        /// 定顺序、引用缺失仅约束不生效;prerequisites 是存在性断言。
        [[nodiscard]] virtual std::span<const Type> prerequisites() const noexcept { return {}; }

        /// Called once per frame by Schedule::tick().
        ///
        /// 形参是窄上下文而不是裸 `registry + dt`:结构修改要有地方写(命令分片)、
        /// 诊断要认得出是第几个 tick,而这两样都不该让每个系统自己想办法。
        /// 上下文**不**提供帧状态 —— 系统不知道帧是否开着,也不 pump/submit。
        virtual void update(const SystemUpdateContext& ctx) = 0;

        /// 我必须跑在这些系统**之后** / **之前**（强类型引用）。
        [[nodiscard]] virtual std::span<const Type> runsAfter() const noexcept { return {}; }
        [[nodiscard]] virtual std::span<const Type> runsBefore() const noexcept { return {}; }

        /// Inspectable component/resource access contract. Unknown contracts
        /// remain exclusive; this prevents optimistic parallelism from racing
        /// through an undeclared EnTT access.
        [[nodiscard]] virtual AccessDeclaration accessDeclaration() const noexcept
        {
            return {};
        }

    };

} // namespace lux::ecs
