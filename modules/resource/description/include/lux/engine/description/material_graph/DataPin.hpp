#pragma once
// =============================================================================
//  DataPin.hpp  —  材质图的类型化数据引脚 + 连接
// -----------------------------------------------------------------------------
//  材质图是【纯 dataflow】：只有 data pin，没有 FlowForge 的 exec pin / token。
//  引脚类型用 EMatValueType（简单枚举），不走 lux::meta 反射系统。
//
//  数据模型层（下沉到 description）：纯数据，资产/编辑器/降级器共享同一套图结构。
// =============================================================================

#include <cstdint>
#include <string>
#include <vector>

#include <lux/engine/description/MaterialGraphContract.hpp>  // EMatValueType

namespace lux::rdesc
{
    using node_id = uint64_t;

    inline constexpr node_id invalid_node = ~0ull;
    inline constexpr uint32_t invalid_pin = ~0u;

    enum class EPinDirection : uint8_t
    {
        Input,
        Output
    };

    /**
     * @brief 一条连接的来源：某节点的某个输出引脚。
     *        输入引脚至多有一个来源（纯 dataflow，fan-in = 1）。
     */
    struct PinLink
    {
        node_id  node = invalid_node;  ///< 来源节点
        uint32_t pin  = invalid_pin;   ///< 来源节点的输出引脚下标

        bool valid() const noexcept
        {
            return node != invalid_node && pin != invalid_pin;
        }
    };

    /**
     * @brief 类型化数据引脚（聚合类型，便于初始化）。
     *        - 输入引脚：`source` 指向来源；未连接时用 `constant` 作默认值。
     *        - 输出引脚：`source` / `constant` 忽略。
     */
    struct DataPin
    {
        std::string   name;
        EMatValueType type      = EMatValueType::Float;
        EPinDirection direction = EPinDirection::Input;
        PinLink       source;                    ///< 仅输入引脚有意义
        float         constant[4] = { 0, 0, 0, 0 }; ///< 输入未连接时的默认常量
    };

} // namespace lux::rdesc
