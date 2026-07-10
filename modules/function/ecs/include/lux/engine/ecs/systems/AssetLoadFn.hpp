#pragma once
/**
 * @file AssetLoadFn.hpp
 * @brief 注入式"按需异步加载某资产"的回调类型。
 *
 * gameplay 的资产消费方(RenderableBridgeContext / SkeletalAnimationResolver)
 * 不知道、也不该知道异步运行时(engine/execution 的 EngineExecutor)的存在 ——
 * 它们只持有这样一个函数值:当某个 asset_id 解析到"缺数据的空壳"时调它一下,
 * 由 app 把它接到 `EngineExecutor::requestLoad`(后台开盘+解码,主线程同步点注入)。
 *
 * 这是 .internal/async-asset-redesign-2026-06-17.md §2 的"流程完全外部控制":
 * 加载编排在 engine 层,消费方只通过这个函数值触发,层次不被倒置,且不耦合 stdexec。
 *
 * 空(未注入)= "没有异步加载器"。消费方此时退化为同步 `AssetManager::ensureAsset`
 * 兜底(见 RenderableBridgeContext 的 ctor),所以未接线的 headless/工具进程仍可用。
 */

#include <functional>

#include <lux/engine/asset/Asset.hpp>   // lux::asset::asset_id_t

namespace lux::ecs
{
    /// "请异步把这个资产的数据加载进它已注册的空壳"。幂等、去重由实现方(执行器)负责;
    /// 主线程调用。
    using AssetLoadFn = std::function<void(const lux::asset::asset_id_t&)>;

} // namespace lux::ecs
