#pragma once
/**
 * @file RenderExtractionResources.hpp
 * @brief 渲染抽取节点共用的**窄类型化资源**。
 *
 * 收口之后,子系统之间不能再靠「谁持有谁的指针」通气(`CameraViewSubsystem` 曾经握着
 * 一个 `RenderSystem*`,只为在 view 落地时通知它)。共享的那点状态改为**独立的、
 * 有名字的资源**:生产者和消费者都只认识资源,彼此不认识。
 *
 * 资源归场景所有(`SceneServices`),生命周期比任何一个节点长 —— 所以节点持有它的
 * 引用是安全的,而持有彼此的指针不是(节点可以被摘掉)。
 *
 * 这里只放**已经有真实消费者**的那些。设计稿列了一串(ActiveRenderScene /
 * RenderFeatureBindings / ResidencyView / …),但一次性把它们都建出来只会得到一堆
 * 零消费者的空壳 —— 每个都等到它那一批有人读时再加。
 */

#include <lux/engine/function/render/client/core/FeatureHandle.hpp>   // lux::render::ViewHandle

#include <cstdint>

namespace lux::ecs
{
    /// 「本场景当前出图的那个 view」。
    ///
    /// 写者:`CameraViewSubsystem`(某台相机的 addView 回执落地 / 绑定消失)。
    /// 读者:各抽取节点(实例可见性、相机上传)。
    ///
    /// ⚠️ **代次不是装饰品。** 有些 per-view 状态是「置一次就不再看」的闩 ——
    ///    `MeshInstanceSubsystem` 的实例可见性就是(`makeInstanceVisibleForView`
    ///    每个实例只发一次)。view 一换,那些闩仍记着「已对**旧** view 可见」,于是
    ///    新 view 里一个网格都不会出现:**不崩、不报错、测试也抓不到**(gpu 测试
    ///    不走编辑器 Play 切换)。代次让持有者用「!= 当前代次」代替「== false」来
    ///    判,换 view 时自然重发一遍。新增任何 per-view 的一次性状态,都要按代次记。
    class ActiveRenderView final
    {
    public:
        ActiveRenderView() = default;

        /// 装配期就知道 view 的场合(手工驱动的 demo/探针)。这样交来的那个 view
        /// 就是**第 1 代**,与默认构造后再 setView 的语义一致。
        explicit ActiveRenderView(lux::render::ViewHandle v) noexcept : view_(v) {}

        [[nodiscard]] lux::render::ViewHandle view() const noexcept { return view_; }

        /// 当前 view 的代次。**真实代次从 1 起**,所以持有者用 0 做「从未发过」的
        /// 初值,与任何真实代次都不等。
        [[nodiscard]] std::uint32_t generation() const noexcept
        {
            return generation_;
        }

        /// 换掉出图的 view。幂等:同一个 view 重复设不该让实例白重发。
        void setView(lux::render::ViewHandle v) noexcept
        {
            if (v == view_) return;
            view_ = v;
            ++generation_;
        }

    private:
        lux::render::ViewHandle view_{};
        std::uint32_t           generation_{1};
    };

} // namespace lux::ecs
