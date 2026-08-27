#pragma once
/**
 * @file ExternalInterop.hpp
 * @brief 跨 API 互操作(CUDA↔Vulkan 等)导出资源的纯值类型。
 *
 * 这两个结构原先住在公开门面 RenderContextView.hpp 里,但它们只是 POD
 * (Vk 句柄 + 外部句柄整数),既不是门面概念也不依赖门面。放在门面头里会
 * 迫使**产出方**(L1 的 RenderContext)反过来依赖 L4 门面 —— 一条向上边。
 *
 * 那条向上边当时是靠"下沉到 core(L0)"解掉的 —— **解得对,但落点错了。**
 * 它们是 **CUDA↔Vulkan 互操作**概念:字段就是 Vulkan 句柄,存在的理由就是
 * 把显存导给外部 API。这是 GPU 层(L1)的语义,L0 既不知道也不该知道。
 * 结果是引擎最底层的公开面上凭空多了三个 Vulkan 类型作数据成员。
 *
 * 现在归位到 gpu(L1)。产出方 RenderContext 与本头同层;消费方
 * RenderContextView(L4 门面)按值返回它们、需要完整定义 —— L4→L1 向下,合法。
 * 函数签名一字未改,所以树外无感知。
 *
 * (注意判据:L0 公开面不得把 Vulkan 类型**作为实体**——成员/参数/返回值。
 *  core/vk_fwd.hpp 只生产不完全的不透明句柄名,不构成实体,故豁免;本头把它们
 *  变成结构体数据成员,构成实体,所以不能留在 L0。)
 *
 * Vk 句柄用 core/vk_fwd.hpp 的前向声明,不拉 <vulkan/vulkan.h>。
 */

#include <lux/engine/render/core/vk_fwd.hpp>

#include <cstdint>

namespace lux::render
{
    /// 可被外部 API(CUDA 等)导入的缓冲:Vulkan 侧句柄 + 平台外部句柄。
    struct ExportableBuffer
    {
        VkBuffer buffer{}; // forward-declared in core/vk_fwd.hpp
        VkDeviceMemory memory{};
        uint64_t external_handle{0}; // Win32 HANDLE / POSIX fd widened to u64
        uint64_t actual_size{0};     // >= requested (driver may round up)
    };

    /// 可被外部 API 导入的时间线信号量。
    struct ExportableTimelineSemaphore
    {
        VkSemaphore semaphore{};
        uint64_t external_handle{0};
    };

} // namespace lux::render
