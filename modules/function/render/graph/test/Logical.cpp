#include <lux/engine/render/graph/Bindings.hpp>
#include <lux/engine/render/graph/DefinitionAccess.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>

using namespace lux::render;

namespace
{
    void check(bool value, int line)
    {
        if (!value)
        {
            std::fprintf(stderr, "F2 logical contract failed at %d\n", line);
            std::abort();
        }
    }

#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)), __LINE__)

    GraphResource buffer(bool imported = false)
    {
        GraphResource result;
        result.description = BufferDesc{64, 4};
        result.origin = imported ? EGraphResourceOrigin::IMPORTED : EGraphResourceOrigin::TRANSIENT;
        return result;
    }

    GraphResource image(bool imported = false)
    {
        auto result = buffer(imported);
        TextureDesc description;
        description.mip_count = 3;
        description.array_layers = 2;
        description.format = lux::rdesc::ETextureFormat::D24_UNORM_S8_UINT;
        result.description = description;
        return result;
    }

    GraphResourceUse use(
        unsigned resource,
        EGraphAccess access,
        VGraphRange range,
        VGraphProducer producer = AutomaticProducer{}
    )
    {
        GraphResourceUse result{GraphResourceId{resource}, access, EGraphUsage::SHADER, std::move(range)};
        result.producer = std::move(producer);
        return result;
    }

    GraphPass pass(std::string name, std::vector<GraphResourceUse> uses)
    {
        GraphPass result;
        result.key = passKey(name);
        result.canonical_name = std::move(name);
        result.uses = std::move(uses);
        return result;
    }

    GraphDependency order(unsigned a, unsigned b)
    {
        return {GraphPassId{a}, GraphPassId{b}};
    }

    GraphOutput output(
        unsigned resource,
        VGraphProducer producer = AutomaticProducer{},
        VGraphRange range = WholeResource{}
    )
    {
        return {GraphResourceId{resource}, producer, range, EGraphOutput::EXPORT};
    }

    RenderGraphDefinition definition(
        std::vector<GraphResource> resources,
        std::vector<GraphPass> passes,
        std::vector<GraphDependency> dependencies = {},
        std::vector<GraphOutput> outputs = {},
        std::vector<GraphProvider> providers = {}
    )
    {
        auto result = detail::DefinitionAccess::create(
            std::move(resources),
            std::move(passes),
            std::move(dependencies),
            std::move(outputs),
            std::move(providers)
        );
        CHECK(result);
        return std::move(*result);
    }

    bool edge(const LogicalGraphPlan& plan, unsigned a, unsigned b, EGraphHazard kind)
    {
        return std::any_of(
            plan.hazards().begin(),
            plan.hazards().end(),
            [&](const auto& hazard)
            { return hazard.before == GraphPassId{a} && hazard.after == GraphPassId{b} && hazard.kind == kind; }
        );
    }

    void ranges()
    {
        for (auto aspect : {EAspect::DEPTH, EAspect::STENCIL})
        {
            for (unsigned layer = 0; layer < 2; ++layer)
            {
                ImageRange range{aspect, 1, 1, layer, 1};
                auto graph = definition(
                    {image()},
                    {pass("write", {use(1, EGraphAccess::WRITE, range)}),
                     pass("read", {use(1, EGraphAccess::READ, range, PassProducer{passKey("write")})}),
                     pass("overwrite", {use(1, EGraphAccess::WRITE, range)})},
                    {order(1, 3)},
                    {output(1, {}, range)}
                );
                auto plan = compileLogicalGraph(graph, {.cull_unused = false});
                CHECK(plan && edge(*plan, 1, 2, EGraphHazard::RAW));
                CHECK(edge(*plan, 2, 3, EGraphHazard::WAR) && edge(*plan, 1, 3, EGraphHazard::WAW));
            }
        }
        for (const bool overlapping : {false, true})
        {
            const BufferRange first{0, 32}, second{overlapping ? 16u : 32u, 32};
            auto graph = definition(
                {buffer(true)},
                {pass("write", {use(1, EGraphAccess::WRITE, first)}), pass("read", {use(1, EGraphAccess::READ, second)})
                }
            );
            auto plan = compileLogicalGraph(graph, {.cull_unused = false});
            CHECK(plan && edge(*plan, 1, 2, EGraphHazard::RAW) == overlapping);
        }
        auto graph = definition(
            {image()},
            {pass("mip0", {use(1, EGraphAccess::WRITE, ImageRange{EAspect::DEPTH, 0, 1, 0, 1})}),
             pass("mip1", {use(1, EGraphAccess::WRITE, ImageRange{EAspect::DEPTH, 1, 1, 0, 1})}),
             pass("stencil", {use(1, EGraphAccess::WRITE, ImageRange{EAspect::STENCIL, 0, 1, 0, 1})})}
        );
        auto plan = compileLogicalGraph(graph, {.cull_unused = false});
        CHECK(plan && plan->dependencies().empty());
    }

    void versions()
    {
        auto a = pass("a", {use(1, EGraphAccess::WRITE, BufferRange{0, 64})});
        auto b = pass("b", {use(1, EGraphAccess::WRITE, BufferRange{0, 64})});
        auto read = pass("read", {use(1, EGraphAccess::READ, BufferRange{0, 64})});
        auto ambiguous = compileLogicalGraph(definition({buffer()}, {a, b, read}));
        CHECK(!ambiguous && ambiguous.error().type == kGraphAmbiguousProducer);
        ambiguous = compileLogicalGraph(definition({buffer()}, {a, b, read}, {order(1, 2)}));
        CHECK(!ambiguous && ambiguous.error().type == kGraphAmbiguousProducer);
        read.uses[0].producer = PassProducer{a.key};
        auto plan = compileLogicalGraph(definition({buffer()}, {b, read, a}, {order(3, 1)}), {.cull_unused = false});
        CHECK(plan && edge(*plan, 3, 2, EGraphHazard::RAW) && edge(*plan, 2, 1, EGraphHazard::WAR));
        CHECK(plan->versions().size() == 2);
        auto cycle = definition({buffer()}, {a, b, read}, {order(1, 2), order(2, 3)});
        auto rejected = compileLogicalGraph(cycle);
        CHECK(!rejected && rejected.error().type == kGraphCycle);
        auto diagnostic = diagnoseLogicalGraph(cycle);
        CHECK(diagnostic.cycle_path.size() >= 3 && diagnostic.cycle_path.front() == diagnostic.cycle_path.back());
        CHECK(diagnostic.json.find("read") != std::string::npos);
    }

    void initialization()
    {
        auto partial = buffer(true);
        partial.import_contract = GraphImportContract{{BufferRange{0, 16}}, false};
        auto good = definition({partial}, {pass("read", {use(1, EGraphAccess::READ, BufferRange{0, 16})})});
        CHECK(compileLogicalGraph(good));
        auto ready_plan = compileLogicalGraph(good);
        std::array imports{GraphImportBinding{GraphResourceId{1}, GraphBackingId{1}, 0, 0}};
        CHECK(!FrameGraphBindings::create(*ready_plan, {}, imports));
        imports[0].ready_epoch = 1;
        CHECK(FrameGraphBindings::create(*ready_plan, {}, imports));
        partial.import_contract->temporal_history = true;
        auto history_graph = definition({partial}, {pass("read", {use(1, EGraphAccess::READ, BufferRange{0, 16})})});
        auto history_plan = compileLogicalGraph(history_graph);
        CHECK(history_plan);
        auto values = makeGraphInvocationData(history_graph);
        values.history_epoch = 2;
        CHECK(!FrameGraphBindings::create(*history_plan, {}, imports, values));
        imports[0].history_epoch = 2;
        CHECK(FrameGraphBindings::create(*history_plan, {}, imports, values));
        auto bad =
            compileLogicalGraph(definition({partial}, {pass("read", {use(1, EGraphAccess::READ, BufferRange{0, 20})})})
            );
        CHECK(!bad && bad.error().type == kGraphMissingProducer);
        for (const auto shape : {image(), image(true)})
        {
            auto resource = shape;
            if (resource.origin == EGraphResourceOrigin::IMPORTED)
            {
                resource.import_contract = GraphImportContract{{ImageRange{EAspect::DEPTH, 0, 1, 0, 1}}, false};
            }
            auto graph = definition(
                {resource},
                {pass("write", {use(1, EGraphAccess::WRITE, ImageRange{EAspect::DEPTH, 0, 1, 0, 1})}),
                 pass("read", {use(1, EGraphAccess::READ, ImageRange{EAspect::DEPTH, 1, 1, 0, 1})})}
            );
            CHECK(!compileLogicalGraph(graph));
        }
        auto rw = definition({buffer()}, {pass("rw", {use(1, EGraphAccess::READ_WRITE, BufferRange{0, 64})})});
        CHECK(!compileLogicalGraph(rw));
    }

    void depthStencil()
    {
        auto resource = image(true);
        resource.import_contract = GraphImportContract{{ImageRange{EAspect::STENCIL, 0, 1, 0, 1}}, false};
        auto access = use(1, EGraphAccess::READ_WRITE, ImageRange{EAspect::DEPTH_STENCIL, 0, 1, 0, 1});
        access.usage = EGraphUsage::DEPTH_ATTACHMENT;
        access.field_index = 0;
        auto graphics = pass("mixed", {access});
        graphics.kind = EPassKind::GRAPHICS;
        CapturedFieldBinding field;
        field.resource = GraphResourceId{1};
        field.resource_kind = EGraphResourceKind::IMAGE;
        field.role = lux::rdesc::EPassFieldRole::DEPTH_STENCIL;
        field.image_range = {EAspect::DEPTH_STENCIL, 0, 1, 0, 1};
        field.load = ELoadOp::CLEAR;
        field.store = EStoreOp::STORE;
        field.stencil_load = ELoadOp::LOAD;
        field.stencil_store = EStoreOp::STORE;
        graphics.bindings.push_back(field);
        auto graph = definition({resource}, {graphics});
        CHECK(graph.passes()[0].uses.size() == 2);
        CHECK(graph.passes()[0].uses[0].access == EGraphAccess::WRITE);
        CHECK(graph.passes()[0].uses[1].access == EGraphAccess::READ_WRITE);
        CHECK(compileLogicalGraph(graph));
        resource.import_contract->initialized_ranges.clear();
        CHECK(!compileLogicalGraph(definition({resource}, {graphics})));
        resource.import_contract->initialized_ranges = {WholeResource{}};
        graphics.bindings[0].stencil_store = EStoreOp::DISCARD;
        auto reader = pass(
            "read",
            {use(1, EGraphAccess::READ, ImageRange{EAspect::STENCIL, 0, 1, 0, 1}, PassProducer{graphics.key})}
        );
        CHECK(!compileLogicalGraph(definition({resource}, {graphics, reader})));
    }

    void providers()
    {
        const auto semantic = graphResourceKey("feature.color");
        auto producer = pass("provider", {use(2, EGraphAccess::WRITE, BufferRange{0, 64})});
        auto consumer = pass("consumer", {use(1, EGraphAccess::READ, BufferRange{0, 64}, SemanticProducer{semantic})});
        auto missing = compileLogicalGraph(definition({buffer(), buffer()}, {consumer}));
        CHECK(!missing && missing.error().type == kGraphMissingProducer);
        const GraphProvider provider{semantic, GraphResourceId{2}, producer.key};
        auto graph = definition({buffer(), buffer()}, {consumer, producer}, {}, {}, {provider});
        auto plan = compileLogicalGraph(graph, {.cull_unused = false});
        CHECK(plan && edge(*plan, 2, 1, EGraphHazard::RAW) && plan->matches(graph));
        CHECK(explainLogicalCompatibility(*plan, graph).empty());
        auto altered = consumer;
        altered.shader = ShaderKey{99};
        auto changed_graph = definition({buffer(), buffer()}, {altered, producer}, {}, {}, {provider});
        CHECK(!plan->matches(changed_graph));
        const auto differences = explainLogicalCompatibility(*plan, changed_graph);
        CHECK(std::find(differences.begin(), differences.end(), "passes[0].shader") != differences.end());
        CHECK(plan->identity().passes[0].uses[0].resource == GraphResourceId{2});
        CHECK(!compileLogicalGraph(definition({buffer(), buffer()}, {consumer, producer}, {}, {}, {provider, provider}))
        );
        consumer.uses[0].fallback = GraphFallback{GraphResourceId{2}, ImportedProducer{}};
        CHECK(compileLogicalGraph(definition({buffer(), buffer(true)}, {consumer})));
        CHECK(!compileLogicalGraph(definition({buffer(), buffer()}, {consumer})));
        consumer.uses[0].fallback->producer = PassProducer{producer.key};
        CHECK(compileLogicalGraph(definition({buffer(), buffer()}, {consumer, producer})));
    }

    void conditions()
    {
        auto producer = pass("conditional", {use(1, EGraphAccess::WRITE, BufferRange{0, 64})});
        producer.condition = graphResourceKey("enabled");
        auto consumer = pass("consumer", {use(1, EGraphAccess::READ, BufferRange{0, 64}, PassProducer{producer.key})});
        CHECK(!compileLogicalGraph(definition({buffer(), buffer(true)}, {producer, consumer})));
        consumer.uses[0].fallback = GraphFallback{GraphResourceId{2}, ImportedProducer{}};
        auto graph = definition({buffer(), buffer(true)}, {producer, consumer});
        auto plan = compileLogicalGraph(graph, {.cull_unused = false});
        CHECK(plan && plan->inputChoices().size() == 1);
        auto values = makeGraphInvocationData(graph);
        const std::array imports{GraphImportBinding{GraphResourceId{2}, GraphBackingId{99}, 0, 1}};
        auto first = FrameGraphBindings::create(*plan, {}, imports, values);
        CHECK(first && first->resourceFor(GraphPassId{2}, 0) == GraphResourceId{1});
        values.passes[0].enabled = false;
        auto second = FrameGraphBindings::create(*plan, {}, imports, values);
        CHECK(second && second->resourceFor(GraphPassId{2}, 0) == GraphResourceId{2});
        consumer.uses[0].fallback = GraphFallback{GraphResourceId{1}, ImportedProducer{}};
        auto same_backing = definition({buffer(true)}, {producer, consumer});
        auto same_plan = compileLogicalGraph(same_backing, {.cull_unused = false});
        CHECK(same_plan); // Alternate initial version is only read while its writer is disabled.
        consumer.uses[0].fallback.reset();
        consumer.condition = producer.condition;
        graph = definition({buffer()}, {producer, consumer});
        plan = compileLogicalGraph(graph, {.cull_unused = false});
        CHECK(plan);
        values = makeGraphInvocationData(graph);
        const std::array<GraphImportBinding, 0> none{};
        values.passes[0].enabled = false;
        CHECK(!FrameGraphBindings::create(*plan, {}, none, values));
        values.passes[1].enabled = false;
        CHECK(FrameGraphBindings::create(*plan, {}, none, values));
    }

    void culling()
    {
        auto graph = definition(
            {buffer(), buffer(), buffer()},
            {pass("root", {use(1, EGraphAccess::WRITE, BufferRange{0, 64})}),
             pass("dead", {use(2, EGraphAccess::WRITE, BufferRange{0, 64})}),
             pass(
                 "tail",
                 {use(1, EGraphAccess::READ, BufferRange{0, 64}), use(3, EGraphAccess::WRITE, BufferRange{0, 64})}
             )},
            {},
            {output(3)}
        );
        auto plan = compileLogicalGraph(graph);
        CHECK(plan && plan->executionOrder().size() == 2 && !plan->livePasses()[1]);
        CHECK(!plan->lifetimes()[1] && plan->lifetimes()[2]->last_pass == 2);
        CHECK(plan->diagnosticsJson().find("unreachable") != std::string::npos);
        auto host = pass("readback", {use(1, EGraphAccess::READ, BufferRange{0, 64})});
        host.kind = EPassKind::HOST_READBACK;
        host.uses[0].usage = EGraphUsage::TRANSFER;
        auto readback = compileLogicalGraph(definition({buffer(true)}, {host}));
        CHECK(readback && readback->executionOrder().size() == 1);
        host.uses.clear();
        CHECK(!compileLogicalGraph(definition({}, {host})));
        auto invalid = output(1);
        invalid.kind = EGraphOutput::PRESENT;
        CHECK(!compileLogicalGraph(definition({buffer(true)}, {}, {}, {invalid})));
        invalid.kind = EGraphOutput::EXTERNAL_WRITE;
        CHECK(!compileLogicalGraph(definition({buffer(true)}, {}, {}, {invalid})));
    }

    void scopes()
    {
        auto resource = buffer(true);
        resource.persistent_scope = EPersistentScope::VIEW;
        auto scene = pass("scene", {use(1, EGraphAccess::READ, BufferRange{0, 64})});
        scene.scope = EExecutionScope::SCENE;
        CHECK(!compileLogicalGraph(definition({resource}, {scene})));
        resource.persistent_scope = EPersistentScope::SCENE;
        for (auto mask : {1u, 2u, 4u, 8u})
        {
            scene.invocation_inputs = mask;
            CHECK(!compileLogicalGraph(definition({resource}, {scene})));
        }
        scene.invocation_inputs = 0;
        auto graph = definition({resource}, {scene});
        auto plan = compileLogicalGraph(graph, {.cull_unused = false});
        CHECK(plan);
        auto a = makeGraphInvocationData(graph), b = a;
        a.scene = b.scene = 1;
        a.scene_revision = b.scene_revision = 9;
        a.view = 1;
        b.view = 2;
        b.camera[0] = 0.5f;
        std::array imports{GraphImportBinding{GraphResourceId{1}, GraphBackingId{3}, 0, 1}};
        auto first = FrameGraphBindings::create(*plan, {1, 10, 0}, imports, a);
        auto second = FrameGraphBindings::create(*plan, {9, 200, 1}, imports, b);
        CHECK(first && second && mayShareSceneInvocation(GraphPassId{1}, *first, *second));
        auto c = b;
        c.view = 3;
        c.camera[0] = 0.9f;
        auto third = FrameGraphBindings::create(*plan, {10, 300, 2}, imports, c);
        CHECK(third && mayShareSceneInvocation(GraphPassId{1}, *first, *third));
        const unsigned scene_invocations = 1u + !mayShareSceneInvocation(GraphPassId{1}, *first, *second) +
                                           !mayShareSceneInvocation(GraphPassId{1}, *first, *third);
        CHECK(scene_invocations == 1); // Neutral invocation grouping, no Runtime or fake FrameLoop.
        ++b.scene_revision;
        CHECK(!mayShareSceneInvocation(GraphPassId{1}, *first, *second));
        --b.scene_revision;
        auto changed = imports;
        ++changed[0].ready_epoch;
        second = FrameGraphBindings::create(*plan, {}, changed, b);
        CHECK(second && !mayShareSceneInvocation(GraphPassId{1}, *first, *second));
    }

    void feedback()
    {
        TextureDesc desc;
        GraphResource resource;
        resource.description = desc;
        const ImageRange range{EAspect::COLOR, 0, 1, 0, 1};
        auto write = use(1, EGraphAccess::WRITE, range);
        write.usage = EGraphUsage::COLOR_ATTACHMENT;
        auto read = use(1, EGraphAccess::READ, range);
        read.usage = EGraphUsage::INPUT_ATTACHMENT;
        auto graphics = pass("local", {write, read});
        graphics.kind = EPassKind::GRAPHICS;
        CHECK(!detail::DefinitionAccess::create({resource}, {graphics}));
        graphics.uses[1].local_read = true;
        auto graph = definition({resource}, {graphics});
        CHECK(!compileLogicalGraph(graph));
        auto plan = compileLogicalGraph(graph, {.cull_unused = false, .allow_local_read = true});
        CHECK(plan && plan->executionOrder().size() == 1);
        graphics.uses[1].usage = EGraphUsage::SHADER;
        CHECK(!detail::DefinitionAccess::create({resource}, {graphics}));
        auto disjoint = definition(
            {buffer()},
            {pass(
                "separate",
                {use(1, EGraphAccess::WRITE, BufferRange{0, 16}), use(1, EGraphAccess::WRITE, BufferRange{16, 16})}
            )}
        );
        CHECK(compileLogicalGraph(disjoint));
    }

    void oracle()
    {
        // Independent per-byte model, no production interval partitioning or analyzer calls.
        // Explicit writer ordering and producer references encode the fixture's authored sequence.
        std::uint32_t seed = 0x132794ab;
        for (unsigned trial = 0; trial < 700; ++trial)
        {
            const bool texture = trial % 2 != 0;
            auto cellRange = [&](unsigned cell) -> VGraphRange
            {
                if (!texture)
                {
                    return BufferRange{cell, 1};
                }
                return ImageRange{cell < 6 ? EAspect::DEPTH : EAspect::STENCIL, cell % 3, 1, (cell % 6) / 3, 1};
            };
            std::vector<GraphPass> passes;
            std::vector<GraphDependency> deps;
            std::array<int, 64> writer{};
            std::array<std::array<int, 64>, 16> accesses{};
            std::array<std::array<int, 64>, 16> expected_writer{};
            for (unsigned p = 0; p < 16; ++p)
            {
                seed = seed * 1664525u + 1013904223u;
                const unsigned offset = texture ? ((seed >> 16) % 9) : ((seed >> 16) % 8) * 8;
                const unsigned width = texture ? 4 : 8;
                const auto access = static_cast<EGraphAccess>((seed >> 8) % 3);
                auto node = pass("pass." + std::to_string(100 + p), {});
                // Split authored reads by independently tracked version, retaining exact bytes.
                for (unsigned byte = offset; byte < offset + width; ++byte)
                {
                    const auto source = writer[byte] == 0 ? VGraphProducer{ImportedProducer{}}
                                                          : VGraphProducer{PassProducer{passes[writer[byte] - 1].key}};
                    node.uses.push_back(use(1, access, cellRange(byte), source));
                    accesses[p][byte] = static_cast<int>(access) + 1;
                    expected_writer[p][byte] = writer[byte];
                    if (access != EGraphAccess::READ)
                    {
                        if (writer[byte])
                        {
                            deps.push_back(order(writer[byte], p + 1));
                        }
                        writer[byte] = p + 1;
                    }
                }
                passes.push_back(std::move(node));
            }
            auto graph = definition({texture ? image(true) : buffer(true)}, passes, deps);
            auto plan = compileLogicalGraph(graph, {.cull_unused = false});
            CHECK(plan);
            auto repeated = compileLogicalGraph(graph, {.cull_unused = false});
            CHECK(repeated && repeated->diagnosticsJson() == plan->diagnosticsJson());
            std::array<unsigned, 16> position{};
            for (unsigned i = 0; i < 16; ++i)
            {
                position[plan->executionOrder()[i].value() - 1] = i;
            }
            for (unsigned a = 0; a < 16; ++a)
            {
                for (unsigned b = a + 1; b < 16; ++b)
                {
                    for (unsigned byte = 0; byte < 64; ++byte)
                    {
                        if (accesses[a][byte] && accesses[b][byte] && (accesses[a][byte] > 1 || accesses[b][byte] > 1))
                        {
                            CHECK(position[a] < position[b]);
                        }
                    }
                }
            }
            for (unsigned p = 0; p < 16; ++p)
            {
                for (unsigned byte = 0; byte < 64; ++byte)
                {
                    if (accesses[p][byte] == 0 || accesses[p][byte] == 2)
                    {
                        continue;
                    }
                    bool found = false;
                    for (const auto& version : plan->versions())
                    {
                        if (!texture)
                        {
                            const auto& range = std::get<BufferRange>(version.range);
                            if (byte < range.byte_offset || byte >= range.byte_offset + range.byte_count)
                            {
                                continue;
                            }
                        }
                        else
                        {
                            const auto& range = std::get<ImageRange>(version.range);
                            const auto aspect = byte < 6 ? EAspect::DEPTH : EAspect::STENCIL;
                            const auto mip = byte % 3, layer = (byte % 6) / 3;
                            if (range.aspect != aspect || mip < range.base_mip ||
                                mip >= range.base_mip + range.mip_count || layer < range.base_layer ||
                                layer >= range.base_layer + range.layer_count)
                            {
                                continue;
                            }
                        }
                        if (version.writer.value() != static_cast<unsigned>(expected_writer[p][byte]))
                        {
                            continue;
                        }
                        found = std::find(version.readers.begin(), version.readers.end(), GraphPassId{p + 1}) !=
                                version.readers.end();
                        if (found)
                        {
                            break;
                        }
                    }
                    CHECK(found);
                }
            }
        }
        std::puts(
            "independent byte/version oracle: 700 mixed Buffer/Texture graphs PASS; original R3 oracle remains separate"
        );
    }
} // namespace

int main(int argc, char** argv)
{
    const std::string_view name = argc == 2 ? argv[1] : "";
    if (name == "ranges")
    {
        ranges();
    }
    else if (name == "versions")
    {
        versions();
    }
    else if (name == "initialization")
    {
        initialization();
    }
    else if (name == "depth_stencil")
    {
        depthStencil();
    }
    else if (name == "providers")
    {
        providers();
    }
    else if (name == "conditions")
    {
        conditions();
    }
    else if (name == "culling")
    {
        culling();
    }
    else if (name == "scopes")
    {
        scopes();
    }
    else if (name == "feedback")
    {
        feedback();
    }
    else if (name == "oracle")
    {
        oracle();
    }
    else
    {
        return 2;
    }
}
