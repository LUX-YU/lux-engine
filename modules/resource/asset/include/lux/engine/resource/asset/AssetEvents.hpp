#pragma once
/**
 * @file AssetEvents.hpp
 * @brief 资产域的广播事件类型(统一事件系统条例②:事件 struct 是领域词汇,
 *        定义在字段类型可见的最低层 —— asset_id_t 在这里)。
 *
 * 本模块**没有事件概念**(裁决③):这些只是纯数据类型。发布发生在宿主
 * 组装层 —— AssetManager::setBroadcast 注入的回调把账本事实翻译成总线
 * 事件;订阅在消费层(ResidencyAssembly / MaterialPreviewHost / …)。
 *
 * 三个语义**互不混用**(热更新批4 定死的不变量):
 *   · AssetUnreferenced   —— 计数 1→0:没人要了,派生物(GPU 副本)回收;
 *   · AssetInvalidated    —— removeAsset 扩散(裁决七):对象没了,停发
 *                            load + M_Missing 换装兜底;账本不清;
 *   · AssetContentChanged —— 对象**还在**,内容换了(replaceAsset /
 *                            notifyContentChanged):派生副本按 revision
 *                            失配重建,不闪兜底、不动计数。
 */

#include <lux/engine/resource/asset/Asset.hpp>   // asset_id_t

#include <cstdint>

namespace lux::asset
{
    /// 引用计数降到 0(没人要了 —— 派生物回收的触发边沿)。
    struct AssetUnreferenced
    {
        asset_id_t id;
    };

    /// 对象被 removeAsset 摘除(兴趣可能还在 —— 账本不清,裁决七)。
    struct AssetInvalidated
    {
        asset_id_t id;
    };

    /// 对象在场,内容换了(revision 已 bump —— 派生副本失配重建)。
    struct AssetContentChanged
    {
        asset_id_t    id;
        std::uint32_t revision;
    };

    /// 资产(重新)注册进账本(驻留 T11:失效封印的**推式解封** ——
    /// 删除后又恢复的 id 经它触发重加载,消灭旧缓存的每帧解封查询)。
    struct AssetRegistered
    {
        asset_id_t id;
    };

} // namespace lux::asset
