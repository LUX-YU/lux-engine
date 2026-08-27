// ============================================================================
//  material_instance_bucket_test — PERMANENT (cpu tier)
//
//  钉住「材质实例」这套设计的**全部价值所在**:同一个父材质的 N 个实例共享
//  一个 variant bucket(= 一个 PSO),各自只占一条参数 lane。
//
//  这条不变式此前一行测试都没有。仓库里唯一相关的 graph_distinct_pso_render_test
//  证的是**反方向**(两个不同材质 → 两个不同 PSO),而"相同父级 → 同一个 PSO"
//  —— 也就是实例相对于"复制一份材质"的唯一好处 —— 只有代码和注释在保证。
//  它坏掉的样子是无声的:画面完全正确,只是每个实例多一次 PSO 切换,N 大了才发现。
//
//  bucket 是 GPU-driven 绘制的 PSO 路由单元:MDC 键是
//  (geometry_kind, bucket_id, section_id),所以同网格同 bucket 的实例会塌进同一条
//  间接绘制,instanceCount = N。bucket 不同就意味着多一条绘制 + 一次管线切换。
//
//  无 Vulkan、无设备:VariantBucketManager 是纯 CPU 的键→id 映射。
// ============================================================================

#include <lux/engine/render/resources/material/VariantBucketManager.hpp>

#include <cstdio>

namespace
{
    int g_failures = 0;

    void check(bool ok, const char* what)
    {
        std::printf("%-62s %s\n", what, ok ? "PASS" : "FAIL");
        if (!ok)
            ++g_failures;
    }

    /// 造一个 ShaderHandle。只要求"同值同句柄、异值异句柄",不碰 GPU。
    lux::render::ShaderHandle shader(std::uint32_t index)
    {
        lux::render::ShaderHandle h{};
        h.index = index;
        return h;
    }
} // namespace

int
main()
{
    using lux::rdesc::EAlphaMode;
    using lux::render::VariantBucketManager;

    VariantBucketManager mgr;
    mgr.seedFamilyBootstrapBuckets();

    const auto gb = shader(11); // 父材质烘焙出的 GBuffer frag
    const auto fw = shader(12); // 同上,Forward
    constexpr std::uint64_t kParentKey = 0xABCDEF01u;

    // ── 1. 同一父级的三个实例 ───────────────────────────────────────────
    //
    // 实例不自带着色器(MaterialInstanceData 里没有任何 SPIR-V 字段),桥把父级
    // 去重后的 ShaderHandle 原样传下来 —— 所以三次调用的实参完全相同。
    const std::uint32_t b0 = mgr.getOrCreateGraph(kParentKey, gb, fw, EAlphaMode::Opaque, false);
    const std::uint32_t b1 = mgr.getOrCreateGraph(kParentKey, gb, fw, EAlphaMode::Opaque, false);
    const std::uint32_t b2 = mgr.getOrCreateGraph(kParentKey, gb, fw, EAlphaMode::Opaque, false);

    check(b0 == b1 && b1 == b2, "同一父级的三个实例共享同一个 bucket(= 同一个 PSO)");

    const std::uint32_t after_three = mgr.count();

    // 再来一次也不该长出新桶 —— 否则实例越多 PSO 越多,等于没有实例这个概念。
    (void)mgr.getOrCreateGraph(kParentKey, gb, fw, EAlphaMode::Opaque, false);
    check(mgr.count() == after_three, "重复取同一父级的桶不会增加 PSO 数量");

    // ── 2. 不同父级 → 不同 bucket ───────────────────────────────────────
    //
    // 这是 graph_distinct_pso_render_test 在 GPU 上证过的方向,这里在键这一层
    // 再钉一次:两条断言必须同时成立,只有 1 没有 2 的话"全都塌成一个桶"也能过。
    const auto other_gb = shader(21);
    const auto other_fw = shader(22);
    const std::uint32_t other = mgr.getOrCreateGraph(0x12345678u, other_gb, other_fw, EAlphaMode::Opaque, false);
    check(other != b0, "不同父材质拿到不同的 bucket");

    // ── 3. 渲染状态覆盖会分桶(这是实例唯一会额外要一个 PSO 的情形)────
    //
    // alpha_mode / double_sided 烧在 PSO 里,Vulkan 层面绕不开。
    // MaterialInstanceData::render_state_override 默认 0 = 继承父级,正是为此。
    const std::uint32_t blend = mgr.getOrCreateGraph(kParentKey, gb, fw, EAlphaMode::Blend, false);
    check(blend != b0, "实例覆盖 alpha_mode 会分出自己的 bucket");

    const std::uint32_t two_sided = mgr.getOrCreateGraph(kParentKey, gb, fw, EAlphaMode::Opaque, true);
    check(two_sided != b0, "实例覆盖 double_sided 会分出自己的 bucket");
    check(two_sided != blend, "两种渲染状态覆盖彼此也不同桶");

    // ── 4. 释放到零之后 id 可回收,但不影响仍在引用的 ───────────────────
    //
    // b0/b1/b2 是同一个 id 上的三次引用。放掉两次,桶必须还在 —— 否则一个实体
    // 删掉会把兄弟实体的 PSO 一起带走。
    mgr.release(b0);
    mgr.release(b1);
    const std::uint32_t still = mgr.getOrCreateGraph(kParentKey, gb, fw, EAlphaMode::Opaque, false);
    check(still == b0, "放掉部分引用后,其余实例仍指向同一个存活的 bucket");

    if (g_failures != 0)
    {
        std::fprintf(
            stderr,
            "\n%d 项失败。第 1/2 组失败意味着实例不再共享 PSO —— "
            "N 个实例会变成 N 次管线切换,材质实例这个概念也就失去了意义。\n",
            g_failures
        );
        return 1;
    }
    std::puts("\nmaterial_instance_bucket_test PASSED");
    return 0;
}
