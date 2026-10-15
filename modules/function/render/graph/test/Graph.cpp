#include <lux/engine/render/graph/Builder.hpp>
#include <lux/engine/render/graph/Analysis.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <type_traits>

using namespace lux::render;

void check(bool condition, const char* expression, int line)
{
    if (condition) return;
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, line, expression);
    std::abort();
}

#define CHECK(x) check(static_cast<bool>(x), #x, __LINE__)

template <typename T>
T take(RenderResult<T> result)
{
    if (!result)
    {
        std::fprintf(stderr, "error=%llx pass=%llu resource=%llu detail=%llu\n", result.error().type,
            result.error().args[0], result.error().args[1], result.error().args[2]);
        std::abort();
    }
    return std::move(*result);
}

template <typename T>
void fails(const RenderResult<T>& result, lux::error::ErrorId type)
{
    CHECK(!result);
    CHECK(result.error().type == type);
}

GraphResourceId buffer(RenderGraphBuilder& b, std::string key = "buffer", std::uint64_t size = 64)
{
    return take(b.declareResource({std::move(key), GraphBufferDesc{size}}));
}

GraphResourceUse use(GraphResourceId r, EGraphAccess access, std::uint32_t version = 1, VGraphRange range = {})
{
    return {{r, version}, range, access};
}

GraphPassId pass(RenderGraphBuilder& b, std::string key, std::vector<GraphResourceUse> uses, bool retained = false)
{
    return take(b.addPass({std::move(key), EGraphPassKind::COMPUTE, std::move(uses), retained}));
}

GraphAnalysis finish(RenderGraphBuilder& b)
{
    auto graph = take(std::move(b).finish());
    const auto analysis = take(analyze(graph));
    for (int i = 0; i < 5; ++i) CHECK(take(analyze(graph)) == analysis);
    return analysis;
}

bool edge(const GraphAnalysis& a, GraphPassId from, GraphPassId to)
{
    return std::find(a.dependencies.begin(), a.dependencies.end(), GraphDependency{from, to}) != a.dependencies.end();
}

std::size_t hazards(const GraphAnalysis& a, EGraphHazard kind)
{
    return std::count_if(a.hazards.begin(), a.hazards.end(), [&](const auto& h) { return h.kind == kind; });
}

void authoring()
{
    static_assert(!std::is_convertible_v<GraphPassId, GraphResourceId>);
    static_assert(!std::is_convertible_v<std::uint32_t, GraphPassId>);
    RenderGraphDefinition owned;
    {
        RenderGraphBuilder b;
        std::string name = "owned";
        const auto r = buffer(b, name);
        auto first = pass(b, "chunk-one", {use(r, EGraphAccess::WRITE)});
        auto second = pass(b, "chunk-two", {use(r, EGraphAccess::READ)}, true);
        CHECK(b.dependsOn(first, second));
        CHECK(b.dependsOn(first, second));
        name.assign(500, 'x');
        owned = take(std::move(b).finish());
    }
    CHECK(owned.resources()[0].key == "owned");
    CHECK(owned.dependencies().size() == 1);
    auto moved = std::move(owned);
    CHECK(take(analyze(moved)).execution_order.size() == 2);
    {
        RenderGraphBuilder b;
        buffer(b, "same");
        buffer(b, "same");
        fails(std::move(b).finish(), kGraphDuplicateKey);
    }
    {
        RenderGraphBuilder b;
        pass(b, "same", {});
        pass(b, "same", {});
        fails(std::move(b).finish(), kGraphDuplicateKey);
    }
    RenderGraphBuilder b;
    const auto r = buffer(b);
    const auto p = pass(b, "one", {use(r, EGraphAccess::WRITE)});
    fails(b.dependsOn(p, p), kGraphDependencyCycle);
    fails(b.dependsOn(p, {1}), kGraphUnknownPass);
    fails(b.addPass({"bad", {}, {use({1}, EGraphAccess::READ)}}), kGraphUnknownResource);
    fails(b.addPass({"bad", {}, {use({}, EGraphAccess::READ)}}), kGraphUnknownResource);
    fails(b.exportOutput({{{1}, 1}}), kGraphUnknownResource);
    fails(b.addPass({"duplicate", {}, {use(r, EGraphAccess::WRITE), use(r, EGraphAccess::WRITE)}}),
        kGraphConflictingOutput);
    CHECK(b.exportOutput({{r, 1}}));
    fails(b.exportOutput({{r, 1}}), kGraphConflictingOutput);
    CHECK(finish(b).execution_order == std::vector<GraphPassId>{p});
    CHECK(renderGraphErrorDescriptors().size() == 11);
    for (const auto& descriptor : renderGraphErrorDescriptors())
        CHECK(lux::error::errorId(descriptor.name) != 0 && !descriptor.message.empty());
    // Equal numeric IDs from different owners are explicitly not provenance tokens.
    RenderGraphBuilder other;
    CHECK(buffer(other) == r);
}

void ranges()
{
    RenderGraphBuilder b;
    fails(b.declareResource({"zero", GraphBufferDesc{0}}), kGraphInvalidDescriptor);
    fails(b.declareResource({"bad-image", GraphImageDesc{0, 1}}), kGraphInvalidDescriptor);
    fails(b.declareResource({"bad-mip", GraphImageDesc{8, 8, 5}}), kGraphInvalidDescriptor);
    fails(b.declareResource({"bad-format", GraphImageDesc{1, 1, 1, 1,
        static_cast<lux::rdesc::ETextureFormat>(999)}}), kGraphInvalidDescriptor);
    const auto r = buffer(b);
    for (auto range : {GraphBufferRange{0, 0}, {64, 1}, {63, 2}, {UINT64_MAX, 2}, {1, UINT64_MAX}})
        fails(b.addPass({"bad", {}, {use(r, EGraphAccess::READ, 1, range)}}), kGraphInvalidRange);
    fails(b.addPass({"type", {}, {use(r, EGraphAccess::READ, 1, GraphImageRange{})}}), kGraphInvalidRange);
    fails(b.addPass({"enum", {}, {use(r, static_cast<EGraphAccess>(255))}}), kGraphInvalidDescriptor);
    fails(b.addPass({"initial-write", {}, {use(r, EGraphAccess::WRITE, 0)}}), kGraphInvalidDescriptor);
    fails(b.exportOutput({{r, 1}, GraphBufferRange{0, 0}}), kGraphInvalidRange);
    fails(b.declareResource({"bad-import", GraphBufferDesc{8}, EGraphOrigin::IMPORTED,
        {GraphBufferRange{0, 9}}}), kGraphInvalidRange);
    auto a = pass(b, "left", {use(r, EGraphAccess::WRITE, 1, GraphBufferRange{0, 32})});
    auto c = pass(b, "right", {use(r, EGraphAccess::WRITE, 1, GraphBufferRange{32, 32})});
    auto reader = pass(b, "join", {use(r, EGraphAccess::READ)}, true);
    const auto analysis = finish(b);
    CHECK(edge(analysis, a, reader) && edge(analysis, c, reader));
    CHECK(!edge(analysis, a, c) && !edge(analysis, c, a));
    CHECK(hazards(analysis, EGraphHazard::RAW) == 2);
    CHECK(hazards(analysis, EGraphHazard::WAW) == 0);
    {
        RenderGraphBuilder image_builder;
        const GraphImageDesc desc{8, 8, 4, 2, lux::rdesc::ETextureFormat::D24_UNORM_S8_UINT};
        const auto image = take(image_builder.declareResource({"depth", desc}));
        for (auto range : {GraphImageRange{0}, {1}, {8}, {2, 4, 1}, {2, 0, 0},
                          {2, 0, 1, 2, 1}, {2, UINT32_MAX, 2}, {2, 0, 1, 0, UINT32_MAX}})
        {
            const auto result = image_builder.addPass({"invalid", {}, {use(image, EGraphAccess::READ, 1, range)}});
            fails(result, kGraphInvalidRange);
        }
        const auto depth = pass(image_builder, "depth-write",
            {use(image, EGraphAccess::WRITE, 1, GraphImageRange{2, 0, 2, 0, 2})});
        const auto stencil = pass(image_builder, "stencil-write",
            {use(image, EGraphAccess::WRITE, 1, GraphImageRange{4, 0, 2, 0, 2})});
        const auto mips = pass(image_builder, "later-mips",
            {use(image, EGraphAccess::WRITE, 1, GraphImageRange{6, 2, 2, 0, 2})});
        const auto read = pass(image_builder, "all", {use(image, EGraphAccess::READ)}, true);
        const auto plan = finish(image_builder);
        CHECK(!edge(plan, depth, stencil) && !edge(plan, depth, mips));
        CHECK(edge(plan, depth, read) && edge(plan, stencil, read) && edge(plan, mips, read));
        CHECK(hazards(plan, EGraphHazard::RAW) == 4);
    }
    {
        RenderGraphBuilder image_builder;
        const auto image = take(image_builder.declareResource({"color", GraphImageDesc{8, 8, 1, 2}}));
        auto a = pass(image_builder, "layer0", {use(image, EGraphAccess::WRITE, 1, GraphImageRange{})}, true);
        const auto layer1 = use(image, EGraphAccess::WRITE, 2, GraphImageRange{1, 0, 1, 1, 1});
        auto b = pass(image_builder, "layer1", {layer1}, true);
        CHECK(!edge(finish(image_builder), a, b));
    }
}

void versions()
{
    RenderGraphBuilder b;
    const auto r = buffer(b);
    auto later = pass(b, "later", {use(r, EGraphAccess::WRITE, 2)});
    auto reader2 = pass(b, "reader2", {use(r, EGraphAccess::READ, 1)}, true);
    auto writer = pass(b, "first", {use(r, EGraphAccess::WRITE, 1)});
    auto reader1 = pass(b, "reader1", {use(r, EGraphAccess::READ, 1)}, true);
    CHECK(b.exportOutput({{r, 2}}));
    const auto a = finish(b);
    CHECK((a.execution_order == std::vector<GraphPassId>{writer, reader2, reader1, later}));
    CHECK(hazards(a, EGraphHazard::RAW) == 2);
    CHECK(hazards(a, EGraphHazard::WAW) == 1);
    CHECK(hazards(a, EGraphHazard::WAR) == 2);
    for (bool missing : {false, true})
    {
        RenderGraphBuilder bad;
        auto r = buffer(bad);
        if (!missing)
        {
            pass(bad, "a", {use(r, EGraphAccess::WRITE)});
            pass(bad, "b", {use(r, EGraphAccess::WRITE)});
        }
        auto reader = pass(bad, "read", {use(r, EGraphAccess::READ)}, true);
        auto graph = take(std::move(bad).finish());
        auto error = analyze(graph);
        fails(error, missing ? kGraphUndefinedRead : kGraphAmbiguousProducer);
        CHECK(error.error().args[1] == r.index);
        if (missing) CHECK(error.error().args[0] == reader.index);
        CHECK(analyze(graph).error() == error.error());
    }
    {
        RenderGraphBuilder b;
        auto r = buffer(b);
        pass(b, "half", {use(r, EGraphAccess::WRITE, 1, GraphBufferRange{0, 32})});
        pass(b, "read", {use(r, EGraphAccess::READ)}, true);
        fails(analyze(take(std::move(b).finish())), kGraphUndefinedRead);
    }
    {
        RenderGraphBuilder b;
        auto r = buffer(b);
        pass(b, "write", {use(r, EGraphAccess::WRITE)});
        auto update = use(r, EGraphAccess::READ_WRITE, 2);
        update.input_version = 1;
        pass(b, "update", {update}, true);
        const auto a = finish(b);
        CHECK(hazards(a, EGraphHazard::RAW) == 1);
        CHECK(hazards(a, EGraphHazard::WAW) == 1);
    }
    {
        RenderGraphBuilder b;
        auto image = take(b.declareResource({"image", GraphImageDesc{}}));
        auto load = use(image, EGraphAccess::ATTACHMENT_LOAD);
        take(b.addPass({"load", EGraphPassKind::GRAPHICS, {load}, true}));
        fails(analyze(take(std::move(b).finish())), kGraphUndefinedRead);
    }
    {
        RenderGraphBuilder b;
        auto image = take(b.declareResource({"image", GraphImageDesc{}, EGraphOrigin::IMPORTED, {{}}}));
        auto load = use(image, EGraphAccess::ATTACHMENT_LOAD);
        take(b.addPass({"load", EGraphPassKind::GRAPHICS, {load}, true}));
        CHECK(finish(b).versions.front().version.number == 0);
    }
}

void topology()
{
    // Every declaration permutation yields the same semantic version schedule.
    std::array<int, 3> permutation{0, 1, 2};
    do
    {
        RenderGraphBuilder b;
        auto r = buffer(b);
        std::array<GraphPassId, 3> ids;
        for (int role : permutation)
        {
            const auto access = role == 1 ? EGraphAccess::READ : EGraphAccess::WRITE;
            ids[role] = pass(b, std::to_string(role), {use(r, access, role == 2 ? 2 : 1)}, true);
        }
        const auto a = finish(b);
        CHECK((a.execution_order == std::vector<GraphPassId>{ids[0], ids[1], ids[2]}));
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    for (int length : {2, 3})
    {
        RenderGraphBuilder b;
        std::vector<GraphPassId> ps;
        for (int i = 0; i < length; ++i) ps.push_back(pass(b, std::to_string(i), {}, true));
        for (int i = 0; i < length; ++i) CHECK(b.dependsOn(ps[i], ps[(i + 1) % length]));
        auto bad = analyze(take(std::move(b).finish()));
        fails(bad, kGraphDependencyCycle);
        CHECK(bad.error().args[0] < static_cast<std::uint64_t>(length));
    }
    {
        RenderGraphBuilder b;
        auto r = buffer(b);
        auto consumer = pass(b, "consumer", {use(r, EGraphAccess::READ)}, true);
        auto producer = pass(b, "producer", {use(r, EGraphAccess::WRITE)});
        CHECK(b.dependsOn(consumer, producer));
        fails(analyze(take(std::move(b).finish())), kGraphDependencyCycle);
    }
    {
        RenderGraphBuilder b;
        auto r = buffer(b);
        pass(b, "unobserved", {use(r, EGraphAccess::READ)});
        CHECK(finish(b).execution_order.empty());
    }
    {
        RenderGraphBuilder b;
        auto r = buffer(b);
        auto dead = buffer(b, "dead");
        pass(b, "dead-read", {use(dead, EGraphAccess::READ)});
        take(b.addPass({"dead-conditional", {}, {use(dead, EGraphAccess::WRITE)}, false, true}));
        auto root = pass(b, "root", {use(r, EGraphAccess::WRITE)});
        auto explicit_before = pass(b, "explicit", {});
        CHECK(b.dependsOn(explicit_before, root));
        CHECK(b.exportOutput({{r, 1}}));
        const auto a = finish(b);
        CHECK((a.execution_order == std::vector<GraphPassId>{explicit_before, root}));
        for (const auto& life : a.lifetimes) CHECK(life.version.resource != dead);
    }
    {
        RenderGraphBuilder b;
        auto downstream = pass(b, "innocent", {}, true);
        auto a = pass(b, "cycle-a", {}, true);
        auto c = pass(b, "cycle-b", {}, true);
        CHECK(b.dependsOn(a, c) && b.dependsOn(c, a) && b.dependsOn(c, downstream));
        auto result = analyze(take(std::move(b).finish()));
        fails(result, kGraphDependencyCycle);
        CHECK(result.error().args[0] != downstream.index);
    }
}

void conditionalHistory()
{
    for (bool fallback : {false, true})
    {
        RenderGraphBuilder b;
        auto r = buffer(b);
        auto base = pass(b, "base", {use(r, EGraphAccess::WRITE)});
        auto update = use(r, EGraphAccess::WRITE, 2);
        if (fallback) update.fallback = GraphVersion{r, 1};
        auto conditional = take(b.addPass({"conditional", {}, {update}, false, true}));
        auto read = pass(b, "read", {use(r, EGraphAccess::READ, 2)}, true);
        auto graph = take(std::move(b).finish());
        if (!fallback) fails(analyze(graph), kGraphMissingFallback);
        else
        {
            const auto a = take(analyze(graph));
            CHECK((a.execution_order == std::vector<GraphPassId>{base, conditional, read}));
            CHECK(std::count_if(a.versions.begin(), a.versions.end(), [](auto& v) { return v.fallback; }) == 1);
        }
    }
    for (bool initialized : {false, true})
    {
        RenderGraphBuilder b;
        GraphResource imported{"imported", GraphBufferDesc{64}, EGraphOrigin::IMPORTED};
        if (initialized) imported.initial_ranges.push_back(GraphBufferRange{0, 32});
        auto r = take(b.declareResource(imported));
        pass(b, "read", {use(r, EGraphAccess::READ, 0, GraphBufferRange{0, 32})}, true);
        auto graph = take(std::move(b).finish());
        if (!initialized) fails(analyze(graph), kGraphMissingInitial);
        else CHECK(take(analyze(graph)).versions[0].producer.index == kGraphInvalidIndex);
    }
    {
        RenderGraphBuilder b;
        auto r = take(b.declareResource({"partial-import", GraphBufferDesc{64}, EGraphOrigin::IMPORTED,
            {GraphBufferRange{0, 32}}}));
        pass(b, "read-all", {use(r, EGraphAccess::READ, 0)}, true);
        fails(analyze(take(std::move(b).finish())), kGraphMissingInitial);
    }
    for (bool valid_history : {false, true})
    {
        RenderGraphBuilder b;
        GraphResource prev{"previous", GraphBufferDesc{64}, EGraphOrigin::HISTORY_PREVIOUS};
        if (valid_history) prev.initial_ranges.emplace_back();
        auto previous = take(b.declareResource(prev));
        auto current = take(b.declareResource({"current", GraphBufferDesc{64}, EGraphOrigin::HISTORY_CURRENT}));
        auto fallback = buffer(b, "fallback");
        auto clear = pass(b, "clear", {use(fallback, EGraphAccess::WRITE)});
        auto input = use(previous, EGraphAccess::READ, 0);
        input.fallback = GraphVersion{fallback, 1};
        auto temporal = pass(b, "temporal", {input, use(current, EGraphAccess::WRITE)});
        CHECK(b.exportOutput({{current, 1}}));
        auto a = finish(b);
        CHECK(a.execution_order.size() == (valid_history ? 1 : 2));
        CHECK((std::find(a.execution_order.begin(), a.execution_order.end(), clear) == a.execution_order.end())
            == valid_history);
        CHECK(a.execution_order.back() == temporal);
        for (const auto& life : a.lifetimes)
        {
            if (life.version.resource == previous) CHECK(life.origin == EGraphOrigin::HISTORY_PREVIOUS);
            if (life.version.resource == current) CHECK(life.origin == EGraphOrigin::HISTORY_CURRENT);
        }
    }
    {
        RenderGraphBuilder b;
        auto previous = take(b.declareResource({"previous", GraphBufferDesc{8}, EGraphOrigin::HISTORY_PREVIOUS}));
        fails(b.addPass({"write-prev", {}, {use(previous, EGraphAccess::WRITE)}}), kGraphInvalidDescriptor);
        pass(b, "read", {use(previous, EGraphAccess::READ, 0)}, true);
        fails(analyze(take(std::move(b).finish())), kGraphMissingInitial);
    }
    // An intervening physical overwrite cannot serve as a fallback for an older version.
    {
        RenderGraphBuilder b;
        auto r = buffer(b);
        pass(b, "base", {use(r, EGraphAccess::WRITE, 1)});
        pass(b, "intervening", {use(r, EGraphAccess::WRITE, 2)}, true);
        auto write = use(r, EGraphAccess::WRITE, 3);
        write.fallback = GraphVersion{r, 1};
        take(b.addPass({"conditional", {}, {write}, true, true}));
        fails(analyze(take(std::move(b).finish())), kGraphDependencyCycle);
    }
    {
        RenderGraphBuilder b;
        auto r = buffer(b);
        auto update = use(r, EGraphAccess::WRITE, 2);
        update.fallback = GraphVersion{r, 1};
        take(b.addPass({"missing-fallback-producer", {}, {update}, true, true}));
        fails(analyze(take(std::move(b).finish())), kGraphUndefinedRead);
    }
}

void lifetimes()
{
    RenderGraphBuilder b;
    auto r = buffer(b);
    auto independent = buffer(b, "independent");
    auto reader = pass(b, "reader-first-declared", {use(r, EGraphAccess::READ)}, true);
    auto writer = pass(b, "writer-later-declared", {use(r, EGraphAccess::WRITE)});
    auto tail = pass(b, "tail", {use(independent, EGraphAccess::WRITE)});
    CHECK(b.dependsOn(reader, tail));
    CHECK(b.exportOutput({{independent, 1}}));
    auto a = finish(b);
    CHECK((a.execution_order == std::vector<GraphPassId>{writer, reader, tail}));
    CHECK(a.lifetimes.size() == 2);
    CHECK(a.lifetimes[0].first == 0 && a.lifetimes[0].last == 1);
    CHECK(a.lifetimes[1].first == 2 && a.lifetimes[1].last == 3);
    CHECK(a.lifetimes[0].last < a.lifetimes[1].first);
    {
        RenderGraphBuilder b;
        auto r = buffer(b);
        pass(b, "w", {use(r, EGraphAccess::WRITE)});
        pass(b, "left", {use(r, EGraphAccess::READ, 1, GraphBufferRange{0, 16})}, true);
        pass(b, "right", {use(r, EGraphAccess::READ, 1, GraphBufferRange{16, 16})}, true);
        CHECK(b.exportOutput({{r, 1}, GraphBufferRange{32, 32}}));
        const auto a = finish(b);
        CHECK(a.lifetimes.size() == 3);
        for (const auto& life : a.lifetimes)
        {
            CHECK(life.first == 0);
            const auto range = std::get<GraphBufferRange>(life.range);
            CHECK(life.last == (range.offset == 0 ? 1 : range.offset == 16 ? 2 : 3));
        }
    }
    {
        RenderGraphBuilder b;
        auto r = buffer(b);
        pass(b, "v1", {use(r, EGraphAccess::WRITE, 1)});
        pass(b, "v2", {use(r, EGraphAccess::WRITE, 2)}, true);
        CHECK(b.exportOutput({{r, 1}}));
        fails(analyze(take(std::move(b).finish())), kGraphConflictingOutput);
    }
    {
        RenderGraphBuilder b;
        auto image = take(b.declareResource({"target", GraphImageDesc{32, 16}}));
        pass(b, "render", {use(image, EGraphAccess::WRITE)});
        TargetDesc target{32, 16, lux::rdesc::ETextureFormat::RGBA8_UNORM};
        CHECK(b.exportOutput({{image, 1}, {}, target}));
        CHECK(finish(b).lifetimes[0].last == 1);
    }
}

// Independent byte-level oracle: partitions, overlapping writers, coverage and
// lifetime cells are checked by expanding only these tiny test buffers to bytes.
void oracle()
{
    for (std::uint32_t seed = 1; seed <= 160; ++seed)
    {
        auto random = seed;
        auto next = [&] { random = random * 1664525u + 1013904223u; return random; };
        RenderGraphBuilder b;
        auto r = buffer(b, "bytes", 16);
        std::array<int, 16> owners{};
        owners.fill(-1);
        bool ambiguous = false;
        for (int p = 0; p < 4; ++p)
        {
            const auto start = next() % 16;
            const auto count = 1 + next() % (16 - start);
            pass(b, std::to_string(p), {use(r, EGraphAccess::WRITE, 1, GraphBufferRange{start, count})});
            for (auto i = start; i < start + count; ++i)
            {
                if (owners[i] >= 0) ambiguous = true;
                owners[i] = p;
            }
        }
        auto reader = pass(b, "reader", {use(r, EGraphAccess::READ)}, true);
        auto a = analyze(take(std::move(b).finish()));
        const bool missing = std::find(owners.begin(), owners.end(), -1) != owners.end();
        if (ambiguous) fails(a, kGraphAmbiguousProducer);
        else if (missing) fails(a, kGraphUndefinedRead);
        else
        {
            CHECK(a);
            std::array<int, 16> covered{};
            for (const auto& relation : a->versions)
            {
                CHECK(relation.consumer == reader);
                auto range = std::get<GraphBufferRange>(relation.range);
                for (auto i = range.offset; i < range.offset + range.size; ++i)
                {
                    CHECK(owners[i] == static_cast<int>(relation.producer.index));
                    ++covered[i];
                }
            }
            for (auto count : covered) CHECK(count == 1);
        }
    }
    // Exhaust all pairs of small intervals for RAW, WAR and WAW overlap semantics.
    for (std::uint64_t start = 0; start < 8; ++start)
    {
        for (std::uint64_t count = 1; count <= 8 - start; ++count)
        {
            RenderGraphBuilder b;
            auto r = take(b.declareResource({"import", GraphBufferDesc{8}, EGraphOrigin::IMPORTED, {{}}}));
            auto read = pass(b, "initial-read", {use(r, EGraphAccess::READ, 0, GraphBufferRange{2, 3})}, true);
            auto write = pass(b, "write", {use(r, EGraphAccess::WRITE, 1, GraphBufferRange{start, count})}, true);
            const auto a = finish(b);
            const bool overlap = start < 5 && start + count > 2;
            CHECK(edge(a, read, write) == overlap);
            CHECK(hazards(a, EGraphHazard::WAR) == (overlap ? 1 : 0));
            if (overlap)
            {
                const auto range = std::get<GraphBufferRange>(a.hazards[0].range);
                CHECK(range.offset == std::max(start, std::uint64_t{2}));
                CHECK(range.size == std::min(start + count, std::uint64_t{5}) - range.offset);
            }
            RenderGraphBuilder versioned;
            auto vr = buffer(versioned, "v", 8);
            auto w1 = pass(versioned, "v1", {use(vr, EGraphAccess::WRITE)}, true);
            auto r1 = pass(versioned, "r1", {use(vr, EGraphAccess::READ, 1, GraphBufferRange{2, 3})}, true);
            auto w2 = pass(versioned, "v2", {use(vr, EGraphAccess::WRITE, 2, GraphBufferRange{start, count})}, true);
            const auto plan = finish(versioned);
            CHECK(edge(plan, w1, r1) && edge(plan, w1, w2));
            CHECK(edge(plan, r1, w2) == overlap);
            CHECK(hazards(plan, EGraphHazard::RAW) == 1 && hazards(plan, EGraphHazard::WAW) == 1);
        }
    }
    // Image coverage/lifetime oracle over 2 aspects x 3 mips x 3 layers.
    for (std::uint32_t mip = 0; mip < 3; ++mip)
    {
        for (std::uint32_t layer = 0; layer < 3; ++layer)
        {
            RenderGraphBuilder b;
            auto image = take(b.declareResource({"image", GraphImageDesc{4, 4, 3, 3,
                lux::rdesc::ETextureFormat::D24_UNORM_S8_UINT}}));
            auto writer = pass(b, "writer", {use(image, EGraphAccess::WRITE)});
            auto reader = pass(b, "reader",
                {use(image, EGraphAccess::READ, 1, GraphImageRange{2, mip, 3 - mip, layer, 3 - layer})}, true);
            const auto a = finish(b);
            CHECK(edge(a, writer, reader));
            int coverage[2][3][3]{};
            for (const auto& life : a.lifetimes)
            {
                const auto range = std::get<GraphImageRange>(life.range);
                for (std::uint32_t m = range.first_mip; m < range.first_mip + range.mip_count; ++m)
                {
                    for (std::uint32_t l = range.first_layer; l < range.first_layer + range.layer_count; ++l)
                    {
                        const int aspect = range.aspects == 2 ? 0 : 1;
                        CHECK(range.aspects == 2 || range.aspects == 4);
                        ++coverage[aspect][m][l];
                        CHECK(life.first == 0);
                        CHECK(life.last == (aspect == 0 && m >= mip && l >= layer ? 1u : 0u));
                    }
                }
            }
            for (const auto& aspect : coverage)
                for (const auto& mip_cells : aspect)
                    for (auto count : mip_cells) CHECK(count == 1);
        }
    }
}

int main(int argc, char** argv)
{
    // G11A extends use, not Graph ownership or dependency analysis. Invalid uses never append a Pass.
    {
        RenderGraphBuilder builder;
        const auto image = take(builder.declareResource({"sample", GraphImageDesc{}, EGraphOrigin::IMPORTED,
            {GraphImageRange{}}}));
        const auto bytes = take(builder.declareResource({"bytes", GraphBufferDesc{16}, EGraphOrigin::IMPORTED,
            {GraphBufferRange{0, 16}}}));
        CHECK(!builder.addPass({"bad_buffer", EGraphPassKind::GRAPHICS,
            {{{bytes, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::SAMPLED}}}));
        CHECK(!builder.addPass({"bad_transfer", EGraphPassKind::TRANSFER,
            {{{image, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::SAMPLED}}}));
        CHECK(!builder.addPass({"bad_write", EGraphPassKind::GRAPHICS,
            {{{image, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::SAMPLED}}}));
        CHECK(builder.passCount() == 0);
        CHECK(builder.addPass({"sample", EGraphPassKind::GRAPHICS,
            {{{image, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::SAMPLED}}, true}));
        CHECK(take(analyze(take(std::move(builder).finish()))).execution_order.size() == 1);
    }
    if (argc != 2) return 2;
    const std::string_view group = argv[1];
    if (group == "authoring") authoring();
    else if (group == "ranges") ranges();
    else if (group == "versions") versions();
    else if (group == "topology") topology();
    else if (group == "conditional_history") conditionalHistory();
    else if (group == "lifetimes") lifetimes();
    else if (group == "oracle") oracle();
    else return 3;
    std::printf("PASS %.*s\n", int(group.size()), group.data());
}
