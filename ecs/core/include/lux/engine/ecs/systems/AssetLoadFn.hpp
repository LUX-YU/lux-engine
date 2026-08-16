#pragma once
/**
 * @file AssetLoadFn.hpp
 * @brief Injectable callback type for "load this asset asynchronously, on
 *        demand".
 *
 * gameplay's asset consumers (SceneRenderBinding / SkeletalAnimationResolver)
 * don't know — and shouldn't know — that the async runtime (runtime/execution's
 * AssetClient) exists. They only hold a function value like this one: call
 * it when some asset_id resolves to an "empty shell missing its data", and the
 * app wires it to `AssetClient::request` (which opens and decodes through
 * the background, then injects the result at the main-thread sync point).
 *
 * This is the "orchestration is entirely external" principle from
 * .internal/async-asset-redesign-2026-06-17.md §2: load orchestration lives
 * in the engine layer, consumers only trigger it through this function value,
 * so the layering is never inverted and nothing here couples to stdexec.
 *
 * **必填**(2026-08-04 收口):空值不再回落同步 ensureAsset —— 同步加载可在
 * ensure* 状态机**中途**注册/驱逐任意资产,是「payload 引用悬垂」一族 bug 的
 * 根(缓存里修过三处)。产品宿主全部接线 AssetClient::request;测试/
 * demo 用下面的 syncTestLoader,让「同步只在测试」在调用点看得见。
 */

#include <functional>

#include <lux/engine/resource/asset/Asset.hpp>          // lux::asset::asset_id_t
#include <lux/engine/resource/asset/AssetManager.hpp>   // syncTestLoader 的实现

namespace lux::ecs
{
    /// "Please asynchronously load this asset's data into the empty shell
    /// already registered for it." Idempotency and de-duplication are the
    /// implementation's (the executor's) responsibility; called from the
    /// main thread.
    using AssetLoadFn = std::function<void(const lux::asset::asset_id_t&)>;

    /// 测试/demo 专用的同步 loader:当场 ensureAsset。**产品宿主不得使用**——
    /// 它会在消费者的 ensure* 状态机中途同步改世界(注册/驱逐),悬垂窗口
    /// 就是这么来的。具名而不是传 {},让同步语义在调用点无处遁形。
    /// @p mgr 必须比消费者活得久(测试夹具里天然成立)。
    [[nodiscard]] inline AssetLoadFn syncTestLoader(lux::asset::AssetManager& mgr)
    {
        return [&mgr](const lux::asset::asset_id_t& id)
        {
            (void)mgr.ensureAsset(id);
        };
    }

} // namespace lux::ecs
