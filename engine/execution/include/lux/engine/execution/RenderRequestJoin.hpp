#pragma once
/**
 * @file RenderRequestJoin.hpp
 * @brief `whenAllReady(...)` — 动态元数的"全就绪"计数 join,把一组运行期大小的
 *        `RenderRequest<T>` 合成一条 sender:当**所有**请求都就绪时以 `set_value()` 收尾。
 *
 * Phase C5(ThumbnailService)需要它:每个 job 的并行上传(N 个 mesh / shader / texture 上传,
 * N 在编译期未知)对应早先 `advanceJob` 里 `allReady(vector<RenderRequest<T>>)` 的轮询。
 * `stdexec::when_all` 是**静态元数**的,故这里手写一个计数 join。
 *
 * 语义:reply 的**值不向下传播**(join 只发"全好了"信号)—— 调用方随后自己 `req.result()` 取值。
 * 因为 join 用掉了每个 request 的**单一 continuation 槽**(`then`),所以传进来的 request **不可**
 * 再被 `asSender`/`then`(见 RenderRequestSender.hpp 脚枪 1)。空集合**同步**完成。
 *
 * 仅主线程:RenderRequest 的 continuation 在 `pumpReplies()`(主线程)触发,故计数器无需原子。
 *
 * **同步完成脚枪**:已就绪的 request(makeImmediate / 已 pump)会在 `start()` 里**同步**触发
 * then。用 `+1 哨兵` 把"归零→set_value(可能析构 *this)"推迟到遍历 reqs_ 之后,避免迭代中析构。
 *
 * opt-in stdexec 头 —— 含它的 TU 需自带 /Zc 开关 + stdexec 链接。
 */

#include <cstddef>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include <lux/engine/render/comm/client/RenderRequest.hpp>

namespace lux::exec
{
    namespace ex = ::stdexec;

    namespace detail
    {
        template <class T>
        struct when_all_ready_sender
        {
            using sender_concept = ex::sender_t;
            using completion_signatures = ex::completion_signatures<ex::set_value_t()>;

            std::vector<lux::render::RenderRequest<T>> reqs_;

            ex::empty_env get_env() const noexcept { return {}; }

            template <class Rcvr>
            struct _op
            {
                using operation_state_concept = ex::operation_state_t;

                std::vector<lux::render::RenderRequest<T>> reqs_;   // own copies → State 活到 join 完成
                Rcvr                                       rcvr_;
                std::size_t                                remaining_{0};

                void start() & noexcept
                {
                    // +1 哨兵:已就绪的 request 会在下面 then() 里同步 dec;若不加哨兵,可能在
                    // 遍历 reqs_ 途中归零→set_value→析构 *this(迭代中 use-after-free)。哨兵把
                    // 完成推迟到循环之后那次 dec()。
                    remaining_ = reqs_.size() + 1;
                    for (auto& r : reqs_)
                        r.then([this](const T&) noexcept { dec(); });
                    dec();   // 释放哨兵 —— 若全部已就绪,完成发生在这里(循环已结束)
                }

                void dec() noexcept
                {
                    // 归零边沿必须是对 *this 的最后一次触碰:set_value 可能析构本 op。
                    if (--remaining_ == 0)
                        ex::set_value(std::move(rcvr_));
                }
            };

            template <class Rcvr>
            _op<std::decay_t<Rcvr>> connect(Rcvr&& r)
            {
                return _op<std::decay_t<Rcvr>>{ std::move(reqs_), std::forward<Rcvr>(r), 0 };
            }
        };
    } // namespace detail

    /// 把一组 render reply 合成"全就绪"信号:全部就绪后以 `set_value()` 收尾。值不传播——
    /// 随后自行 `req.result()` 取值;**不要**再对这些 request 调 asSender/then(槽已被 join 占用)。
    /// 空集合同步完成。仅主线程。
    template <class T>
    [[nodiscard]] detail::when_all_ready_sender<T>
    whenAllReady(std::vector<lux::render::RenderRequest<T>> reqs)
    {
        return detail::when_all_ready_sender<T>{ std::move(reqs) };
    }

} // namespace lux::exec
