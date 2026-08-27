// ============================================================================
//  feature_level_test.cpp — PERMANENT (cpu tier):特性等级协商的判据表。
//
//  为什么它值得一个常驻测试:代码质量审计的 R11 指出,**这套协商机制零使用者**
//  —— 全仓没有一个 feature 声明 level_profiles,kDevFeat* 位也无人引用,于是
//  capsSatisfy / achievableFeatureLevel / 装载期的等级校验全是**从未执行过的
//  代码**。第一个真正挂上 profile 的特性会踩一条没人走过的路。
//
//  这个测试就是那条路的替身:它不需要设备(DeviceCaps 是纯数据),把三件事钉住
//    · 位 → DeviceCaps 成员的对应关系(表驱动之后只有一份,但仍要证明它对);
//    · 成对消费的位(BDA 要求 bufferDeviceAddress 与 shaderInt64 同时启用)——
//      这是唯一一个"一位两成员"的条目,最容易在改表时漏掉第二个;
//    · unmetDeviceFeatures 报的是**缺了哪些**,不是"要求了哪些" —— 装载被拒时
//      调用方要的是"缺什么",而不是把要求再抄一遍给它。
//
//  等级公式本身也钉:wideLines 故意不参与分级(它是编辑器 gizmo 的装饰位,
//  不卡任何结构),漏进公式会让一台桌面设备因为一个装饰特性掉档。
// ============================================================================

#include <lux/engine/function/render/client/core/EFeatureLevel.hpp>

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace lux::render;

namespace
{
    /// 全都启用的设备(桌面基线)。
    [[nodiscard]] constexpr DeviceCaps desktopCaps() noexcept
    {
        DeviceCaps c{};
        c.draw_indirect_count = true;
        c.shader_output_layer = true;
        c.buffer_device_address = true;
        c.shader_int64 = true;
        c.wide_lines = true;
        c.dynamic_rendering_local_read = true;
        return c;
    }
} // namespace

int
main()
{
    // ── 1. 每个位都真的映到它该映的 caps 成员 ─────────────────────────────
    // 逐位关掉一项,断言恰好这一位不满足、其余位不受影响。
    // 这是"表填错了行"唯一能被抓住的地方 —— 表本身的 static_assert 只查
    // 覆盖度,查不出把 wideLines 写成 shaderInt64 这种错。
    {
        struct Probe
        {
            std::uint32_t bit;
            bool DeviceCaps::* off;
        };
        constexpr Probe probes[]{
            {kDevFeatDrawIndirectCount, &DeviceCaps::draw_indirect_count},
            {kDevFeatShaderOutputLayer, &DeviceCaps::shader_output_layer},
            {kDevFeatWideLines, &DeviceCaps::wide_lines},
            {kDevFeatDynamicRenderingLocalRead, &DeviceCaps::dynamic_rendering_local_read},
        };
        constexpr std::uint32_t all_bits = (1u << kDevFeatDeclaredCount) - 1u;

        for (const auto& p : probes)
        {
            DeviceCaps caps = desktopCaps();
            caps.*(p.off) = false;

            assert(!capsSatisfy(caps, p.bit) && "关掉了这一位对应的成员,它却仍报满足");
            assert(unmetDeviceFeatures(caps, all_bits) == p.bit && "关掉一个成员应当且只应当让一个位不满足");
            assert(capsSatisfy(caps, all_bits & ~p.bit) && "其余位不该受这个成员影响");
        }
    }

    // ── 2. 成对消费的位:BDA 要两个成员同时启用 ───────────────────────────
    // buffer_reference 的 SPIR-V 声明 Int64,所以一个位盖住两个成员。
    // 只关掉其中任意一个都必须让该位不满足。
    {
        for (bool DeviceCaps::* off : {&DeviceCaps::buffer_device_address, &DeviceCaps::shader_int64})
        {
            DeviceCaps caps = desktopCaps();
            caps.*off = false;
            assert(!capsSatisfy(caps, kDevFeatBufferDeviceAddress) && "BDA 位要求两个成员同时启用,关掉其一就该不满足");
            assert(unmetDeviceFeatures(caps, kDevFeatBufferDeviceAddress) == kDevFeatBufferDeviceAddress);
        }
    }

    // ── 3. 空掩码恒满足;全空设备一个都不满足 ─────────────────────────────
    {
        const DeviceCaps none{};
        assert(capsSatisfy(none, 0) && "空要求应当恒满足");
        assert(unmetDeviceFeatures(none, 0) == 0);

        constexpr std::uint32_t all_bits = (1u << kDevFeatDeclaredCount) - 1u;
        assert(unmetDeviceFeatures(none, all_bits) == all_bits && "什么都没启用的设备应当每一位都缺");
    }

    // ── 4. 等级公式 ───────────────────────────────────────────────────────
    {
        assert(achievableFeatureLevel(desktopCaps()) == EFeatureLevel::Desktop);

        // wideLines 是装饰位:关掉它**不该**让设备掉档。漏进等级公式的话,
        // 一台桌面设备会因为编辑器 gizmo 的一个位被判成移动端。
        DeviceCaps no_wide = desktopCaps();
        no_wide.wide_lines = false;
        assert(achievableFeatureLevel(no_wide) == EFeatureLevel::Desktop && "wideLines 是装饰位,不该参与分级");

        // local_read 同理:它是变体选择器,不是分级门。
        DeviceCaps no_lr = desktopCaps();
        no_lr.dynamic_rendering_local_read = false;
        assert(achievableFeatureLevel(no_lr) == EFeatureLevel::Desktop);

        // 少了 BDA / shaderOutputLayer 但仍有 drawIndirectCount → MobileHigh。
        DeviceCaps mobile_high = desktopCaps();
        mobile_high.buffer_device_address = false;
        assert(achievableFeatureLevel(mobile_high) == EFeatureLevel::MobileHigh);

        // 连 drawIndirectCount 都没有 → Mobile 地板。
        DeviceCaps mobile = mobile_high;
        mobile.draw_indirect_count = false;
        assert(achievableFeatureLevel(mobile) == EFeatureLevel::Mobile);
    }

    // ── 5. 位 → 诊断名的反查覆盖每一个已声明的位 ──────────────────────────
    // 上报 err::feature::LevelRequirementsUnmet 时消费侧照这张表说人话;
    // 少一个位就会显示成一个裸数字。
    {
        for (std::uint32_t i = 0; i < kDevFeatDeclaredCount; ++i)
        {
            const char* name = deviceFeatureName(1u << i);
            assert(name != nullptr && std::strlen(name) > 0 && "已声明的位在需求表里查不到名字");
        }
        assert(deviceFeatureName(1u << kDevFeatDeclaredCount) == nullptr && "未声明的位不该查出名字");
    }

    std::printf("feature_level_test: OK\n");
    return 0;
}
