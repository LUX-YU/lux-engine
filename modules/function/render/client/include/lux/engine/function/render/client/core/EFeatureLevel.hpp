#pragma once
/**
 * @file EFeatureLevel.hpp
 * @brief Runtime feature-level tiers + the device-feature vocabulary that
 *        decides them (mobile-adaptation topic ①, item 1-3).
 *
 * A tier switches IMPLEMENTATION (visually equivalent variants: BDA cull vs
 * bound-SSBO cull, gl_Layer caster vs multiview); quality params switch
 * EFFECT BUDGET (atlas resolution, cascade count). Keep the two axes apart —
 * that separation is the "editor look survives on the phone" guarantee
 * (investigation §1.3 first principle).
 *
 * The enum is a TOTAL ORDER (comparisons like `level >= MobileHigh` are the
 * intended idiom). The tier threshold sits ABOVE descriptor indexing by
 * design: bindless is core floor on every tier, never negotiated.
 */

#include <lux/engine/function/render/client/core/DeviceCaps.hpp>

#include <cstdint>
#include <iterator>

namespace lux::render
{
    enum class EFeatureLevel : std::uint8_t
    {
        Mobile = 0,     ///< Vulkan 1.3 baseline device (bindless floor, no GPU-driven extras)
        MobileHigh = 1, ///< mobile flagship: GPU-driven kept (drawIndirectCount present)
        Desktop = 2,    ///< current full feature set
    };

    /// Whitelisted (enable-if-present) device features a level profile may
    /// require. Mirrors the gate whitelist in VulkanContext::init —
    /// extend BOTH when adding an entry.
    enum EDeviceFeatureBits : std::uint32_t
    {
        kDevFeatDrawIndirectCount = 1u << 0,
        kDevFeatShaderOutputLayer = 1u << 1,
        /// BDA and shaderInt64 are consumed as a pair (buffer_reference SPIR-V
        /// declares Int64), so one bit covers both.
        kDevFeatBufferDeviceAddress = 1u << 2,
        kDevFeatWideLines = 1u << 3,
        /// Variant selector (not a tier gate): tile-local G-buffer reads.
        kDevFeatDynamicRenderingLocalRead = 1u << 4,

        /// 已声明的位数。**加新位时同时加一** —— 下面的 static_assert 以它为准,
        /// 检查每个位在需求表里都有对应行。忘了加行会编译失败,而不是让
        /// capsSatisfy 对那个位静默放行。
        kDevFeatDeclaredCount = 5,
    };

    /// 一个特性位所要求的 `DeviceCaps` 成员。
    ///
    /// 此前 `capsSatisfy` 是一条五分支的 if 链,`achievableFeatureLevel` 又按
    /// 自己的口径拼掩码 —— 位与 caps 成员的对应关系散在两处,靠"extend BOTH"
    /// 这句注释维持。表驱动之后对应关系只有一份,加位就是加一行。
    struct DeviceFeatureRequirement
    {
        std::uint32_t bit;
        bool DeviceCaps::* member;
        /// 该位要求**同时**启用的第二个成员(null = 只要求一个)。
        bool DeviceCaps::* also{nullptr};
        /// 诊断名。上报 err::feature::LevelRequirementsUnmet 时,消费侧按位
        /// 反查这张表就能说出缺的是哪一项,不必自己维护一份名字表。
        const char* name;
    };

    inline constexpr DeviceFeatureRequirement kDeviceFeatureTable[]{
        {kDevFeatDrawIndirectCount, &DeviceCaps::draw_indirect_count, nullptr, "drawIndirectCount"},
        {kDevFeatShaderOutputLayer, &DeviceCaps::shader_output_layer, nullptr, "shaderOutputLayer"},
        {kDevFeatBufferDeviceAddress,
         &DeviceCaps::buffer_device_address,
         &DeviceCaps::shader_int64,
         "bufferDeviceAddress+shaderInt64"},
        {kDevFeatWideLines, &DeviceCaps::wide_lines, nullptr, "wideLines"},
        {kDevFeatDynamicRenderingLocalRead,
         &DeviceCaps::dynamic_rendering_local_read,
         nullptr,
         "dynamicRenderingLocalRead"},
    };

    namespace detail
    {
        [[nodiscard]] consteval std::uint32_t deviceFeatureTableMask() noexcept
        {
            std::uint32_t mask = 0;
            for (const auto& r : kDeviceFeatureTable)
                mask |= r.bit;
            return mask;
        }
    } // namespace detail

    static_assert(
        detail::deviceFeatureTableMask() == (1u << kDevFeatDeclaredCount) - 1u,
        "EDeviceFeatureBits 与 kDeviceFeatureTable 对不上 —— 加了位没补行"
        "(capsSatisfy 会对它静默放行),或加了行没更新 kDevFeatDeclaredCount。");
    static_assert(std::size(kDeviceFeatureTable) == kDevFeatDeclaredCount, "kDeviceFeatureTable 里有重复位或多余行。");

    /// True iff every feature named in `mask` is enabled on the device.
    [[nodiscard]] constexpr bool capsSatisfy(const DeviceCaps& caps, std::uint32_t mask) noexcept
    {
        for (const auto& req : kDeviceFeatureTable)
        {
            if ((mask & req.bit) == 0)
                continue;
            if (!(caps.*req.member))
                return false;
            if (req.also != nullptr && !(caps.*(req.also)))
                return false;
        }
        return true;
    }

    /// `mask` 里设备没满足的那些位。上报时带上它,消费侧照表就能说出缺了什么。
    [[nodiscard]] constexpr std::uint32_t unmetDeviceFeatures(const DeviceCaps& caps, std::uint32_t mask) noexcept
    {
        std::uint32_t missing = 0;
        for (const auto& req : kDeviceFeatureTable)
        {
            if ((mask & req.bit) == 0)
                continue;
            if (!(caps.*req.member) || (req.also != nullptr && !(caps.*(req.also))))
                missing |= req.bit;
        }
        return missing;
    }

    /// 特性位 → 诊断名。表里没有的位返回 nullptr。
    [[nodiscard]] constexpr const char* deviceFeatureName(std::uint32_t bit) noexcept
    {
        for (const auto& req : kDeviceFeatureTable)
            if (req.bit == bit)
                return req.name;
        return nullptr;
    }

    /// Highest tier this device can reach. wideLines is deliberately NOT part
    /// of the tier formula — it is cosmetic (editor gizmo/grid) and gates
    /// nothing structural; consumers check the bit directly.
    [[nodiscard]] constexpr EFeatureLevel achievableFeatureLevel(const DeviceCaps& caps) noexcept
    {
        if (capsSatisfy(caps, kDevFeatDrawIndirectCount | kDevFeatShaderOutputLayer | kDevFeatBufferDeviceAddress))
            return EFeatureLevel::Desktop;
        if (capsSatisfy(caps, kDevFeatDrawIndirectCount))
            return EFeatureLevel::MobileHigh;
        return EFeatureLevel::Mobile;
    }

    /// One per-tier requirement row a feature type declares (static storage,
    /// referenced by FeatureDescriptor::level_profiles). A feature listing no
    /// profile for the resolved tier is NOT installable at that tier — attach
    /// negotiation (①-4) rejects it; an EMPTY level_profiles span means "any
    /// tier, no extra requirements" (today's behaviour).
    struct FeatureLevelProfile
    {
        EFeatureLevel level;
        std::uint32_t required_features{0}; ///< EDeviceFeatureBits mask on top of the tier floor
    };

} // namespace lux::render
