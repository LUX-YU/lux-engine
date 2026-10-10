#include <lux/engine/render/graph/Bindings.hpp>
#include <lux/engine/render/graph/DefinitionAccess.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <type_traits>

using namespace lux::render;

namespace
{
    const std::array<GraphImportBinding, 0> empty_imports{};

    void require(bool value, int line)
    {
        if (!value)
        {
            std::fprintf(stderr, "Graph contract failed at line %d\n", line);
            std::abort();
        }
    }

#define CHECK(expression) require(static_cast<bool>(expression), __LINE__)

    GraphResourceUse use(std::uint32_t resource, EGraphAccess access, EGraphUsage usage = EGraphUsage::SHADER)
    {
        return {GraphResourceId{resource}, access, usage};
    }

    GraphDependency edge(std::uint32_t before, std::uint32_t after)
    {
        return {GraphPassId{before}, GraphPassId{after}};
    }

    LogicalGraphPlan compile(
        std::vector<GraphResource> resources,
        std::vector<GraphPass> passes,
        std::vector<GraphDependency> dependencies = {}
    )
    {
        if (dependencies.empty())
        {
            std::vector<std::optional<unsigned>> writer(resources.size());
            for (unsigned p = 0; p < passes.size(); ++p)
            {
                passes[p].canonical_name = "ordered.fixture." + std::to_string(p + 1);
                passes[p].key = passKey(passes[p].canonical_name);
                for (auto& access : passes[p].uses)
                {
                    auto& previous = writer[access.resource.value() - 1];
                    if (access.access != EGraphAccess::WRITE)
                    {
                        access.producer = previous ? VGraphProducer{PassProducer{passes[*previous].key}}
                                                   : VGraphProducer{ImportedProducer{}};
                    }
                    if (access.access != EGraphAccess::READ)
                    {
                        if (previous)
                        {
                            dependencies.push_back(edge(*previous + 1, p + 1));
                        }
                        previous = p;
                    }
                }
            }
        }
        auto definition =
            detail::DefinitionAccess::create(std::move(resources), std::move(passes), std::move(dependencies));
        CHECK(definition);
        auto result = compileLogicalGraph(*definition, {.cull_unused = false});
        CHECK(result);
        return std::move(*result);
    }

    bool hasEdge(const LogicalGraphPlan& plan, std::uint32_t before, std::uint32_t after)
    {
        return std::find(plan.dependencies().begin(), plan.dependencies().end(), edge(before, after)) !=
               plan.dependencies().end();
    }

    template <typename T> void error(const RenderResult<T>& result, lux::error::ErrorId expected)
    {
        CHECK(!result);
        CHECK(result.error().type == expected);
    }

    void declarations()
    {
        static_assert(!std::is_convertible_v<GraphResourceId, GraphPassId>);
        static_assert(!std::is_convertible_v<GraphBackingId, GraphResourceId>);
        static_assert(!std::is_default_constructible_v<RenderGraphDefinition>);
        static_assert(!std::is_default_constructible_v<LogicalGraphPlan>);
        static_assert(!std::is_default_constructible_v<FrameGraphBindings>);
        auto empty = compile({}, {});
        CHECK(empty.executionOrder().empty() && empty.lifetimes().empty());
        CHECK(renderGraphErrorDescriptors().size() == 11);
        error(detail::DefinitionAccess::create({{BufferDesc{0, 1}}}, {}), kGraphInvalidResource);
        error(
            detail::DefinitionAccess::create({{BufferDesc{}, static_cast<EGraphResourceOrigin>(55)}}, {}),
            kGraphInvalidResource
        );
        const auto target = renderTargetSemanticId("test.color");
        error(
            detail::DefinitionAccess::create({{BufferDesc{}, EGraphResourceOrigin::IMPORTED, target}}, {}),
            kGraphInvalidResource
        );
        const GraphResource image{TextureDesc{}, EGraphResourceOrigin::IMPORTED, target};
        error(detail::DefinitionAccess::create({image, image}, {}), kGraphInvalidResource);
        for (auto id : {0u, 2u})
        {
            error(detail::DefinitionAccess::create({{}}, {{{use(id, EGraphAccess::WRITE)}}}), kGraphInvalidUse);
        }
        error(
            detail::DefinitionAccess::create({{}}, {{{use(1, EGraphAccess::READ), use(1, EGraphAccess::WRITE)}}}),
            kGraphInvalidUse
        );
        error(detail::DefinitionAccess::create({{}}, {{{use(1, static_cast<EGraphAccess>(55))}}}), kGraphInvalidUse);
        error(
            detail::DefinitionAccess::create({{}}, {{{use(1, EGraphAccess::WRITE, static_cast<EGraphUsage>(55))}}}),
            kGraphInvalidUse
        );
        error(
            detail::DefinitionAccess::create({{}}, {{{use(1, EGraphAccess::WRITE, EGraphUsage::COLOR_ATTACHMENT)}}}),
            kGraphInvalidUse
        );
        error(
            detail::DefinitionAccess::create({image}, {{{use(1, EGraphAccess::READ, EGraphUsage::VERTEX)}}}),
            kGraphInvalidUse
        );
        error(
            detail::DefinitionAccess::create({{}}, {{{use(1, EGraphAccess::WRITE, EGraphUsage::UNIFORM)}}}),
            kGraphInvalidUse
        );
        error(
            detail::DefinitionAccess::create({{}}, {{{use(1, EGraphAccess::READ_WRITE, EGraphUsage::TRANSFER)}}}),
            kGraphInvalidUse
        );
        error(
            detail::DefinitionAccess::create({{TextureDesc{}}}, {{{use(1, EGraphAccess::READ, EGraphUsage::PRESENT)}}}),
            kGraphInvalidUse
        );
        for (auto dependency : {edge(0, 1), edge(1, 2), edge(1, 1)})
        {
            error(detail::DefinitionAccess::create({}, {{}}, {dependency}), kGraphInvalidDependency);
        }
        auto a = detail::DefinitionAccess::create({}, {{}, {}, {}}, {edge(2, 3), edge(1, 2), edge(1, 2)});
        auto b = detail::DefinitionAccess::create({}, {{}, {}, {}}, {edge(1, 2), edge(2, 3)});
        CHECK(a && b && *a == *b);
        auto uses_a =
            detail::DefinitionAccess::create({{}, {}}, {{{use(2, EGraphAccess::WRITE), use(1, EGraphAccess::WRITE)}}});
        auto uses_b =
            detail::DefinitionAccess::create({{}, {}}, {{{use(1, EGraphAccess::WRITE), use(2, EGraphAccess::WRITE)}}});
        CHECK(uses_a && uses_b && *uses_a == *uses_b);
    }

    void hazards()
    {
        auto plan = compile(
            {{}},
            {{{use(1, EGraphAccess::WRITE)}},
             {{use(1, EGraphAccess::READ)}},
             {{use(1, EGraphAccess::READ)}},
             {{use(1, EGraphAccess::WRITE)}},
             {{use(1, EGraphAccess::READ_WRITE)}},
             {{use(1, EGraphAccess::READ)}}}
        );
        CHECK(hasEdge(plan, 1, 2) && hasEdge(plan, 1, 3));
        CHECK(!hasEdge(plan, 2, 3) && !hasEdge(plan, 3, 2));
        CHECK(hasEdge(plan, 1, 4) && hasEdge(plan, 2, 4) && hasEdge(plan, 3, 4));
        CHECK(hasEdge(plan, 4, 5) && hasEdge(plan, 5, 6));
        auto imported = compile(
            {{BufferDesc{}, EGraphResourceOrigin::IMPORTED}},
            {{{use(1, EGraphAccess::READ)}}, {{use(1, EGraphAccess::READ)}}, {{use(1, EGraphAccess::WRITE)}}}
        );
        CHECK(hasEdge(imported, 1, 3) && hasEdge(imported, 2, 3));
        // Explicit dependencies can put a later-declared producer first (V1 semantics).
        auto reordered = compile({{}}, {{{use(1, EGraphAccess::READ)}}, {{use(1, EGraphAccess::WRITE)}}}, {edge(2, 1)});
        CHECK(reordered.executionOrder()[0] == GraphPassId{2});
        CHECK(hasEdge(reordered, 2, 1));
        auto independent = compile({}, {{}, {}, {}}, {edge(3, 1)});
        CHECK(independent.executionOrder()[0] == GraphPassId{2});
        CHECK(independent.executionOrder()[1] == GraphPassId{3});
        CHECK(independent.executionOrder()[2] == GraphPassId{1});
    }

    void lifetimes()
    {
        auto plan = compile(
            {{}, {}, {}, {TextureDesc{}, EGraphResourceOrigin::IMPORTED}},
            {{{use(1, EGraphAccess::WRITE)}},
             {{use(1, EGraphAccess::READ), use(2, EGraphAccess::WRITE)}},
             {{use(2, EGraphAccess::READ), use(4, EGraphAccess::WRITE, EGraphUsage::COLOR_ATTACHMENT)}}}
        );
        CHECK(plan.lifetimes()[0] == (GraphResourceLifetime{0, 1}));
        CHECK(plan.lifetimes()[1] == (GraphResourceLifetime{1, 2}));
        CHECK(!plan.lifetimes()[2]);
        CHECK(plan.lifetimes()[3] == (GraphResourceLifetime{2, 2}));
        CHECK(plan.imports().size() == 1 && plan.imports()[0] == GraphResourceId{4});
        auto reordered = compile(
            {{}},
            {{{use(1, EGraphAccess::READ)}}, {{use(1, EGraphAccess::WRITE)}}, {}},
            {edge(3, 2), edge(2, 1)}
        );
        CHECK(reordered.lifetimes()[0] == (GraphResourceLifetime{1, 2}));
    }

    void legacyVectors()
    {
        // compiler_result.cpp: dependency cycle, required producer, retained
        // empty side-effect pass, owned moved plan, failed candidate/last good.
        auto cycle = detail::DefinitionAccess::create({}, {{}, {}}, {edge(1, 2), edge(2, 1)});
        CHECK(cycle);
        error(compileLogicalGraph(*cycle), kGraphCycle);
        auto missing = detail::DefinitionAccess::create({{}}, {{{use(1, EGraphAccess::READ)}}});
        CHECK(missing);
        error(compileLogicalGraph(*missing), kGraphMissingProducer);
        auto read_write = detail::DefinitionAccess::create({{}}, {{{use(1, EGraphAccess::READ_WRITE)}}});
        CHECK(read_write);
        error(compileLogicalGraph(*read_write), kGraphMissingProducer);
        auto good = compile({}, {{}});
        const auto* original = good.executionOrder().data();
        auto rejected = compileLogicalGraph(*cycle);
        if (rejected)
        {
            good = std::move(*rejected);
        }
        CHECK(good.executionOrder().data() == original && good.executionOrder().size() == 1);
        auto owned = std::move(good);
        CHECK(owned.identity().passes.size() == 1 && owned.executionOrder()[0] == GraphPassId{1});
        // Native source validation is now frame binding validation, not compile.
        auto imported = compile({{TextureDesc{}, EGraphResourceOrigin::IMPORTED}}, {});
        error(FrameGraphBindings::create(imported, {}, empty_imports), kGraphInvalidBinding);
    }

    void bindings()
    {
        auto plan = compile(
            {{TextureDesc{}, EGraphResourceOrigin::IMPORTED}, {BufferDesc{}, EGraphResourceOrigin::IMPORTED}},
            {}
        );
        std::array imports{
            GraphImportBinding{GraphResourceId{1}, GraphBackingId{10}, 0, 1},
            GraphImportBinding{GraphResourceId{2}, GraphBackingId{20}, 64, 1}
        };
        CHECK(FrameGraphBindings::create(plan, {7, 100, 2}, imports));
        auto short_imports = std::span(imports).first(1);
        error(FrameGraphBindings::create(plan, {}, short_imports), kGraphInvalidBinding);
        imports[0].dynamic_offset = 1;
        error(FrameGraphBindings::create(plan, {}, imports), kGraphInvalidBinding);
        imports[0].dynamic_offset = 0;
        imports[0].backing = {};
        error(FrameGraphBindings::create(plan, {}, imports), kGraphInvalidBinding);
        imports[0].backing = GraphBackingId{20};
        error(FrameGraphBindings::create(plan, {}, imports), kGraphInvalidBinding);
        imports[0].backing = GraphBackingId{10};
        std::swap(imports[0], imports[1]);
        error(FrameGraphBindings::create(plan, {}, imports), kGraphInvalidBinding);
        auto empty = compile({}, {});
        CHECK(FrameGraphBindings::create(empty, {}, empty_imports));
    }

    void reuse()
    {
        std::vector<GraphResource> resources{{BufferDesc{}, EGraphResourceOrigin::IMPORTED}};
        std::vector<GraphPass> passes{{{use(1, EGraphAccess::READ)}}};
        auto definition = detail::DefinitionAccess::create(resources, passes);
        CHECK(definition);
        std::size_t compile_count = 1;
        auto result = compileLogicalGraph(*definition);
        CHECK(result);
        auto& plan = *result;
        const auto* order = plan.executionOrder().data();
        for (std::uint64_t frame = 1; frame <= 10000; ++frame)
        {
            const std::array imports{GraphImportBinding{GraphResourceId{1}, GraphBackingId{frame}, frame * 256, 1}};
            auto binding =
                FrameGraphBindings::create(plan, {frame, frame * 1000, static_cast<std::uint32_t>(frame % 3)}, imports);
            CHECK(binding && &binding->plan() == &plan);
            CHECK(binding->frame().frame_serial == frame && binding->frame().render_time_ns == frame * 1000);
            CHECK(binding->imports()[0].dynamic_offset == frame * 256);
            CHECK(plan.executionOrder().data() == order);
        }
        auto same = detail::DefinitionAccess::create(resources, passes);
        CHECK(same && plan.matches(*same));
        passes[0].uses[0].access = EGraphAccess::READ_WRITE;
        auto changed = detail::DefinitionAccess::create(resources, passes);
        CHECK(changed && !plan.matches(*changed));
        auto replacement = compileLogicalGraph(*changed);
        CHECK(replacement);
        ++compile_count;
        CHECK(compile_count == 2 && plan.matches(*same));
        // Resource kind/origin/semantic, usage, pass set and edges are topology.
        resources[0].origin = EGraphResourceOrigin::TRANSIENT;
        auto origin = detail::DefinitionAccess::create(resources, {{{use(1, EGraphAccess::READ)}}});
        CHECK(origin && !plan.matches(*origin));
        resources[0] = {TextureDesc{}, EGraphResourceOrigin::IMPORTED};
        auto kind = detail::DefinitionAccess::create(resources, {{{use(1, EGraphAccess::READ)}}});
        CHECK(kind && !plan.matches(*kind));
        auto image_plan = compileLogicalGraph(*kind);
        CHECK(image_plan);
        resources[0].target_semantic = renderTargetSemanticId("test.frame.color");
        auto semantic = detail::DefinitionAccess::create(resources, {{{use(1, EGraphAccess::READ)}}});
        CHECK(semantic && !image_plan->matches(*semantic));
        auto usage = detail::DefinitionAccess::create(
            {{BufferDesc{}, EGraphResourceOrigin::IMPORTED}},
            {{{use(1, EGraphAccess::READ, EGraphUsage::UNIFORM)}}}
        );
        CHECK(usage && !plan.matches(*usage));
        auto extra = detail::DefinitionAccess::create({}, {{}, {}}, {edge(2, 1)});
        auto plain = compile({}, {{}, {}});
        CHECK(extra && !plain.matches(*extra));
        auto fewer = detail::DefinitionAccess::create({}, {{}});
        CHECK(fewer && !plain.matches(*fewer));
    }

    void generatedVectors()
    {
        // Independent O(P^2 R) oracle: every pair with at least one write must
        // be ordered; read/read pairs need not be. No production analyzer reuse.
        std::uint32_t seed = 3817;
        for (std::uint32_t trial = 0; trial < 300; ++trial)
        {
            std::vector<GraphResource> resources(7, {BufferDesc{}, EGraphResourceOrigin::IMPORTED});
            std::vector<GraphPass> passes(24);
            std::array<std::array<int, 7>, 24> access{};
            for (std::uint32_t p = 0; p < 24; ++p)
            {
                for (std::uint32_t r = 0; r < 7; ++r)
                {
                    seed = seed * 1664525u + 1013904223u;
                    access[p][r] = static_cast<int>((seed >> 16) % 4);
                    if (access[p][r] != 0)
                    {
                        passes[p].uses.push_back(use(r + 1, static_cast<EGraphAccess>(access[p][r] - 1)));
                    }
                }
            }
            auto plan = compile(resources, passes);
            std::array<std::array<bool, 24>, 24> reachable{};
            std::array<std::uint32_t, 24> positions{};
            for (std::uint32_t i = 0; i < 24; ++i)
            {
                positions[plan.executionOrder()[i].value() - 1] = i;
            }
            for (auto dependency : plan.dependencies())
            {
                reachable[dependency.before.value() - 1][dependency.after.value() - 1] = true;
            }
            for (std::uint32_t k = 0; k < 24; ++k)
            {
                for (std::uint32_t a = 0; a < 24; ++a)
                {
                    for (std::uint32_t b = 0; b < 24; ++b)
                    {
                        reachable[a][b] = reachable[a][b] || (reachable[a][k] && reachable[k][b]);
                    }
                }
            }
            for (std::uint32_t r = 0; r < 7; ++r)
            {
                std::optional<GraphResourceLifetime> interval;
                for (std::uint32_t a = 0; a < 24; ++a)
                {
                    if (access[a][r] == 0)
                    {
                        continue;
                    }
                    if (!interval)
                    {
                        interval = GraphResourceLifetime{positions[a], positions[a]};
                    }
                    interval->first_pass = std::min(interval->first_pass, positions[a]);
                    interval->last_pass = std::max(interval->last_pass, positions[a]);
                    for (std::uint32_t b = a + 1; b < 24; ++b)
                    {
                        const bool has_conflict = access[b][r] != 0 && (access[a][r] > 1 || access[b][r] > 1);
                        if (has_conflict)
                        {
                            CHECK(reachable[a][b] && positions[a] < positions[b]);
                        }
                    }
                }
                CHECK(interval == plan.lifetimes()[r]);
            }
        }
    }
} // namespace

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    const std::string_view test = argv[1];
    if (test == "declarations")
    {
        declarations();
    }
    else if (test == "hazards")
    {
        hazards();
    }
    else if (test == "lifetimes")
    {
        lifetimes();
    }
    else if (test == "legacy")
    {
        legacyVectors();
    }
    else if (test == "bindings")
    {
        bindings();
    }
    else if (test == "reuse")
    {
        reuse();
    }
    else if (test == "generated")
    {
        generatedVectors();
    }
    else
    {
        CHECK(false);
    }
    std::printf("PASS graph %s\n", argv[1]);
}
