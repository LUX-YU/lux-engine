#pragma once
/**
 * @file AsyncCache.hpp
 * @brief `AsyncCache<K,V>` — 主线程单线程的"键 → {Pending|Ready|Failed, 值}"缓存,统一
 *        编辑器里两套手写缓存(ThumbnailService 的 cache_、EditorTextureCache 的 cache_)。
 *
 * 两种用法:
 *  - **高层 fire-and-forget**(EditorTextureCache):`ensure(key, producerFactory)` 首次 miss
 *    标 Pending 并在 EngineExecutor 的 async_scope 上起一条 `producer() | continues_on(main)
 *    | then(存 Ready) | upon_error/upon_stopped(存 Failed)`;命中(任意态)即 no-op(去重)。
 *  - **低层 driver**(ThumbnailService 的 FIFO/单活动 job 自管调度):`markPending` /
 *    `setReady` / `setFailed` —— 调用方自己起管线、自己把结果存回同一缓存。
 *
 * **单线程契约**:所有读写都在主线程(`ensure` 的 store 经 `continues_on(mainScheduler)` 落到
 * drainMain;driver 的 set* 由主线程调)。故 map 无需锁。存储用的 lambda 按值捕获 `this`+`key`
 * 并重新 `find(key)`(rehash 不失效指针;invalidate 后 miss 即丢弃,天然安全)。
 *
 * **生命周期**:`ensure` 的 lambda 捕获 `this`(本缓存),故本类禁拷贝/移动(地址须稳定)——
 * 编辑器子系统按成员持有它、子系统本身在堆上(地址稳定)。销毁序:必须先
 * `EngineExecutor::shutdown()`(抽干 scope)再析构本缓存,否则迟到的 store 写已死 map。
 *
 * opt-in stdexec 头(含 EngineExecutorSenders.hpp)—— 包含它的 TU 需自带 /Zc 开关 + stdexec 链接。
 */

#include <lux/engine/execution/EngineExecutor.hpp>
#include <lux/engine/execution/EngineExecutorSenders.hpp>

#include <stdexec/execution.hpp>

#include <functional>
#include <unordered_map>
#include <utility>

namespace lux::exec
{
    enum class CacheState { Pending, Ready, Failed };

    template <class K, class V, class Hash = std::hash<K>, class Eq = std::equal_to<K>>
    class AsyncCache
    {
    public:
        explicit AsyncCache(EngineExecutor& exec) noexcept : exec_(&exec) {}

        AsyncCache(const AsyncCache&)            = delete;   // ensure 的 lambda 捕获 this → 地址须稳定
        AsyncCache& operator=(const AsyncCache&) = delete;
        AsyncCache(AsyncCache&&)                 = delete;
        AsyncCache& operator=(AsyncCache&&)      = delete;

        // ── 查询(只读)────────────────────────────────────────────────────────
        /// Ready 则返回值指针,否则 nullptr(Pending/Failed/缺失)。
        [[nodiscard]] const V* tryGet(const K& key) const noexcept
        {
            auto it = map_.find(key);
            return (it != map_.end() && it->second.st == CacheState::Ready) ? &it->second.val
                                                                            : nullptr;
        }
        /// 缺失视为 Pending(两个消费方只区分 Ready vs 非-Ready)。
        [[nodiscard]] CacheState state(const K& key) const noexcept
        {
            auto it = map_.find(key);
            return it == map_.end() ? CacheState::Pending : it->second.st;
        }

        // ── 低层 driver API(调用方自管调度)──────────────────────────────────
        /// 标记 @p key 为 Pending。返回 true 当且仅当本次新插入(调用方据此决定是否起生产)。
        bool markPending(const K& key)
        {
            return map_.try_emplace(key, Slot{CacheState::Pending, V{}}).second;
        }
        void setReady(const K& key, V v)
        {
            if (auto it = map_.find(key); it != map_.end())
            {
                it->second.val = std::move(v);
                it->second.st  = CacheState::Ready;
            }
        }
        void setFailed(const K& key)
        {
            if (auto it = map_.find(key); it != map_.end())
                it->second.st = CacheState::Failed;
        }

        // ── 高层 fire-and-forget ───────────────────────────────────────────────
        /// 首次 miss → 标 Pending + 在 scope 上起生产管线;命中(任意态)→ no-op(去重)。
        /// @p producerFactory 是无参可调用,返回一条 set_value(V) 的 sender。
        /// @warning 若 producer 内含发渲染命令的步骤,必须在**帧-OPEN**(tick)里调本函数——
        ///          spawn 会立即 start 该 sender。返回调用后的当前态。
        template <class ProducerFactory>
        CacheState ensure(const K& key, ProducerFactory&& producerFactory)
        {
            auto [it, inserted] = map_.try_emplace(key, Slot{CacheState::Pending, V{}});
            if (!inserted) return it->second.st;   // 去重

            namespace ex = ::stdexec;
            spawn(*exec_,
                  producerFactory()
                | ex::continues_on(mainScheduler(*exec_))      // store 落到主线程
                | ex::then([this, key](V v) noexcept
                  {
                      if (auto i = map_.find(key); i != map_.end())
                      {
                          i->second.val = std::move(v);
                          i->second.st  = CacheState::Ready;
                      }
                  })
                | ex::upon_error([this, key](auto&&) noexcept   // 任意 error 类型 → Failed
                  {
                      if (auto i = map_.find(key); i != map_.end())
                          i->second.st = CacheState::Failed;
                  })
                | ex::upon_stopped([this, key]() noexcept        // 取消 → Failed
                  {
                      if (auto i = map_.find(key); i != map_.end())
                          i->second.st = CacheState::Failed;
                  }));
            return CacheState::Pending;
        }

        void invalidate(const K& key) { map_.erase(key); }
        void clear() noexcept { map_.clear(); }

    private:
        struct Slot { CacheState st; V val; };

        EngineExecutor*                          exec_;
        std::unordered_map<K, Slot, Hash, Eq>    map_;
    };

} // namespace lux::exec
