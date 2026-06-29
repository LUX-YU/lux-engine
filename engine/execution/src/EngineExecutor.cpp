#include <lux/engine/execution/EngineExecutor.hpp>

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/AssetSerDeser.hpp>   // decodeAssetData / AssetDataInjector
#include <lux/engine/asset/AssetVfs.hpp>         // AssetVfs::open / AssetBlob

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/compile_time/move_only_function.hpp>

#include <stdexec/execution.hpp>
#include <exec/static_thread_pool.hpp>
#include <exec/async_scope.hpp>

#include <lux/engine/execution/detail/MainScheduler.hpp>   // MainQueue / MainScheduler(原匿名命名空间,已迁出)

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

namespace lux::exec
{
    namespace ex = stdexec;

    // MainQueue / MainScheduler 已迁出本 .cpp 的匿名命名空间,改放 detail/MainScheduler.hpp
    // (namespace lux::exec,非匿名)—— 因为 EngineExecutorSenders.hpp 把
    // mainQueueHandle() 的 void* 强转回 MainQueue*,二者必须同一定义(否则静默 ODR 违例)。

    // =========================================================================
    //  Impl
    // =========================================================================
    struct EngineExecutor::Impl
    {
        lux::asset::AssetManager& mgr;
        // NB: fully-qualified ::exec:: — we are INSIDE namespace lux::exec, which
        // would otherwise shadow stdexec's extension namespace ::exec.
        ::exec::static_thread_pool  pool;
        MainQueue                   main;
        ::exec::async_scope         scope;
        // 在飞 id 去重集。仅主线程访问(requestLoad 与 install/取消-tail 都在主线程),无需锁。
        std::unordered_set<lux::asset::asset_id_t> in_flight;
        bool stopped_{false};   // shutdown 幂等门闩(dtor 与显式 shutdown() 都可能调)

        Impl(lux::asset::AssetManager& m, std::uint32_t threads)
            : mgr(m), pool(threads) {}

        void requestLoad(const lux::asset::asset_id_t& id)
        {
            if (id.is_nil())            return;
            if (mgr.hasData(id))        return;   // 数据已就绪
            if (in_flight.contains(id)) return;   // 已在飞

            auto vfs = mgr.vfs();
            if (!vfs)                   return;   // 无 provider,无从加载

            in_flight.insert(id);
            scope.spawn(
                ex::schedule(pool.get_scheduler())
                | ex::then([id, vfs]() noexcept
                           -> lux::cxx::expected<lux::asset::AssetDataInjector, lux::asset::EAssetError>
                  {
                      auto blob = vfs->open(id);                       // 后台 IO(VFS 并发只读安全)
                      if (!blob.has_value())
                          return lux::cxx::unexpected(blob.error());
                      if (blob.value().size < sizeof(std::uint32_t))
                          return lux::cxx::unexpected(lux::asset::EAssetError::ABNORMAL_FILE_SIZE);
                      return lux::asset::decodeAssetData(blob.value().data.get(),
                                                         blob.value().size);  // 纯解码 → 注入器
                  })
                | ex::continues_on(MainScheduler{main})                 // 跳回主线程汇合点
                | ex::then([this, id](
                      lux::cxx::expected<lux::asset::AssetDataInjector, lux::asset::EAssetError> r) noexcept
                  {
                      in_flight.erase(id);
                      if (r.has_value())
                      {
                          // 注入既有空壳:fetchAsset 取壳 → injector(setData)。壳若已 evict → null,丢弃。
                          if (auto* shell = mgr.fetchAsset(id))
                              r.value()(*shell);
                      }
                  })
                // 取消路径:shutdown 的 request_stop() 会让"已入队未开跑"的池任务以
                // set_stopped 收尾 → 经 continues_on 回到主线程,但上面的 install-then 只在
                // set_value 触发,被跳过。这里在主线程补做 in_flight 清理,使 shutdown 的抽干
                // 循环仍能据 in_flight 收敛(否则那条记录残留,循环空转满 1M 次才退出)。
                // 两条路径互斥,故 in_flight.erase 不会重复(erase 对缺失键也安全)。
                | ex::upon_stopped([this, id]() noexcept
                  {
                      in_flight.erase(id);
                  }));
        }

        void drainMain(std::size_t max_n) { (void)main.pump(max_n); }

        void shutdown()
        {
            if (stopped_) return;     // 幂等
            stopped_ = true;
            scope.request_stop();     // 取消"已入队未开跑"的池任务;运行中的解码不打断,会自然跑完
            // 抽干:pump 主队列直到 in_flight 清空。任务无论以 set_value(install-then 清)
            // 还是 set_stopped(upon_stopped 清)收尾都会移除自己的 in_flight 项,故据此收敛。
            // bounded spin 兜底(防御一个永不收尾的病态任务,例如卡死的 VFS open)。
            for (int spins = 0; spins < 1'000'000; ++spins)
            {
                if (in_flight.empty()) break;
                main.pump(static_cast<std::size_t>(-1));
                std::this_thread::yield();
            }
            // 此时所有 spawned op 已完成(value/stopped),scope 计数归零 → on_empty 即时返回。
            ex::sync_wait(scope.on_empty());
        }
    };

    // =========================================================================
    //  Public facade
    // =========================================================================
    namespace
    {
        std::uint32_t pickThreads(std::size_t requested) noexcept
        {
            if (requested > 0)
                return static_cast<std::uint32_t>(requested);
            const unsigned hw = std::thread::hardware_concurrency();
            const unsigned avail = (hw > 2u) ? (hw - 2u) : 2u;
            return std::max(avail, 2u);
        }
    } // namespace

    EngineExecutor::EngineExecutor(lux::asset::AssetManager& mgr, std::size_t cpu_threads)
        : impl_(std::make_unique<Impl>(mgr, pickThreads(cpu_threads)))
    {
    }

    EngineExecutor::~EngineExecutor()
    {
        impl_->shutdown();
    }

    void EngineExecutor::requestLoad(const lux::asset::asset_id_t& id)
    {
        impl_->requestLoad(id);
    }

    std::size_t EngineExecutor::drainMain(std::size_t max_n)
    {
        return impl_->main.pump(max_n);
    }

    void EngineExecutor::shutdown()
    {
        impl_->shutdown();
    }

    // ── 调度器句柄:返回活着的 Impl 子对象地址。EngineExecutorSenders.hpp 把它们强转回
    //    ::exec::static_thread_pool* / MainQueue* / ::exec::async_scope*(转回原类型,确定行为)。
    void* EngineExecutor::cpuPoolHandle()    noexcept { return &impl_->pool; }
    void* EngineExecutor::mainQueueHandle()  noexcept { return &impl_->main; }
    void* EngineExecutor::asyncScopeHandle() noexcept { return &impl_->scope; }

} // namespace lux::exec
