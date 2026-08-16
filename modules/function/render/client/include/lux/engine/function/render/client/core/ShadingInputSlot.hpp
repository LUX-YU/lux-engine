#pragma once
// ============================================================================
//  ShadingInputSlot.hpp — 屏幕空间着色输入槽(Light set b11 的元素索引)
//
//  「着色输入」是喂给光照计算的逐像素调制纹理:由可选的屏幕空间特性
//  (SSAO、屏幕空间阴影、bent normal……)产出,由所有 lit 路径消费。
//  它们活在 Light set 的 SHADING_INPUTS binding(一个 COMBINED_IMAGE_SAMPLER
//  数组,每槽一个元素),与影子段(b4-b10)的关键区别:
//
//    **没有 PARTIALLY_BOUND —— 每个槽总是有效。**
//    LightResources 在 init 期用 1×1 白纹理(1.0)把数组写满,消费着色器
//    无条件采样;提供者只是把自己的槽覆写成真纹理,卸载时写回默认。
//    「没装 SSAO」于是不是一条着色器分支,而是「采样到 1.0 = 无遮蔽」。
//
//  加槽的步骤(A/B/C 三表 + 本枚举必须同一提交):
//    1. 这里加枚举项(COUNT 前);
//    2. 描述符契约 C 表(description/LayoutContract.hpp)的 uShadingInputs
//       条目 count 字面量 +1;
//    3. 形状表 B(EngineSetShapes.hpp)的 count 用本枚举的 COUNT,自动跟;
//    4. 着色器侧 shading_inputs.glsl 的数组大小与槽宏同步。
//  漏改会被 B3 的编译期对账断言拦下。
// ============================================================================
#include <cstdint>

namespace lux::render
{
    /// Light set b11 数组内的元素索引。默认值语义写在每项注释里 ——
    /// 默认纹理是 1×1 纯白(所有通道 1.0),每个槽的「中性值」必须
    /// 在这个前提下成立。
    enum class EShadingInputSlot : uint8_t
    {
        /// 屏幕空间环境光遮蔽,.r 通道,1.0 = 完全不遮蔽(中性)。
        AmbientOcclusion = 0,

        COUNT
    };

    inline constexpr uint32_t kShadingInputSlotCount =
        static_cast<uint32_t>(EShadingInputSlot::COUNT);

} // namespace lux::render
