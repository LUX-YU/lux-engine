#pragma once
/**
 * @file RenderViewUtil.hpp
 * @brief 渲染子系统共用的三样小件:视图工具(ComponentList / componentView /
 *        inComponentView)+ 离场观察者(ComponentSetLeaveObserver)+ 实例变换 POD
 *        (InstanceTransform)。
 *
 * 前身是 EcsRenderTraits.hpp:那时每个可渲染组件特化一个 `EcsRenderTraits<C>`
 * (声明 KIND + 钩子),`makeRenderSubsystem<C>()` 按 kind 编译期派发到三个通用
 * 子系统模板。装配归属 ADR 裁决六把那层间接消灭了 —— 身份还给具体渲染功能:
 * 每个功能一个具名子系统类型(SkyboxSubsystem / PointLightSubsystem / …),
 * 由「策略结构体 + 形状基类别名」构成(FeatureParamSubsystem / PooledSlotSubsystem /
 * MeshInstanceSubsystem),`addSubsystem(std::make_unique<XxxSubsystem>())` 直接装。
 * 本头只留下当年 traits 头里**与派发无关**、各子系统仍共用的可复用件。
 */

#include <algorithm>
#include <array>
#include <functional>
#include <string_view>
#include <type_traits>
#include <vector>

#include <lux/engine/ecs/Registry.hpp>   // EntityRegistry / entt::exclude
#include <lux/engine/function/render/client/RenderControlSession.hpp>
#include <lux/engine/function/render/client/core/RenderSpatialTypes.hpp>

// ComponentList / componentView / ExtractionChangeSet —— 批 T2 下沉到 ecs/core。
#include <lux/engine/ecs/ComponentChangeSet.hpp>

namespace lux::ecs
{
    // ★ 批 T2:`ComponentList` / `componentView` / `ExtractionChangeSet` 已下沉到
    //   `ecs/core` 的 ComponentChangeSet.hpp —— 变换系统(在 core)成了第二个
    //   消费者,够不到 render。名字同在 `lux::ecs`,调用点一行不用改。

    /// Membership predicate for the same `componentView<C>(reg, Require, Exclude)` set,
    /// queried per-entity: true iff the entity is alive, has C plus every Require, and
    /// none of the Excludes (e.g. world-streaming's dormant tag). A subsystem never
    /// names a specific exclusion component; the policy's Exclude list is the single
    /// source of truth. (曾是 INSTANCE 桥每帧全扫 reap 的判据;全扫换成
    /// `ComponentSetLeaveObserver` 之后留作按实体查询的工具。)
    template <class C, class... Rs, class... Es>
    [[nodiscard]] inline bool inComponentView(lux::ecs::Registry& reg, lux::ecs::Entity e, ComponentList<Rs...>, ComponentList<Es...>)
    {
        if (!reg.valid(e) || !reg.template all_of<C, Rs...>(e)) return false;
        if constexpr (sizeof...(Es) > 0)
            if (reg.template any_of<Es...>(e)) return false;
        return true;
    }

    template <class C, class Req, class Exc> class ComponentSetLeaveObserver;

    template <class C, class... Rs, class... Es>
    class ComponentSetLeaveObserver<C, ComponentList<Rs...>, ComponentList<Es...>>
    {
        lux::ecs::Registry*                 reg_{nullptr};
        std::function<void(lux::ecs::Entity)>  on_leave_{};

        /// EnTT 信号传入 canonical EntityRegistryBase。
        void onLeft(
            lux::ecs::RegistryBase&,
            lux::ecs::Entity e)
        {
            if (on_leave_)
                on_leave_(e);
        }

    public:
        ComponentSetLeaveObserver() = default;
        ~ComponentSetLeaveObserver() { detach(); }

        ComponentSetLeaveObserver(const ComponentSetLeaveObserver&)            = delete;
        ComponentSetLeaveObserver& operator=(const ComponentSetLeaveObserver&) = delete;
        // 连接持有 `this`：移动会让信号指向搬走后的旧地址。禁掉。
        ComponentSetLeaveObserver(ComponentSetLeaveObserver&&)                 = delete;
        ComponentSetLeaveObserver& operator=(ComponentSetLeaveObserver&&)      = delete;

        void attach(lux::ecs::Registry& reg, std::function<void(lux::ecs::Entity)> cb)
        {
            if (reg_ == &reg) return;
            detach();
            reg_      = &reg;
            on_leave_ = std::move(cb);
            reg.template on_destroy<C>().template connect<&ComponentSetLeaveObserver::onLeft>(*this);
            (reg.template on_destroy  <Rs>().template connect<&ComponentSetLeaveObserver::onLeft>(*this), ...);
            (reg.template on_construct<Es>().template connect<&ComponentSetLeaveObserver::onLeft>(*this), ...);
        }

        void detach()
        {
            if (!reg_) return;
            reg_->template on_destroy<C>().template disconnect<&ComponentSetLeaveObserver::onLeft>(*this);
            (reg_->template on_destroy  <Rs>().template disconnect<&ComponentSetLeaveObserver::onLeft>(*this), ...);
            (reg_->template on_construct<Es>().template disconnect<&ComponentSetLeaveObserver::onLeft>(*this), ...);
            reg_ = nullptr;
        }
    };

    template <class C, class Req, class Exc> class ComponentSetChangeObserver;

    template <class C, class... Rs, class... Es>
    class ComponentSetChangeObserver<C, ComponentList<Rs...>, ComponentList<Es...>>
    {
        lux::ecs::Registry*                 reg_{nullptr};
        std::function<void(lux::ecs::Entity)>  on_change_{};

        void onChanged(
            lux::ecs::RegistryBase&,
            lux::ecs::Entity e)
        {
            if (on_change_)
                on_change_(e);
        }

    public:
        ComponentSetChangeObserver() = default;
        ~ComponentSetChangeObserver() { detach(); }

        ComponentSetChangeObserver(const ComponentSetChangeObserver&)            = delete;
        ComponentSetChangeObserver& operator=(const ComponentSetChangeObserver&) = delete;
        // 连接持有 `this`,移动即悬垂(同 LeaveObserver)。禁掉。
        ComponentSetChangeObserver(ComponentSetChangeObserver&&)                 = delete;
        ComponentSetChangeObserver& operator=(ComponentSetChangeObserver&&)      = delete;

        void attach(lux::ecs::Registry& reg, std::function<void(lux::ecs::Entity)> cb)
        {
            if (reg_ == &reg) return;
            detach();
            reg_       = &reg;
            on_change_ = std::move(cb);
            reg.template on_construct<C>().template connect<&ComponentSetChangeObserver::onChanged>(*this);
            reg.template on_update   <C>().template connect<&ComponentSetChangeObserver::onChanged>(*this);
            (reg.template on_construct<Rs>().template connect<&ComponentSetChangeObserver::onChanged>(*this), ...);
            (reg.template on_destroy  <Es>().template connect<&ComponentSetChangeObserver::onChanged>(*this), ...);
            // 折入存量:此刻已在集合里的实体一个不漏地补发。
            componentView<C>(reg, ComponentList<Rs...>{}, ComponentList<Es...>{}).each(
                [this](lux::ecs::Entity e, auto&&...) { on_change_(e); });
        }

        void detach()
        {
            if (!reg_) return;
            reg_->template on_construct<C>().template disconnect<&ComponentSetChangeObserver::onChanged>(*this);
            reg_->template on_update   <C>().template disconnect<&ComponentSetChangeObserver::onChanged>(*this);
            (reg_->template on_construct<Rs>().template disconnect<&ComponentSetChangeObserver::onChanged>(*this), ...);
            (reg_->template on_destroy  <Es>().template disconnect<&ComponentSetChangeObserver::onChanged>(*this), ...);
            reg_ = nullptr;
        }
    };

    struct InstanceTransform
    {
        lux::render::RenderSpatialTransform3D spatial{};
        bool                                  valid{false};
    };

    /// 归还一个**本节点自建**的全局贴图(Tilemap 的索引图、PixelField 的图集/调色板)。
    ///
    /// Destruction is control-plane work and is valid independently of frame
    /// admission. This makes the same release path safe from update, close and
    /// destructor fallback without a deferred "next beginFrame" queue.
    inline void releaseOwnedTexture(lux::render::RenderControlSession& control,
                                    lux::render::RTextureHandle handle) noexcept
    {
        if (handle.isNull()) return;
        control.destroyTexture(handle);
    }

    [[nodiscard]] inline bool isRenderUploadBackpressure(
        lux::render::ERenderUploadSubmitError error) noexcept
    {
        return error == lux::render::ERenderUploadSubmitError::QUEUE_FULL
            || error == lux::render::ERenderUploadSubmitError::
                BYTE_BUDGET_EXHAUSTED;
    }
} // namespace lux::ecs
