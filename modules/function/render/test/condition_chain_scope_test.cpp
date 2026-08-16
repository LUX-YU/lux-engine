// ============================================================================
//  condition_chain_scope_test.cpp — PERMANENT (cpu tier): RGBuilder 的条件链
//  作用域,把「链的成员资格」从约定变成结构。
//
//  条件链的语义是「这几个 pass 要么全跑、要么全不跑」—— 链内的 transient 每帧
//  UNDEFINED 起手,布局链才自洽。此前每个成员各写一遍 `.setCondition(cond, tag)`:
//  链的边界要数遍那六处调用才知道,漏挂一个只会在编译期被 classifyElectivePasses
//  兜底捕获(而那是**下游**的检查,报的是"这一组不封闭",不是"你漏了一个")。
//
//  作用域接管之后,漏挂在结构上不可能。这个测试钉的正是那句"不可能":
//    · 作用域内建的 pass 全部拿到同一个 tag 与同一个条件;
//    · 作用域外的 pass 一个都不沾;
//    · 两条先后开闭的链拿到不同的 tag(编译器按 tag 分组,撞了就会把两条链
//      判成一条,而那是静默的错误结果而非报错)。
//
//  无 Vulkan、无设备:RGBuilder 建的是纯描述数据。
// ============================================================================

#include <lux/engine/render/graph/RGBuilder.hpp>
#include <lux/engine/render/graph/RGPassTypes.hpp>

#include <cassert>
#include <cstdio>

using namespace lux::render;

namespace
{
    [[nodiscard]] const RGPassDescription* findPass(const RGGraphDescription& g, std::string_view name)
    {
        for (const auto& p : g.passes)
            if (p.name == name)
                return &p;
        return nullptr;
    }
} // namespace

int main()
{
    // ── 1. 作用域内自动入链,作用域外不沾 ────────────────────────────────
    RGBuilder builder;

    (void)builder.addPass("BeforeChain", ERGPassType::COMPUTE);

    std::uint64_t first_tag = 0;
    {
        auto chain = builder.conditionChain([] { return true; });
        first_tag = chain.tag();
        assert(first_tag != 0 && "链标签必须非零 —— 0 是「不属于任何链」的哨兵");

        (void)builder.addPass("InChainA", ERGPassType::COMPUTE);
        (void)builder.addPass("InChainB", ERGPassType::GRAPHICS);
    }

    (void)builder.addPass("AfterChain", ERGPassType::COMPUTE);

    // ── 2. 第二条链必须拿到不同的 tag ────────────────────────────────────
    std::uint64_t second_tag = 0;
    {
        auto chain = builder.conditionChain([] { return false; });
        second_tag = chain.tag();
        (void)builder.addPass("SecondChain", ERGPassType::COMPUTE);
    }

    assert(second_tag != first_tag &&
           "两条链的 tag 撞了 —— 编译器按 tag 分组,会把它们判成同一条链");

    const RGGraphDescription graph = std::move(builder).build();

    const auto* before  = findPass(graph, "BeforeChain");
    const auto* in_a    = findPass(graph, "InChainA");
    const auto* in_b    = findPass(graph, "InChainB");
    const auto* after   = findPass(graph, "AfterChain");
    const auto* second  = findPass(graph, "SecondChain");
    assert(before && in_a && in_b && after && second);

    // 链外:既没有条件也没有标签。
    assert(before->condition_tag == 0 && !before->condition);
    assert(after->condition_tag  == 0 && !after->condition);

    // 链内:同一个标签,而且条件真的挂上了(不是只有标签)。
    assert(in_a->condition_tag == first_tag);
    assert(in_b->condition_tag == first_tag);
    assert(in_a->condition && in_a->condition() == true);
    assert(in_b->condition && in_b->condition() == true);

    // 第二条链拿的是自己的条件,不是第一条的。
    assert(second->condition_tag == second_tag);
    assert(second->condition && second->condition() == false);

    // ── 3. 显式 setCondition 仍可覆盖(作用域只负责默认值)──────────────
    RGBuilder override_builder;
    {
        auto chain = override_builder.conditionChain([] { return true; });
        override_builder.addPass("Overridden", ERGPassType::COMPUTE)
            .setCondition([] { return false; }, 0xABCDu);
    }
    const RGGraphDescription overridden = std::move(override_builder).build();
    const auto* ov = findPass(overridden, "Overridden");
    assert(ov && ov->condition_tag == 0xABCDu);
    assert(ov->condition && ov->condition() == false);

    std::printf("condition_chain_scope_test: OK\n");
    return 0;
}
