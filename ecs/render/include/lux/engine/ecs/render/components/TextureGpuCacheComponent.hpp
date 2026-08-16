#pragma once
/**
 * @file TextureGpuCacheComponent.hpp
 * @brief 「这个实体要用的贴图已经在 GPU 上了，句柄在这儿」——运行期产物。
 *
 * 与 `AnimatorCacheComponent` 同一个形状、同一个理由：**作者写意图，解析器产结果，
 * 消费者只读结果**。
 *
 *   Image2DComponent.texture (asset_id, 存盘)
 *          │  ResidencySubsystem 驻留胶水:边沿请求 + 等待名单送达
 *          ▼
 *   TextureGpuCacheComponent.handle (RTextureHandle, 运行期)
 *          │
 *          ▼
 *   Image2DSubsystem：`try_get` 一下就画 —— 不认识 asset_id、不认识 AssetManager
 *
 * 故意**不是反射组件**（没有 LUX_COMPONENT、不在 meta 的 TARGET_FILES 里）：
 * 它装的是运行期 GPU 句柄，绝不该被序列化、也不该出现在 Lua 或属性面板里。
 * 存盘的是上面那个 `asset_id`，重新打开时重新解析。
 *
 * ⚠️ 组件**不存在**就是「还没就绪」。这正是它取代 `ensureTexture` 每帧轮询的地方：
 * 消费者的 `try_get` 拿到空指针就当没贴图，不需要空句柄重试、不需要自己记 pending。
 * 反过来，解析器在贴图就绪之前**不写**这个组件；换图时也要等新图就绪才改写，
 * 所以运行期换图看到的是「旧图一直显示到新图到位」，不是闪一下没有贴图。
 */

#include <lux/engine/resource/asset/Asset.hpp>                        // asset_id_t
#include <lux/engine/function/render/client/core/RenderResourceHandle.hpp>   // RTextureHandle

namespace lux::ecs
{
    struct TextureGpuCacheComponent
    {
        /// 就绪的 GPU 贴图。`RTextureHandle::index` 就是 bindless 索引。
        lux::render::RTextureHandle handle{};
        /// 产生它的资产 id。解析器用它判「作者是不是换图了」；消费者一般不看。
        lux::asset::asset_id_t      source{};
    };

} // namespace lux::ecs
