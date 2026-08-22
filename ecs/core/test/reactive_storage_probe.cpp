/**
 * @file reactive_storage_probe.cpp
 * @brief R1a —— 反应式抽取改造的**前置验证**（设计见
 *        `.internal/reactive-extraction-design.zh-CN.md` §5.4）。
 *
 * 这个探针不测业务，它测的是**结构选型的前提**：EnTT 3.16 的 reactive storage
 * 在本仓的 registry 上，是否满足设计依赖的每一条性质。选型建立在这些性质上，
 * 所以它们必须是**被断言的**，而不是被相信的 —— 任何一条将来被 EnTT 改掉，
 * 这里会先红，而不是等到某个东西悄悄不渲染。
 *
 * 逐条对应设计文档：
 *   ① 绑定    —— 本仓 registry 是 `EntityRegistry : public
 *                 EntityRegistryBase`，mixin 与信号都绑这一 canonical base。
 *   ② 累积    —— on_construct / on_update / on_destroy 各自能把实体收进集合
 *   ③ patch   —— `on_update` **只认** patch/replace,直写不发信号
 *                 (CLAUDE.md 已入册的条例,这里给它一个实证锚点)
 *   ④ 自动清理 —— `registry.destroy(e)` 会不会把 e 从 reactive storage 里摘掉
 *                 (设计 §5.2 引以为「比自造版更优」的两点之一)
 *   ⑤ 消费时求交 —— `view<Get>(exclude_t<...>)` 在**排空时**判集合归属,
 *                 而不是标记时(§5.2 的另一点)
 *   ⑥ 折入存量 —— 确认 EnTT **不做**,所以 §5.3 的 connectAndSeed 是必需的
 *   ⑦ 复用    —— clear() 之后能继续接收
 *   ⑧ 离场读句柄 —— 普通 `on_destroy` 处理器里组件**仍然可读**
 *                 (CLAUDE.md:EnTT 没有 cleanup 组件留存机制,句柄必须当场读走)
 */

#include <lux/engine/ecs/Registry.hpp>

#include <entt/entt.hpp>

#include <cstdio>
#include <vector>

namespace
{
    int g_failed = 0;

    void check(bool ok, const char* what)
    {
        std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what);
        if (!ok) ++g_failed;
    }

    struct Watched { int v{0}; };
    struct Companion {};
    struct Blocker {};

    /// 离场路径用:句柄住在组件里,实体销毁时必须当场读走(⑧)。
    struct StateWithHandle { int handle{0}; };

    std::vector<int> g_reclaimed;

    void onStateDestroyed(
        lux::ecs::RegistryBase& reg,
        entt::entity e)
    {
        // 关键断言点:信号触发时组件**仍然可读**。
        g_reclaimed.push_back(reg.get<StateWithHandle>(e).handle);
    }
}

int main()
{
    using namespace entt::literals;
    std::printf("=== reactive_storage_probe (R1a) ===\n");

    lux::ecs::Registry reg;

    // ── ① 绑定 ─────────────────────────────────────────────────────────
    // mixin 的 bind_any 与 assure() 都使用 canonical basic_registry base，
    // 但这是整个选型的地基,必须实测。
    auto& changed = reg.storage<entt::reactive>("changed"_hs);
    check(static_cast<bool>(changed),
          "① reactive storage 能在派生 EntityRegistry 上绑定");

    changed.on_construct<Watched>()
           .on_update<Watched>()
           .on_construct<Companion>();

    // ── ⑥ 折入存量:先造两个存量实体,再看连接之后集合是不是空的 ──────────
    const auto pre1 = reg.create();
    const auto pre2 = reg.create();
    reg.emplace<Watched>(pre1, Watched{1});
    reg.emplace<Watched>(pre2, Watched{2});
    // ⚠️ 上面两次 emplace 发生在连接**之后**,所以它们会被收进来。
    //    要验「不折入存量」得用连接**之前**就存在的实体 —— 换一个干净的集合。
    auto& late = reg.storage<entt::reactive>("late"_hs);
    late.on_construct<Watched>();
    check(late.empty(),
          "⑥ EnTT **不**折入存量:连接时已存在的组件不进集合(故需 connectAndSeed)");

    // ── ② 累积:construct ───────────────────────────────────────────────
    check(changed.contains(pre1) && changed.contains(pre2),
          "② on_construct 把实体收进集合");
    const auto n_after_construct = changed.size();

    // ── ③ patch 语义:直写不发信号,patch/replace 才发 ────────────────────
    changed.clear();
    reg.get<Watched>(pre1).v = 42;          // 直写
    check(changed.empty(),
          "③a 直接 `reg.get<T>(e).field = x` **不**触发 on_update");

    reg.patch<Watched>(pre1, [](Watched& w) { w.v = 43; });
    check(changed.contains(pre1),
          "③b `patch<T>()` 触发 on_update");

    changed.clear();
    reg.replace<Watched>(pre2, Watched{7});
    check(changed.contains(pre2),
          "③c `replace<T>()` 触发 on_update");

    // ── ④ 实体销毁自动清理 ─────────────────────────────────────────────
    changed.clear();
    const auto doomed = reg.create();
    reg.emplace<Watched>(doomed, Watched{9});
    check(changed.contains(doomed), "④a 新实体先进集合");
    reg.destroy(doomed);
    check(!changed.contains(doomed),
          "④ `registry.destroy` 自动把实体从 reactive storage 摘掉(无陈旧 id)");

    // ── ⑤ 消费时求交:标记后离开集合的条目在 view 里被跳过 ───────────────
    changed.clear();
    const auto a = reg.create();
    const auto b = reg.create();
    reg.emplace<Watched>(a, Watched{10});
    reg.emplace<Watched>(b, Watched{11});
    reg.emplace<Companion>(a);
    reg.emplace<Companion>(b);
    check(changed.contains(a) && changed.contains(b), "⑤a 两个都被标记");

    // b 在排空**之前**获得了 Blocker —— 消费时应当被排除掉。
    reg.emplace<Blocker>(b);
    {
        std::vector<entt::entity> seen;
        for (auto e : changed.view<Watched, Companion>(entt::exclude<Blocker>))
            seen.push_back(e);
        check(seen.size() == 1 && seen[0] == a,
              "⑤ view<Get>(exclude) 在**消费时**求交:标记后离开集合的被跳过");
    }

    // ── ⑦ clear 之后可复用 ─────────────────────────────────────────────
    changed.clear();
    check(changed.empty(), "⑦a clear 之后为空");
    reg.patch<Watched>(a, [](Watched& w) { w.v = 99; });
    check(changed.contains(a), "⑦ clear 之后信号照常收");

    // ── ⑨ storage 引用的地址稳定性 ─────────────────────────────────────
    // `ExtractionChangeSet` 想在 attach 时把 storage 的地址缓存下来,省掉
    // 每次 view()/clear()/empty() 各一次的 `registry.storage<T>(id)` 哈希查找。
    // 前提是**之后再注册别的 storage 不会搬动已有的那个** —— registry 的池表
    // 是会扩容的 dense_map,那一刻里面的元素会移动。这条不能想当然。
    {
        auto* const before = &reg.storage<entt::reactive>("stability"_hs);
        // 制造足够多的新 storage,逼池表扩容若干次。
        reg.storage<entt::reactive>("pad0"_hs).on_construct<Watched>();
        for (int i = 0; i < 64; ++i)
            (void)reg.storage<entt::reactive>(
                static_cast<entt::id_type>(0x5000 + i));
        auto* const after = &reg.storage<entt::reactive>("stability"_hs);
        check(before == after,
              "⑨ 池表扩容后 storage 地址不变 —— 可以在 attach 时缓存引用");
    }

    // ── ⑧ 离场:on_destroy 里组件仍然可读 ───────────────────────────────
    reg.on_destroy<StateWithHandle>().connect<&onStateDestroyed>();
    const auto owner = reg.create();
    reg.emplace<StateWithHandle>(owner, StateWithHandle{1234});
    g_reclaimed.clear();
    reg.destroy(owner);
    check(g_reclaimed.size() == 1 && g_reclaimed[0] == 1234,
          "⑧ `on_destroy` 处理器里组件仍可读 —— 离场句柄能当场读走");

    // 组件被单独摘掉(实体还活着)也走同一条信号。
    const auto owner2 = reg.create();
    reg.emplace<StateWithHandle>(owner2, StateWithHandle{5678});
    g_reclaimed.clear();
    reg.remove<StateWithHandle>(owner2);
    check(g_reclaimed.size() == 1 && g_reclaimed[0] == 5678,
          "⑧b 组件被摘掉(实体仍在)同样触发,句柄同样可读");

    (void)n_after_construct;

    std::printf("=== %s (%d failed) ===\n", g_failed ? "FAILED" : "PASSED", g_failed);
    return g_failed == 0 ? 0 : 1;
}
