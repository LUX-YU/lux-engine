#pragma once
/**
 * @file AssetRef.hpp
 * @brief 驻留兴趣的 RAII 票据：持有 = retain，析构/重置 = release。
 *
 * 账本(AssetManager 的引用计数)此前靠调用方手工配平 retain/release —— 每一处
 * 都要自己维护「先钉新再放旧」「离场必还」的纪律,漂账只在泄漏统计里可见。
 * AssetRef 把这份纪律搬进类型系统:忘还票在语法上不可表达。
 *
 * **为什么不是 shared_ptr<Asset>**(设计稿裁决五,四条都对不上):
 *   1. 归零 ≠ 析构 —— 归零要**广播**给派生缓存与驱逐闸门,资产对象仍归注册表;
 *   2. 计数必须在 manager 的账本里(isReferenced 是驱逐闸门,要精确值);
 *   3. 归零动作必须落在主线程帧相位,deleter 跑在最后持有者的线程上;
 *   4. 引用的单位是跨 shell↔loaded↔重载稳定的 id,不是某一代对象指针。
 *
 * 线程契约:与账本一致 —— **只在主线程持有/拷贝/析构**。后台线程传 id,回主
 * 线程再 acquire(违约由 AssetManager 的 debug 断言当场抓)。
 *
 * 组件里怎么放:本类型**不进反射标注**。放进 LUX_COMPONENT 组件时字段必须加
 * LUX_NO_MEMBER()(生成器默认反射所有 public 成员);宿主组件保持 rule-of-zero,
 * 不得为它手写 operator=(生成器会反射 public operator=,ScriptComponent.hpp
 * 头注释记过这个坑)。非反射组件(AnimatorCacheComponent 等)无约束。
 */

#include <lux/engine/resource/identity/AssetId.hpp>
#include <lux/engine/resource/asset/visibility.h>

namespace lux::asset
{
    class AssetManager;

    class LUX_ASSET_PUBLIC AssetRef
    {
    public:
        AssetRef() = default;

        AssetRef(const AssetRef& other);              ///< 拷贝 = 再取一票(+1)
        AssetRef& operator=(const AssetRef& other);

        AssetRef(AssetRef&& other) noexcept
            : mgr_(other.mgr_), id_(other.id_)
        {
            other.mgr_ = nullptr;
            other.id_  = {};
        }
        AssetRef& operator=(AssetRef&& other) noexcept;

        ~AssetRef();

        /// 提前还票(幂等)。之后 empty()。
        void reset() noexcept;

        /// 这张票指向谁。空票返回 nil id —— resolver 的「还是不是同一个资产」
        /// 判据就用它,与裸 id 时代同形。
        [[nodiscard]] const asset_id_t& id() const noexcept { return id_; }

        [[nodiscard]] bool empty() const noexcept { return mgr_ == nullptr || id_.is_nil(); }
        explicit operator bool() const noexcept { return !empty(); }

    private:
        friend class AssetManager;   // 唯一的发票口:AssetManager::acquire
        AssetRef(AssetManager* mgr, const asset_id_t& id) noexcept
            : mgr_(mgr), id_(id) {}

        // 裸指针是刻意的:AssetManager 在测试里栈构造(enable_shared_from_this
        // 不可行,AssetManager.cpp 有注释),且三类宿主的拆解序都已核实为
        // 「消费者先死、manager 后死」。票比 manager 活得久 = 宿主装配 bug。
        AssetManager* mgr_{nullptr};
        asset_id_t    id_{};
    };
} // namespace lux::asset
