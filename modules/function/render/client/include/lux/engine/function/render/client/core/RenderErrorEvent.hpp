#pragma once
/**
 * @file RenderErrorEvent.hpp
 * @brief 渲染线程自发上报的一条失败,以及它的批头。
 *
 * 为什么需要它:返回路径只覆盖「有人在等结果」的失败。渲染线程上还有一大类失败**没有
 * 调用方可以交待** —— 一帧已经在飞了(管线变体预算耗尽、蒙皮骨骼池满、绑定拿不到布局)、
 * 或者根本不是谁请求的(Vulkan 校验层回调、交换链实际选中的呈现模式)。这类失败此前一律
 * 写 stderr,于是渲染库替上层决定了「这条诊断要出现在哪个终端」,而编辑器想把它显示在自己
 * 的面板里则无从下手。
 *
 * 事件带的就是返回路径上的**同一个 `RenderError`**:类型的静态说明(名字 / 消息模板 /
 * 恢复建议 / 实参语义)全在注册表里,消费方查表即可(RenderErrorRegistry.hpp);事件本身
 * 只额外带发生元数据。同一次失败在两条路径上是同一个值 —— 不会出现「返回值里看得到是哪个
 * binding,上报里看不到」这种不对称。
 *
 * 热路径零字符串、零分配:渲染线程只写 POD,格式化只在消费侧发生一次。
 */

#include <lux/engine/function/render/client/core/RenderError.hpp>

#include <cstdint>
#include <limits>
#include <type_traits>

namespace lux::render
{
    /// 一条自发上报。
    struct RenderErrorEvent
    {
        RenderError   error{};
        /// 关联场景的槽位下标;与场景无关时为 kNoScene。
        std::uint32_t scene_index{0};
        /// 本批内同类合并的次数。取代了各资源类里手写的幂次限流 ——
        /// 限流是通道的职责,不是每个失败点各写一份。
        std::uint32_t occurrences{1};
        std::uint64_t frame_serial{0};
        /// 全局单调序号。客户端比对相邻两批的 seq 即可知道自己漏收了多少。
        std::uint64_t seq{0};

        static constexpr std::uint32_t kNoScene = (std::numeric_limits<std::uint32_t>::max)();
    };
    static_assert(std::is_trivially_copyable_v<RenderErrorEvent>);

    /// 一批上报的头。事件本体作为独立的回复记录跟在同一帧里。
    struct ErrorEventBatchReply
    {
        /// 恒 ok() —— 这个回复本身不是某次操作的结果,它是一批事件的封面。
        RenderError   error{};
        std::uint32_t count{0};
        /// 自上次推送以来因环满丢弃的条数。**必须上报**:一个会静默丢事件的
        /// 诊断通道,本身就是这套设计要消灭的东西。
        std::uint32_t dropped{0};
    };
    static_assert(std::is_trivially_copyable_v<ErrorEventBatchReply>);

} // namespace lux::render
