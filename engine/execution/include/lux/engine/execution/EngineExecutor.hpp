#pragma once
/**
 * @file EngineExecutor.hpp
 * @brief 引擎的异步运行时(stdexec/P2300)—— 唯一的 CPU 池 + 主线程汇合点。
 *
 * 设计:.internal/async-asset-redesign-2026-06-17.md。
 *
 * **stdexec 严格收敛在本模块(engine/execution)的 .cpp 里**;本头是 stdexec-free 的
 * pimpl 门面 —— 包含它的 editor/launcher 不被 stdexec 头 + MSVC /Zc 开关传染。
 *
 * 职责:
 *   - 持唯一的 `exec::static_thread_pool`(后台 IO/CPU)+ 一个主线程汇合点(被 app
 *     主循环每帧 `drainMain()` 抽干的队列 + 一个 conforming stdexec scheduler)。
 *   - 资产**懒加载的外部编排者**:`requestLoad(id)` 去重后起一条 sender:
 *       schedule(cpu) | then(mgr.vfs()->open + decodeAssetData) | continues_on(main) | then(install)
 *     install(主线程)= `mgr.fetchAsset(id)` 取空壳 → 注入器 setData。**只用 AssetManager
 *     的现有只读 API + 工厂自由函数 —— AssetManager 一行未改、不被注入任何东西**。
 *
 * 用法(app):建一个 EngineExecutor(mgr);把 `[&exec](const asset_id_t& id){ exec.requestLoad(id); }`
 * 注入各消费方(bridge / 动画 resolver);主循环每帧 `drainMain()`。
 */

#include <cstddef>
#include <memory>

#include <lux/engine/asset/Asset.hpp>          // lux::asset::asset_id_t

namespace lux::asset { class AssetManager; }

namespace lux::exec
{
    // engine/execution 是 STATIC 库(链进 lux_editor / lux_launcher),无需 DLL 导出宏。
    class EngineExecutor
    {
    public:
        /// @param mgr         资产仓库(用其只读 API:vfs()/fetchAsset/hasData;注入空壳由它注册)。必须比本对象活得久。
        /// @param cpu_threads 后台池线程数;0 = 自动(hw-2,>=2)。
        explicit EngineExecutor(lux::asset::AssetManager& mgr, std::size_t cpu_threads = 0);
        ~EngineExecutor();

        EngineExecutor(const EngineExecutor&)            = delete;
        EngineExecutor& operator=(const EngineExecutor&) = delete;

        /// 请求异步加载 @p id(主线程调)。去重:已在飞或数据已就绪(mgr.hasData)→ no-op。
        /// 否则后台开盘+解码,完成后在 drainMain() 时把数据注入已注册的空壳。无空壳(未知 id)
        /// 也安全(install 时 fetchAsset 为 null → 丢弃)。
        void requestLoad(const lux::asset::asset_id_t& id);

        /// 在**调用线程**(主线程)跑掉至多 @p max_n 个已就绪的加载-注入 continuation。
        /// app 主循环每帧调(在消费方读取资产之前)。返回实际跑掉的个数。
        std::size_t drainMain(std::size_t max_n = static_cast<std::size_t>(-1));

        /// 关停:取消在飞 + 抽干。析构自动调用。须在主线程、池/队列仍存活时。
        void shutdown();

        // ── 调度器句柄(给 opt-in 的 `EngineExecutorSenders.hpp`)──────────────────
        // 本门面保持 stdexec-free,故不能拼写 `::exec::static_thread_pool` /
        // `::exec::async_scope`(`exec` 是 `experimental::execution` 的**命名空间别名**,
        // 二者皆 struct 且经 using 抵达,连前向声明都非法)。改以 void* 暴露 Impl 子对象
        // 地址,由 `EngineExecutorSenders.hpp` 强转回真实类型(地址来自活子对象、转回原型,
        // 行为完全确定)。非 stdexec 消费方永不调它们。
        [[nodiscard]] void* cpuPoolHandle()   noexcept;   ///< → ::exec::static_thread_pool*
        [[nodiscard]] void* mainQueueHandle() noexcept;   ///< → lux::exec::MainQueue*
        [[nodiscard]] void* asyncScopeHandle() noexcept;  ///< → ::exec::async_scope*

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::exec
