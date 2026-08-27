#pragma once

#include <compare>
#include <cstdint>

namespace lux::render
{

    enum class EGeometryKind : uint8_t
    {
        StaticMesh = 0,
        SkinnedMesh,
        Particle,
        Decal,
        Custom,
    };

    /// 一个 GPU 驱动剔除/绘制批次所属的 pass 域(驱动 cull kernel 的 geometry_mask)。
    /// 原先与已删除的 RenderObjectExtractor 同住一个头文件;摄取层是死代码,但本枚举
    /// 是活的(ForwardMesh / DeferredGBuffer / Highlight 的 CullCompactParams 在用),
    /// 故移到它真正的同族这里——EGeometryKind / EPassBit / PassMask 旁边。
    enum class EPassDomain : uint8_t
    {
        DepthPrepass = 0,
        Shadow,
        GBuffer,
        DeferredLighting,
        ForwardOpaque,
        ForwardTransparent,
        Emissive,
        Velocity,
        Custom,
    };

    enum EPassBit : uint16_t
    {
        eBasePass = 1u << 0,
        eShadow = 1u << 1,
        eGBuffer = 1u << 2,
        eDepthPrepass = 1u << 3,
        eEmissive = 1u << 4,
        eTransparent = 1u << 5,
    };

    using PassMask = uint16_t;

    inline constexpr PassMask operator|(EPassBit a, EPassBit b) noexcept
    {
        return static_cast<PassMask>(a) | static_cast<PassMask>(b);
    }

    inline constexpr PassMask operator|(PassMask mask, EPassBit bit) noexcept
    {
        return static_cast<PassMask>(mask | static_cast<PassMask>(bit));
    }

    inline constexpr bool hasPass(PassMask mask, EPassBit bit) noexcept
    {
        return (mask & static_cast<PassMask>(bit)) != 0;
    }

    inline constexpr PassMask kPassMaskOpaqueDefault =
        static_cast<PassMask>(eBasePass | eGBuffer | eDepthPrepass | eShadow);

    // ── 每实例标志位 ────────────────────────────────────────────────────────
    //
    // 此前定义在 comm/RenderProtocol.hpp 里。它们描述的是**网格实例的语义**,
    // 不是线协议机制 —— 放在协议头里,会让只想要这几个位的特性操作头(如
    // HighlightOperation.hpp)去包含整个线协议头。这里才是它们的家:实例语义
    // 与 EGeometryKind / PassMask 同处。
    //
    // 位 0-2 归核心;位 3+ 保留给 RenderFeature —— 特性自定义常量,由
    // gameplay/客户端侧 OR 进同一个 flags 字,核心从不解码(它们不透明地随行)。
    // 把领域标志挡在核心之外,才使得非编辑器 / 2D / 极简消费者能用这套协议而
    // 不必背上特性语义。
    //
    // ── feature 位登记表(声明新位前先登记,并在 feature 头对核心位加
    //    static_assert 不相交;G-buffer ext-flag 撞位事故的教训)──
    //    bit 3  kInstanceFlagHighlight          (HighlightOperation.hpp)
    //    bit 4  kInstanceFlagStreamingFeedback  (StreamingFeedbackOperation.hpp)
    //    bit 4+ 空闲
    inline constexpr uint32_t kInstanceFlagCastShadow = 1u << 0;
    inline constexpr uint32_t kInstanceFlagReceiveShadow = 1u << 1;
    inline constexpr uint32_t kInstanceFlagVisible = 1u << 2;
    /// feature 位起点:bit 0-2 归核心,feature 位从此起。
    inline constexpr uint32_t kInstanceFlagFirstFeatureBit = 3u;
    //
    // (曾在此处的 ERenderFlag 枚举已删 —— 零消费者的死代码,连同其
    //  MotionVector/EditorOnly 位一并清除。)

    struct RenderObjectHandle
    {
        uint32_t index{~0u};
        uint32_t gen{0};

        [[nodiscard]] static constexpr RenderObjectHandle invalid() noexcept
        {
            return {};
        }
        [[nodiscard]] explicit constexpr operator bool() const noexcept
        {
            return index != ~0u;
        }
        friend auto operator<=>(RenderObjectHandle, RenderObjectHandle) = default;
    };

} // namespace lux::render
