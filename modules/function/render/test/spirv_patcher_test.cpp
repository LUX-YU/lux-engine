// =============================================================================
//  spirv_patcher_test — SpirvPatcher self-verification (pure CPU, no GPU needed)
// -----------------------------------------------------------------------------
//  This test actually moves locations around. Production's current allocation
//  strategy is identity, so the patcher is a no-op on the real path -- if the
//  test also only fed it identity relocations, that would be equivalent to
//  verifying nothing, and the problem would only surface once a real
//  allocation strategy replaces it. So this test uses a synthetic relocation
//  plan to move resources elsewhere, then checks the result byte-for-byte.
//
//  Besides the synthetic module, it also scans the actual compiled .spv files
//  in the build tree and verifies:
//    (1) the patcher can parse every real module (it won't choke on some
//        unfamiliar instruction);
//    (2) relocating and then reverse-relocating restores the module
//        byte-for-byte -- the strongest evidence that it "only changes
//        literals, never touches structure."
// =============================================================================
#include <lux/engine/render/gpu/pipeline/SpirvPatcher.hpp>
#include <lux/engine/render/gpu/pipeline/EngineSetShapes.hpp> // relocationsFor (the name-driven relocation table)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

using namespace lux::render;

namespace
{
    int g_failures = 0;

    void check(bool cond, const char* what)
    {
        if (!cond)
        {
            std::printf("  [FAIL] %s\n", what);
            ++g_failures;
        }
    }

    constexpr uint32_t kMagic = 0x07230203u;

    /// Assemble a minimal but valid SPIR-V module: a 5-word header + a handful
    /// of OpDecorate instructions. Deliberately order a variable's Binding
    /// decoration BEFORE its DescriptorSet decoration -- the order of the two
    /// isn't guaranteed, and a single-pass implementation would trip up here.
    std::vector<uint32_t> makeModule()
    {
        auto decorate = [](std::vector<uint32_t>& w, uint32_t target, uint32_t decoration, uint32_t literal) {
            w.push_back((4u << 16) | 71u); // wordcount=4, OpDecorate
            w.push_back(target);
            w.push_back(decoration);
            w.push_back(literal);
        };

        std::vector<uint32_t> w{kMagic, 0x00010300u, 0u, 100u, 0u};

        // %10 : set 2, binding 0   (binding decoration appears first)
        decorate(w, 10, 33, 0);
        decorate(w, 10, 34, 2);
        // %11 : set 7, binding 0
        decorate(w, 11, 34, 7);
        decorate(w, 11, 33, 0);
        // %12 : set 1, binding 3 -- not in the relocation plan, must stay byte-for-byte unchanged
        decorate(w, 12, 34, 1);
        decorate(w, 12, 33, 3);
        // %13 : has only a Binding decoration, no DescriptorSet (not a descriptor resource), must be skipped
        decorate(w, 13, 33, 5);
        // An unrelated decoration (NonWritable=24, carries no positional semantics), must stay unchanged
        w.push_back((3u << 16) | 71u);
        w.push_back(14);
        w.push_back(24);

        return w;
    }

    /// Read a variable's current (set, binding); returns false if not found.
    bool readPos(const std::vector<uint32_t>& w, uint32_t target, uint32_t& set, uint32_t& binding)
    {
        bool hs = false, hb = false;
        for (std::size_t i = 5; i < w.size();)
        {
            const uint32_t wc = w[i] >> 16;
            const uint32_t op = w[i] & 0xFFFFu;
            if (wc == 0)
                return false;
            if (op == 71u && wc >= 4 && w[i + 1] == target)
            {
                if (w[i + 2] == 34u)
                {
                    set = w[i + 3];
                    hs = true;
                }
                if (w[i + 2] == 33u)
                {
                    binding = w[i + 3];
                    hb = true;
                }
            }
            i += wc;
        }
        return hs && hb;
    }

    void testSyntheticRelocation()
    {
        std::printf("[synthetic] 搬运 + 不相干资源保持不动\n");

        auto w = makeModule();
        const auto original = w;

        const SpirvRelocation relocs[] = {
            {2, 0, 0, 4}, // %10 : set2/b0 → set0/b4
            {7, 0, 0, 5}, // %11 : set7/b0 → set0/b5
        };

        auto r = patchSpirvDescriptorPositions(std::span<uint32_t>{w}, relocs);
        check(r.ok, "解析成功");
        check(r.relocated == 2, "搬运计数为 2");

        uint32_t s = 0, b = 0;
        check(readPos(w, 10, s, b) && s == 0 && b == 4, "%10 落到 set0/b4");
        check(readPos(w, 11, s, b) && s == 0 && b == 5, "%11 落到 set0/b5");
        check(readPos(w, 12, s, b) && s == 1 && b == 3, "%12 未被波及");
        check(w.size() == original.size(), "字数不变(只改字面量)");

        // %13 (Binding-only) and the unrelated decoration must stay byte-for-byte unchanged.
        check(
            w[original.size() - 3] == original[original.size() - 3] &&
                w[original.size() - 2] == original[original.size() - 2] &&
                w[original.size() - 1] == original[original.size() - 1],
            "非位置装饰逐字不动"
        );
    }

    void testOrderIndependence()
    {
        std::printf("[synthetic] 两趟实现:先出现的 Binding 不会用旧 set 匹配\n");

        // %10 is set2/b0. If the implementation is single-pass and reads Binding
        // before Set has been read (defaulting to 0), it would incorrectly
        // match the {0,0} relocation.
        auto w = makeModule();
        const SpirvRelocation trap[] = {{0, 0, 6, 6}}; // trap: matches set0/b0

        auto r = patchSpirvDescriptorPositions(std::span<uint32_t>{w}, trap);
        check(r.ok, "解析成功");
        check(r.relocated == 0, "没有任何资源被误搬(%10 是 set2/b0,不是 set0/b0)");
    }

    void testIdentityIsNoOp()
    {
        std::printf("[synthetic] 恒等搬运既不写也不计数\n");

        auto w = makeModule();
        const auto original = w;
        const SpirvRelocation identity[] = {{2, 0, 2, 0}, {7, 0, 7, 0}};

        auto r = patchSpirvDescriptorPositions(std::span<uint32_t>{w}, identity);
        check(r.ok, "解析成功");
        check(r.relocated == 0, "恒等不计入搬运数");
        check(w == original, "字节逐字不变");
    }

    void testRejectsGarbage()
    {
        std::printf("[synthetic] 坏输入一律拒绝且不改动输入\n");

        std::vector<uint32_t> too_short{kMagic, 0, 0};
        auto r1 = patchSpirvDescriptorPositions(std::span<uint32_t>{too_short}, {});
        check(!r1.ok, "过短模块被拒");

        std::vector<uint32_t> bad_magic{0xDEADBEEFu, 0, 0, 0, 0};
        const SpirvRelocation rel[] = {{0, 0, 1, 1}};
        auto r2 = patchSpirvDescriptorPositions(std::span<uint32_t>{bad_magic}, rel);
        check(!r2.ok, "magic 不对被拒");

        // word_count == 0 would spin the linear scan in place forever -- must be treated as corrupt.
        std::vector<uint32_t> zero_wc{kMagic, 0x00010300u, 0u, 10u, 0u, 0u};
        auto r3 = patchSpirvDescriptorPositions(std::span<uint32_t>{zero_wc}, rel);
        check(!r3.ok, "零长指令被判损坏(而不是死循环)");

        std::vector<uint32_t> swapped{0x03022307u, 0, 0, 0, 0};
        auto r4 = patchSpirvDescriptorPositions(std::span<uint32_t>{swapped}, rel);
        check(!r4.ok, "反字节序模块被拒");

        // On the failure path, the copying overload must leave out == input.
        std::vector<uint32_t> out;
        auto r5 = patchSpirvDescriptorPositions(std::span<const uint32_t>{bad_magic}, rel, out);
        check(!r5.ok && out == bad_magic, "失败时拷贝版保持输入原样");
    }

    /// Find real compiled .spv files in the build tree. Skip if none are found
    /// (so the test can still run in a clean tree).
    ///
    /// `limit == 0` means no cap -- contract-completeness reconciliation must
    /// be exhaustive: truncating the scan would turn "reconciliation passed"
    /// into merely "the part we scanned passed", and the shaders that slip
    /// through are exactly the ones nobody looks at often (caster / skinning
    /// shaders). The round-trip test can sample (it's verifying the parser's
    /// general robustness); reconciliation cannot.
    std::vector<std::filesystem::path> findRealSpv(std::size_t limit = 0)
    {
        std::vector<std::filesystem::path> found;
        namespace fs = std::filesystem;

        // Locate the CMake build root first, then scan it exactly once. Scanning
        // the first descendant directory that happens to contain SPIR-V made
        // this test depend on directory layout: a local descriptor-free test
        // shader could hide every production shader in sibling directories.
        fs::path base = fs::current_path();
        fs::path build_root;
        for (int up = 0; up < 8 && !base.empty(); ++up)
        {
            if (fs::exists(base / "CMakeCache.txt"))
            {
                build_root = base;
                break;
            }
            base = base.parent_path();
        }
        if (build_root.empty())
            build_root = fs::current_path();

        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(build_root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator();
             it.increment(ec))
        {
            if (ec)
            {
                ec.clear();
                continue;
            }
            if (it->is_regular_file(ec) && it->path().extension() == ".spv")
            {
                found.push_back(it->path());
                if (limit != 0 && found.size() >= limit)
                    return found;
            }
        }
        return found;
    }

    void testRealShadersRoundTrip()
    {
        const auto files = findRealSpv(40); // sampling is enough here: this verifies the parser's general robustness
        if (files.empty())
        {
            std::printf("[real] 构建树里没找到 .spv —— 跳过(不算失败)\n");
            return;
        }
        std::printf("[real] 对 %zu 个真实模块做搬运/回搬往返\n", files.size());

        uint32_t parsed = 0, moved_total = 0;
        for (const auto& f : files)
        {
            std::ifstream in(f, std::ios::binary);
            if (!in)
                continue;
            std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (bytes.size() < 20 || bytes.size() % 4 != 0)
                continue;

            std::vector<uint32_t> words(bytes.size() / 4);
            std::memcpy(words.data(), bytes.data(), bytes.size());
            if (words[0] != kMagic)
                continue;

            const auto original = words;

            // Move binding0 of set0..7 all into an unused set (15), then move them straight back.
            std::vector<SpirvRelocation> forward, backward;
            for (uint32_t s = 0; s < 8; ++s)
            {
                forward.push_back({s, 0u, 15u, s});
                backward.push_back({15u, s, s, 0u});
            }

            auto r1 = patchSpirvDescriptorPositions(std::span<uint32_t>{words}, forward);
            if (!r1.ok)
            {
                std::printf("  [FAIL] 解析失败 %s : %s\n", f.filename().string().c_str(), r1.error ? r1.error : "?");
                ++g_failures;
                continue;
            }
            ++parsed;
            moved_total += r1.relocated;

            auto r2 = patchSpirvDescriptorPositions(std::span<uint32_t>{words}, backward);
            check(r2.ok, "回搬解析成功");

            if (words != original)
            {
                std::printf("  [FAIL] 往返未还原:%s\n", f.filename().string().c_str());
                ++g_failures;
            }
        }
        std::printf("  解析 %u 个模块,累计搬运 %u 处,全部往返还原\n", parsed, moved_total);
        check(parsed > 0, "至少解析了一个真实模块");
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Contract-completeness reconciliation (closing the loop, part 2)
    //
    //  Move two rules normally enforced by a runtime gate forward to build
    //  time, and run them over EVERY real compiled artifact:
    //    (1) Partial-relocation rule: if any binding in a set matches a
    //        contract resource (engine_set), every other binding in that set
    //        must match too -- an unregistered name gets called out by name
    //        right here, instead of only being discovered when that pipeline
    //        switches over and gets rejected by the runtime gate;
    //    (2) Position rule: for a binding that matches the contract, its
    //        declared binding number must equal canonical_binding (the set
    //        number may legitimately drift via macro overrides, the binding
    //        number may not). Exceptions go through an explicit exemption
    //        table: the same logical resource legitimately lives at a
    //        different position within a feature's explicit set (e.g.
    //        MeshShadow's compact caster set places uShadowSlices at set0
    //        b0). An exemption must record the name, the position, and the
    //        reason -- turning "one name, multiple homes" from something
    //        held in someone's head into a machine-checked registration.
    //
    //  Name source: the .spv's OpName debug info (the same source the
    //  runtime reflection uses) -- self-contained, with no dependency on any
    //  metadata file.
    // ─────────────────────────────────────────────────────────────────────

    struct DeclaredBinding
    {
        uint32_t set{0};
        uint32_t binding{0};
        std::string name;
    };

    /// Parse every variable in a module that carries (set, binding) decorations,
    /// along with its OpName.
    std::vector<DeclaredBinding> parseDeclaredBindings(const std::vector<uint32_t>& w)
    {
        constexpr uint32_t kOpName = 5u;
        constexpr uint32_t kOpDecorate = 71u;
        constexpr uint32_t kDecoBinding = 33u, kDecoSet = 34u;

        struct Pos
        {
            uint32_t set{0}, binding{0};
            bool hs{false}, hb{false};
        };
        std::unordered_map<uint32_t, Pos> pos;
        std::unordered_map<uint32_t, std::string> names;

        for (std::size_t i = 5; i < w.size();)
        {
            const uint32_t wc = w[i] >> 16;
            const uint32_t op = w[i] & 0xFFFFu;
            if (wc == 0 || i + wc > w.size())
                break;

            if (op == kOpName && wc >= 3)
            {
                const char* s = reinterpret_cast<const char*>(&w[i + 2]);
                const std::size_t max_len = (wc - 2) * 4;
                names[w[i + 1]] = std::string(s, strnlen(s, max_len));
            }
            else if (op == kOpDecorate && wc >= 4)
            {
                if (w[i + 2] == kDecoSet)
                {
                    pos[w[i + 1]].set = w[i + 3];
                    pos[w[i + 1]].hs = true;
                }
                if (w[i + 2] == kDecoBinding)
                {
                    pos[w[i + 1]].binding = w[i + 3];
                    pos[w[i + 1]].hb = true;
                }
            }
            i += wc;
        }

        std::vector<DeclaredBinding> out;
        for (const auto& [id, p] : pos)
            if (p.hs && p.hb)
            {
                const auto it = names.find(id);
                out.push_back({p.set, p.binding, it != names.end() ? it->second : std::string{}});
            }
        return out;
    }

    /// Explicit exemption table for "one name, multiple homes": cases where a
    /// contract resource legitimately appears at a non-canonical binding (in
    /// the context of a feature's explicit set). Every entry must spell out
    /// the reason -- adding an exemption means someone made a deliberate
    /// decision to allow a new case of multiple homes, rather than letting it
    /// happen silently.
    struct ContractExemption
    {
        const char* name;
        uint32_t declared_binding;
        const char* reason;
    };
    constexpr ContractExemption kContractExemptions[] = {
        {"uShadowSlices",
         0,
         "MeshShadow 系紧凑 caster set(feature 显式声明)把 slice 表放自己 set 的 b0;"
         "Light set 里的 canonical 位置是 b4,两处是同一数据的两个合法住址"},
    };

    [[nodiscard]] bool isExempt(const std::string& name, uint32_t declared_binding)
    {
        for (const auto& e : kContractExemptions)
            if (name == e.name && declared_binding == e.declared_binding)
                return true;
        return false;
    }

    void testContractCompleteness()
    {
        const auto files = findRealSpv();
        if (files.empty())
        {
            std::printf("[contract] 构建树里没找到 .spv —— 跳过(不算失败)\n");
            return;
        }
        std::printf("[contract] 对 %zu 个真实模块做契约完备性对账\n", files.size());

        uint32_t scanned = 0, engine_sets_seen = 0;
        for (const auto& f : files)
        {
            std::ifstream in(f, std::ios::binary);
            if (!in)
                continue;
            std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (bytes.size() < 20 || bytes.size() % 4 != 0)
                continue;
            std::vector<uint32_t> words(bytes.size() / 4);
            std::memcpy(words.data(), bytes.data(), bytes.size());
            if (words[0] != kMagic)
                continue;

            const auto decls = parseDeclaredBindings(words);
            if (decls.empty())
                continue;
            ++scanned;

            // Reconcile grouped by set.
            for (const auto& d : decls)
            {
                // Process each set only once, when its first binding is encountered.
                bool first_of_set = true;
                for (const auto& other : decls)
                    if (other.set == d.set && (&other != &d) && (other.binding < d.binding))
                    {
                        first_of_set = false;
                        break;
                    }
                if (!first_of_set)
                    continue;

                std::size_t total = 0, hits = 0;
                for (const auto& other : decls)
                    if (other.set == d.set)
                    {
                        ++total;
                        if (engineOwnedResource(other.name))
                            ++hits;
                    }
                if (hits == 0)
                    continue; // pure private/feature set, not the contract's concern
                ++engine_sets_seen;

                // Rule 1: partial registration is a failure, called out by name.
                if (hits < total)
                    for (const auto& other : decls)
                        if (other.set == d.set && !engineOwnedResource(other.name))
                        {
                            std::printf(
                                "  [FAIL] %s : set%u b%u '%s' 与契约资源同住"
                                "一个 set 却未登记 —— 切该管线会被半搬守门拒绝。"
                                "补进 kLayoutContractV0(engine_set)或改名避让。\n",
                                f.filename().string().c_str(),
                                other.set,
                                other.binding,
                                other.name.c_str()
                            );
                            ++g_failures;
                        }

                // Rule 2: a matching binding's number must equal canonical (or be an explicit exemption).
                for (const auto& other : decls)
                    if (other.set == d.set)
                        if (const auto* e = engineOwnedResource(other.name))
                            if (other.binding != e->canonical_binding && !isExempt(other.name, other.binding))
                            {
                                std::printf(
                                    "  [FAIL] %s : '%s' 声明在 b%u,契约 canonical "
                                    "是 b%u —— 若这是 feature 显式 set 的合法别位,"
                                    "加进 kContractExemptions(写明原因);否则是漂移。\n",
                                    f.filename().string().c_str(),
                                    other.name.c_str(),
                                    other.binding,
                                    e->canonical_binding
                                );
                                ++g_failures;
                            }
            }
        }
        std::printf("  扫描 %u 个带描述符的模块,其中 %u 个引擎 set 视图完成对账\n", scanned, engine_sets_seen);
        check(scanned > 0, "至少扫描了一个真实模块");
    }

    /// [Regression test case] The relocation table must be keyed on the
    /// resource NAME, not on the shader's set number.
    ///
    /// This case was caught from a real shader: DeferredLighting's fragment
    /// shader declares Light at set2 (canonical is 3). Relocating by set
    /// number would treat it as a Texture and move it into the BINDLESS
    /// domain, colliding with an actual Texture -- and that collision
    /// produces NO Vulkan error at runtime whatsoever; the two resources
    /// simply end up reading the same descriptor.
    ///
    /// So this constructs two reflections that both declare set2: one named
    /// uSpotLights (contract canonical set3), one named uTex (contract
    /// canonical set2). They must be relocated to DIFFERENT domains -- an
    /// implementation that relocates by set number would fail this
    /// assertion.
    void testRelocationIsNameDriven()
    {
        std::printf("[name-driven] 同在 set2 的两个资源按名字搬去不同的域\n");

        auto makeInfo = [](const char* name, uint32_t set, uint32_t binding) {
            lux::rdesc::ShaderInfo info{};
            lux::rdesc::DescriptorSetLayoutInfo si{};
            si.set = set;
            lux::rdesc::EDescriptorBindingInfo b{};
            b.set = set;
            b.binding = binding;
            b.name = name;
            si.bindings.push_back(b);
            info.sets.push_back(si);
            return info;
        };

        // uSpotLights is declared at set2 (non-canonical) -- must be recognized by name as belonging to Light.
        const auto light = relocationsFor(makeInfo("uSpotLights", 2, 0));
        check(light.size() == 1, "uSpotLights 产出一条搬运");
        if (light.size() == 1)
        {
            check(
                light[0].to_set == domainSetSlot(lux::rdesc::EBindFrequency::FEATURE),
                "uSpotLights 搬去 FEATURE 域,而不是 shader set 号所指的 BINDLESS"
            );
            check(
                light[0].to_binding == engineSetDomainOffset(3),
                "uSpotLights 的 binding 用 canonical 偏移,不是 shader 局部号"
            );
        }

        // uTex is also declared at set2, but it truly belongs to Texture -- must be relocated to BINDLESS.
        const auto tex = relocationsFor(makeInfo("uTex", 2, 0));
        check(tex.size() == 1, "uTex 产出一条搬运");
        if (tex.size() == 1)
            check(tex[0].to_set == domainSetSlot(lux::rdesc::EBindFrequency::BINDLESS), "uTex 搬去 BINDLESS 域");

        if (light.size() == 1 && tex.size() == 1)
            check(light[0].to_set != tex[0].to_set, "同在 set2 的两者去向不同 —— 按 set 号搬就会在这里撞号");

        // An unregistered name (a single-pipeline private set) produces no relocation.
        check(relocationsFor(makeInfo("uPrivateNotInContract", 1, 0)).empty(), "未登记资源不搬");
    }
} // namespace

int
main()
{
    std::printf("=== SpirvPatcher test ===\n");
    testSyntheticRelocation();
    testOrderIndependence();
    testIdentityIsNoOp();
    testRejectsGarbage();
    testRealShadersRoundTrip();
    testRelocationIsNameDriven();
    testContractCompleteness();

    if (g_failures == 0)
    {
        std::printf("=== PASS ===\n");
        return 0;
    }
    std::printf("=== FAIL (%d) ===\n", g_failures);
    return 1;
}
