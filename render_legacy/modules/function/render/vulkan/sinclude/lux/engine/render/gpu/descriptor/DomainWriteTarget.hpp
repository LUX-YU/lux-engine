#pragma once
// ============================================================================
//  DomainWriteTarget.hpp — 域写目标:句柄组 + 域内偏移,绑成一个不可分割的对象
//
//  机制退休后,域集是描述符的**唯一写目标**。每个资源写自己那一段时,
//  binding 号必须加上本 set 在合并域集里的偏移:
//
//      dstBinding = domain_binding_offset_ + <canonical binding>
//
//  这里真正的风险**不是组装错 VkWriteDescriptorSet**(那种错 validation 层
//  会当场抓),而是**拿了句柄却忘了加偏移** —— 写进去的是合法描述符,只是落在
//  邻居的 binding 上。没有任何一层会报错。
//
//  EVSM 的 b9-b10 就这么丢过一次(`6a0a3c0`:只写 legacy 不写域集,阴影静默
//  消失)。此前六个所有者各自持有 `domain_sets_` + `domain_binding_offset_`
//  两个裸成员、各自相加,谁漏一次谁静默出错。
//
//  把两者收进一个对象后,取 set 必须经 setFor()、算 binding 必须经 binding(),
//  **拿到句柄却没有偏移这件事在结构上不再可能**。
// ============================================================================
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/function/visibility.h>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace lux::render
{
    /// 逐 slice 的域集句柄 + 本 set 的域内 binding 偏移。
    class DomainWriteTarget
    {
    public:
        /// 接下写目标。
        ///
        /// 空 span 与「全是 NULL 句柄」是同一种失效:两者都让写入循环一条也发不出去,
        /// 而那意味着这个资源的描述符从未被写过 —— 绑定它是未定义行为,却没有任何一层
        /// 会报错。所以这里返回错误而不是就地报警:装配路径的调用方能决定是整体失败,
        /// 还是先修上游的域 set 分配。
        [[nodiscard]] Expected<void> set(std::span<const VkDescriptorSet> sets, uint32_t binding_offset) noexcept
        {
            sets_.assign(sets.begin(), sets.end());
            binding_offset_ = binding_offset;

            const bool usable = std::ranges::any_of(sets_, [](VkDescriptorSet s) { return s != VK_NULL_HANDLE; });
            if (!usable)
                return renderFailure<err::descriptor::DomainWriteTargetEmpty>();
            return {};
        }

        /// 该 slice 的域集;越界或未配置返回 VK_NULL_HANDLE(调用方按"没有"处理)。
        [[nodiscard]] VkDescriptorSet setFor(uint32_t slice) const noexcept
        {
            return slice < sets_.size() ? sets_[slice] : VK_NULL_HANDLE;
        }

        /// canonical binding → 域内 binding。**唯一**该做这个加法的地方。
        [[nodiscard]] uint32_t binding(uint32_t canonical) const noexcept
        {
            return binding_offset_ + canonical;
        }

        [[nodiscard]] uint32_t sliceCount() const noexcept
        {
            return static_cast<uint32_t>(sets_.size());
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return sets_.empty();
        }

    private:
        std::vector<VkDescriptorSet> sets_;
        uint32_t binding_offset_{0};
    };

} // namespace lux::render
