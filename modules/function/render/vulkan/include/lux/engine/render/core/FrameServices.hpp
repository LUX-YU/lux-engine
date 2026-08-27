#pragma once

#include <lux/engine/function/visibility.h>
#include <lux/engine/function/render/client/core/FrameStamp.hpp>

#include <cstdint>

namespace lux::render
{

    enum class EUploadPhase : uint8_t
    {
        PreUpload = 0,
        Upload = 1,
        PostUpload = 2,
        Count
    };

    // 这里曾有一个 IFrameService 虚接口(更早还是 IGlobal…/IScene… 两个逐字相同的
    // 副本),由资源类型继承、注册表在 emplace<T> 里靠 is_base_of 自动登记。
    //
    // 两个问题:
    //   1. **数据层带上了每帧钩子**。资源被迫继承一个框架接口才能参与帧循环,而
    //      "每帧要不要驱动我"本该由**装配方**决定,不是数据自己的属性。
    //   2. **自动登记是隐式的**。新加一个继承该接口的资源,就静默多出一个每帧回调,
    //      顺序藏在注册表遍历次序里,没人看得见。实测代价:onEndFrame **零个**资源
    //      重写,却每帧被完整遍历两遍(全局 + 每场景);另有两个资源继承了接口却
    //      一个钩子都没重写,纯空转。
    //
    // 改为**由安装点显式登记回调**(见 ResourceRegistry::addBeginFrameHook /
    // addViewDestroyedHook)。驱动仍由注册表做,所以顺序 = 登记顺序 = 原注册顺序,
    // 逐字不变;而登记写在安装点的 fresh 守卫内,天然"恰好一次" —— 若改成"谁安装
    // 谁驱动",ensure<> 安装的资源(LightResources / InstanceResources)在多个单元
    // 同时挂载时会被驱动多次。
    //
    // onEndFrame 一并删除:零用户。

} // namespace lux::render
