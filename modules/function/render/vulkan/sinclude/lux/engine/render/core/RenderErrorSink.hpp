#pragma once
/**
 * @file RenderErrorSink.hpp
 * @brief 渲染线程私有的自发上报汇集器。
 *
 * 定长、无分配 —— 写一条 = 一次线性扫描加一次 POD 赋值。每 tick 由
 * `GeneralRenderServer::flushErrorEvents()` 打包进一帧回复并清空。
 *
 * **只能在渲染线程上用。** 合并靠线性扫描 + 就地自增,没有任何同步 —— 两个线程
 * 同时 emit 就是数据竞争。这不是疏漏而是取舍:合并语义要求单写者,加锁会把渲染
 * 线程上最频繁的那条路径变成有争用的。其它线程要报的东西先进自己的 SPSC 环
 * (见 ValidationEventRing),由渲染线程排空后折进来。
 *
 * 合并键是 (scene_index, error.type, error.args)。**含实参**:同一类型但不同 binding
 * 的两次失败是两条,不会被折叠 —— 这正是错误值保留实参之后拿到的额外精度。
 *
 * 背压写死:环满 → 丢**最新**的,`dropped_` 累加。保留最早的那批,因为第一次失败的
 * 诊断价值最高(后面往往都是它的连锁反应)。
 */

#include <lux/engine/function/render/client/core/RenderErrorEvent.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>

namespace lux::render
{
    class RenderErrorSink
    {
    public:
        /// 环容量。64 × 48 字节 ≈ 3 KB,随服务器对象一起分配一次。
        static constexpr std::size_t kCapacity = 64;

        /// 记一条失败。同键的已有条目只累加 occurrences,不占新槽。
        void emit(const RenderError& error, std::uint32_t scene_index, std::uint64_t frame_serial) noexcept
        {
            if (error.ok())
                return;

            const auto batch = std::span<RenderErrorEvent>{ring_.data(), count_};
            const auto same = std::ranges::find_if(batch, [&](const RenderErrorEvent& e) {
                return e.scene_index == scene_index && e.error.type == error.type && e.error.args == error.args;
            }
            );
            if (same != batch.end())
            {
                ++same->occurrences;
                same->frame_serial = frame_serial; // 最近一次发生的帧
                return;
            }

            if (count_ >= kCapacity)
            {
                ++dropped_;
                return;
            }

            ring_[count_++] = RenderErrorEvent{
                .error = error,
                .scene_index = scene_index,
                .occurrences = 1,
                .frame_serial = frame_serial,
                .seq = next_seq_++,
            };
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return count_ == 0;
        }

        [[nodiscard]] std::span<const RenderErrorEvent> pending() const noexcept
        {
            return {ring_.data(), count_};
        }

        [[nodiscard]] std::uint32_t dropped() const noexcept
        {
            return dropped_;
        }

        /// 推送成功后调用。推送失败(回复环满 / 通道关闭)时**不调用** ——
        /// 事件留在原地,下 tick 重试,与 flushDeferredRepliesOnly 同策略。
        void clear() noexcept
        {
            count_ = 0;
            dropped_ = 0;
        }

    private:
        std::array<RenderErrorEvent, kCapacity> ring_{};
        std::uint32_t count_{0};
        std::uint32_t dropped_{0};
        std::uint64_t next_seq_{0};
    };

} // namespace lux::render
