// ============================================================================
//  emissive_palette_spirv_identity_test — PERMANENT (cpu tier)
//
//  EditorBuiltins 的自发光调色板有 kBuiltinEmissiveCount 个材质,彼此只差三个
//  颜色分量。它们此前各自跑一遍完整的 bake(每个 bake 编两趟:GBuffer +
//  Forward),于是每次启动编器编 16 次 —— VTune 实测那是编辑器启动期最大的一项
//  CPU 开销(shadergen_glsl 1.05s,占整个进程 CPU 的 14.4%,是渲染模块的 6.6 倍)。
//
//  去重的前提是一条不变式:**颜色不进着色器**。它走的是 ParamSlotDecl::dflt,
//  而 MaterialLowering 把 ParamNode 降低成 ShaderIRValue{op=Param, type, slot} ——
//  默认值根本不进 IR,只活在 cooked parameter_defaults 里。
//
//  本测试就钉这条不变式的两半,缺一不可:
//
//    A. 两个不同颜色的图,编出来的 SPIR-V 逐字节相同   → 去重是对的
//    B. 两个 payload 的 Emissive 参数默认值不同 → 颜色没被丢掉
//
//  只测 A 会漏掉"颜色整个消失了"这种把两边都变成一样的坏情况 —— 那时 A 也过。
//  只测 B 说明不了去重安全。
//
//  哪天有人把颜色改成常量节点(或给 Emissive 做了常量折叠),A 会红,而那正是
//  "共享同一份 SPIR-V 不再成立"的信号,该回去把去重撤掉。
//
//  无 Vulkan、无设备:纯 CPU 的 lower + shaderc 编译。
// ============================================================================

#include <lux/engine/toolchain/asset/material/MaterialGraphCompiler.hpp>

#include <lux/engine/authoring/assets/material/MaterialGraph.hpp>

#include <cstdio>
#include <vector>

namespace
{
    int g_failures = 0;

    void check(bool ok, const char* what)
    {
        std::printf("%-58s %s\n", what, ok ? "PASS" : "FAIL");
        if (!ok)
            ++g_failures;
    }

} // namespace

int main()
{
    // 取调色板里差得最远的两个颜色,别用相近色 —— 相近色万一真被折进着色器,
    // 生成的常量也可能因为某种舍入而碰巧一样,那就测不出问题了。
    const auto red   = lux::toolchain::compileGraphToPayload(
        lux::toolchain::makeEmissivePbrGraph(1.0f, 0.0f, 0.0f), /*slot_texture_ids=*/{});
    const auto blue  = lux::toolchain::compileGraphToPayload(
        lux::toolchain::makeEmissivePbrGraph(0.0f, 0.0f, 1.0f), /*slot_texture_ids=*/{});

    if (!red || !blue)
    {
        std::fprintf(stderr, "bake failed: %s\n",
                     !red ? red.error().c_str() : blue.error().c_str());
        return 1;
    }

    check(!red->gbuffer_spirv.empty(), "GBuffer SPIR-V 非空(编译真的跑了)");
    check(!red->forward_spirv.empty(), "Forward SPIR-V 非空");

    // ── A:两个颜色编出同一份 SPIR-V ────────────────────────────────────
    check(red->gbuffer_spirv == blue->gbuffer_spirv,
          "A1 红/蓝的 GBuffer SPIR-V 逐字节相同");
    check(red->forward_spirv == blue->forward_spirv,
          "A2 红/蓝的 Forward SPIR-V 逐字节相同");

    // ── B:颜色确实还在,只是待在图里而不是着色器里 ─────────────────────
    constexpr std::uint32_t kEmissiveSlot = 3u;
    const bool found_r = red->parameter_count > kEmissiveSlot;
    const bool found_b = blue->parameter_count > kEmissiveSlot;
    const auto& red_emi  = red->parameter_defaults[kEmissiveSlot];
    const auto& blue_emi = blue->parameter_defaults[kEmissiveSlot];
    check(found_r && found_b, "B1 两份 payload 的图里都有 Emissive 参数槽");
    check(found_r && found_b &&
          (red_emi[0] != blue_emi[0] || red_emi[2] != blue_emi[2]),
          "B2 Emissive 默认值随颜色不同(颜色没被丢掉)");

    // 反面确认:结构相同的部分也确实相同,免得 A 是因为两次编译都失败成空而过。
    check(red->gbuffer_spirv.size() > 16, "A3 SPIR-V 有实际长度(不是空壳)");

    if (g_failures != 0)
    {
        std::fprintf(stderr,
            "\n%d 项失败。若失败的是 A 组,说明颜色已经会进着色器了 —— "
            "EditorBuiltins::registerEmissivePalette 里的\"编一次复用\"不再成立,"
            "必须改回逐个编译。\n", g_failures);
        return 1;
    }
    std::puts("\nemissive_palette_spirv_identity_test PASSED");
    return 0;
}
