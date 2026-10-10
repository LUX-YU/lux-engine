// ============================================================================
//  shading_inputs.glsl — 屏幕空间着色输入(Light set b11)
//
//  与 C++ 侧 EShadingInputSlot(core/ShadingInputSlot.hpp)一一对应:
//  数组每个元素一个槽,LightResources 在 init 期用 1×1 白纹理写满,
//  提供者(SSAO 等)只是覆写自己的槽 —— 所以这里**无条件采样**,不需要
//  「装没装」分支:没装 = 采到 1.0 = 中性值。
//
//  Define LUX_LIGHT_SET before including to override the descriptor set
//  (deferred_lighting 的 pass 私有布局把 Light 放 set 2;发射器/契约的
//  canonical 位置是 set 3,SPIR-V 重定位负责运行期落位)。
// ============================================================================
#ifndef LUX_SHADING_INPUTS_GLSL
#define LUX_SHADING_INPUTS_GLSL

#ifndef LUX_LIGHT_SET
#define LUX_LIGHT_SET 3
#endif

// 数组大小必须等于 EShadingInputSlot::COUNT(加槽时同步,见该头的步骤注释)。
#define LUX_SHADING_INPUT_COUNT 1
#define LUX_SHADING_INPUT_AO    0

layout(set = LUX_LIGHT_SET, binding = 11)
    uniform sampler2D uShadingInputs[LUX_SHADING_INPUT_COUNT];

/// 屏幕空间环境光遮蔽,1.0 = 不遮蔽。uv 是当前像素的全屏 uv。
float luxShadingInputAO(vec2 uv)
{
    return texture(uShadingInputs[LUX_SHADING_INPUT_AO], uv).r;
}

#endif // LUX_SHADING_INPUTS_GLSL
