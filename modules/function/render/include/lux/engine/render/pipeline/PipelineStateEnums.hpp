#pragma once
/**
 * @file PipelineStateEnums.hpp
 * @brief Platform-agnostic graphics-pipeline-state enumerations for the SDK.
 *
 * The RHI-neutral vocabulary for the fixed-function pipeline state an external
 * RenderFeature declares in a GraphicsPipelineDesc. Mirrors the Vulkan enums the
 * internal GraphicsPipelineTemplate uses, but carries NO <vulkan/vulkan.h>
 * dependency (same policy as lux::common::ImageEnums.hpp). Conversion to the
 * native Vk* enums happens inside the render backend (see GraphicsPipelineDesc
 * -> GraphicsPipelineTemplate conversion in fromDesc()).
 *
 * Pixel / vertex-attribute FORMATS reuse lux::common::ETextureFormat (already
 * neutral, already covers R32/RG32/RGB32/RGBA32_SFLOAT etc.) — no separate
 * vertex-format enum.
 */

#include <cstdint>

namespace lux::render
{
    // ── Primitive assembly ───────────────────────────────────────────────
    enum class EPrimitiveTopology : uint8_t
    {
        POINT_LIST,
        LINE_LIST,
        LINE_STRIP,
        TRIANGLE_LIST,
        TRIANGLE_STRIP,
        TRIANGLE_FAN,
    };

    // ── Rasterization ────────────────────────────────────────────────────
    enum class EPolygonMode : uint8_t { FILL, LINE, POINT };

    /// Cull mode is a flag set (NONE / FRONT / BACK / FRONT_AND_BACK).
    enum class ECullMode : uint8_t
    {
        NONE            = 0,
        FRONT           = 0x1,
        BACK            = 0x2,
        FRONT_AND_BACK  = 0x3,
    };

    enum class EFrontFace : uint8_t { COUNTER_CLOCKWISE, CLOCKWISE };

    // ── Depth / compare ──────────────────────────────────────────────────
    enum class ECompareOp : uint8_t
    {
        NEVER,
        LESS,
        EQUAL,
        LESS_OR_EQUAL,
        GREATER,
        NOT_EQUAL,
        GREATER_OR_EQUAL,
        ALWAYS,
    };

    // ── Color blending ───────────────────────────────────────────────────
    enum class EBlendFactor : uint8_t
    {
        ZERO,
        ONE,
        SRC_COLOR,
        ONE_MINUS_SRC_COLOR,
        DST_COLOR,
        ONE_MINUS_DST_COLOR,
        SRC_ALPHA,
        ONE_MINUS_SRC_ALPHA,
        DST_ALPHA,
        ONE_MINUS_DST_ALPHA,
        CONSTANT_COLOR,
        ONE_MINUS_CONSTANT_COLOR,
        CONSTANT_ALPHA,
        ONE_MINUS_CONSTANT_ALPHA,
        SRC_ALPHA_SATURATE,
    };

    enum class EBlendOp : uint8_t { ADD, SUBTRACT, REVERSE_SUBTRACT, MIN, MAX };

    /// Color write mask is a flag set (R / G / B / A); combine with bitwise OR.
    enum class EColorComponent : uint8_t
    {
        R    = 0x1,
        G    = 0x2,
        B    = 0x4,
        A    = 0x8,
        RGBA = 0xF,
    };

    // ── Vertex input ─────────────────────────────────────────────────────
    enum class EVertexInputRate : uint8_t { VERTEX, INSTANCE };

    // ── Shader stages (flag set) ─────────────────────────────────────────
    enum class EShaderStage : uint32_t
    {
        VERTEX          = 0x1,
        FRAGMENT        = 0x10,
        COMPUTE         = 0x20,
        ALL_GRAPHICS    = 0x1F,
    };

    // -- bitwise helpers for the flag enums --
    inline constexpr ECullMode operator|(ECullMode a, ECullMode b) noexcept
    { return static_cast<ECullMode>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b)); }
    inline constexpr EColorComponent operator|(EColorComponent a, EColorComponent b) noexcept
    { return static_cast<EColorComponent>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b)); }
    inline constexpr EShaderStage operator|(EShaderStage a, EShaderStage b) noexcept
    { return static_cast<EShaderStage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); }

} // namespace lux::render
