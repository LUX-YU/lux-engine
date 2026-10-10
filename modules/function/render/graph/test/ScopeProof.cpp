#include <lux/engine/render/graph/Bindings.hpp>
#include <lux/engine/render/graph/DefinitionAccess.hpp>

#include <algorithm>
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
            std::fprintf(stderr, "Scope proof failed at %d\n", line);
            std::exit(1);
        }
    }

#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)), __LINE__)

    GraphPass pass(std::string name, EExecutionScope scope)
    {
        GraphPass result;
        result.key = passKey(name);
        result.canonical_name = std::move(name);
        result.scope = scope;
        return result;
    }

    void orderView()
    {
        auto a = pass("view.a", EExecutionScope::VIEW);
        auto b = pass("scene.b", EExecutionScope::SCENE);
        auto definition = detail::DefinitionAccess::create({}, {a, b}, {{GraphPassId{1}, GraphPassId{2}}});
        CHECK(definition);
        // Diagnostic retention makes both passes live without disguising ORDER as a data edge.
        LogicalCompileOptions options;
        options.cull_unused = false;
        auto plan = compileLogicalGraph(*definition, options);
        std::printf("View ORDER Scene: accepted=%d; expected kGraphScopeConflict\n", bool(plan));
        CHECK(!plan && plan.error().type == kGraphScopeConflict);
    }

    GraphResource buffer(bool imported = false)
    {
        GraphResource value;
        value.description = BufferDesc{64, 4};
        value.origin = imported ? EGraphResourceOrigin::IMPORTED : EGraphResourceOrigin::TRANSIENT;
        return value;
    }

    GraphResourceUse use(unsigned id, EGraphAccess access, VGraphProducer producer = AutomaticProducer{})
    {
        GraphResourceUse value{GraphResourceId{id}, access, EGraphUsage::SHADER, BufferRange{0, 64}};
        value.producer = producer;
        return value;
    }

    GraphOutput output(unsigned id, PassKey producer)
    {
        return {GraphResourceId{id}, PassProducer{producer}, WholeResource{}, EGraphOutput::EXPORT};
    }

    RenderGraphDefinition define(
        std::vector<GraphResource> resources,
        std::vector<GraphPass> passes,
        std::vector<GraphDependency> edges = {},
        std::vector<GraphOutput> outputs = {}
    )
    {
        auto result = detail::DefinitionAccess::create(
            std::move(resources),
            std::move(passes),
            std::move(edges),
            std::move(outputs)
        );
        CHECK(result);
        return std::move(*result);
    }

    void rejectScope(
        const RenderGraphDefinition& graph,
        EGraphHazard reason,
        LogicalCompileOptions options = {},
        std::size_t minimum_path = 2
    )
    {
        auto result = compileLogicalGraph(graph, options);
        CHECK(!result && result.error().type == kGraphScopeConflict);
        const auto diagnostic = diagnoseLogicalGraph(graph, options);
        CHECK(diagnostic.error && diagnostic.error->type == kGraphScopeConflict);
        CHECK(diagnostic.scope_path.size() >= minimum_path);
        CHECK(diagnostic.scope_names.size() == diagnostic.scope_path.size());
        CHECK(diagnostic.json.find("scope_path") != std::string::npos);
        CHECK(std::any_of(
            diagnostic.scope_edges.begin(),
            diagnostic.scope_edges.end(),
            [&](const auto& edge) { return edge.kind == reason; }
        ));
        for (std::size_t i = 0; i < diagnostic.scope_names.size(); ++i)
        {
            CHECK(passKey(diagnostic.scope_names[i]) == diagnostic.scope_path[i]);
        }
    }

    void orders()
    {
        orderView();
        for (auto scope : {EExecutionScope::VIEW, EExecutionScope::TARGET})
        {
            auto a = pass("a", scope), b = pass("b", EExecutionScope::SCENE);
            a.uses = {use(1, EGraphAccess::WRITE)};
            b.uses = {use(2, EGraphAccess::WRITE)};
            auto graph = define(
                {buffer(), buffer()},
                {a, b},
                {{GraphPassId{1}, GraphPassId{2}}},
                {output(1, a.key), output(2, b.key)}
            );
            rejectScope(graph, EGraphHazard::ORDER);
        }
        for (auto middle : {EExecutionScope::VIEW, EExecutionScope::SCENE})
        {
            // Reverse declaration order ensures that the first reported destination requires a transitive proof.
            auto a = pass("a", EExecutionScope::VIEW), b = pass("b", middle), c = pass("c", EExecutionScope::SCENE);
            auto graph = define({}, {c, b, a}, {{GraphPassId{3}, GraphPassId{2}}, {GraphPassId{2}, GraphPassId{1}}});
            rejectScope(graph, EGraphHazard::ORDER, {.cull_unused = false}, 3);
        }
        for (auto destination : {EExecutionScope::VIEW, EExecutionScope::TARGET, EExecutionScope::SCENE})
        {
            auto a = pass("a", EExecutionScope::SCENE), b = pass("b", destination);
            auto graph = define({}, {a, b}, {{GraphPassId{1}, GraphPassId{2}}});
            auto result = compileLogicalGraph(graph, {.cull_unused = false});
            CHECK(result && result->sceneShareEligible()[0]);
        }
        auto target = pass("target", EExecutionScope::TARGET), view = pass("view", EExecutionScope::VIEW);
        rejectScope(
            define({}, {target, view}, {{GraphPassId{1}, GraphPassId{2}}}),
            EGraphHazard::ORDER,
            {.cull_unused = false}
        );
    }

    void hazards()
    {
        auto a = pass("view", EExecutionScope::VIEW), b = pass("scene", EExecutionScope::SCENE);
        a.uses = {use(1, EGraphAccess::READ, ImportedProducer{}), use(2, EGraphAccess::WRITE)};
        b.uses = {use(1, EGraphAccess::WRITE)};
        rejectScope(
            define({buffer(true), buffer()}, {a, b}, {}, {output(2, a.key), output(1, b.key)}),
            EGraphHazard::WAR
        );
        a.uses = {use(1, EGraphAccess::WRITE), use(2, EGraphAccess::WRITE)};
        rejectScope(
            define(
                {buffer(), buffer()},
                {a, b},
                {{GraphPassId{1}, GraphPassId{2}}},
                {output(2, a.key), output(1, b.key)}
            ),
            EGraphHazard::WAW
        );
        b.uses = {use(1, EGraphAccess::READ, PassProducer{a.key}), use(3, EGraphAccess::WRITE)};
        rejectScope(define({buffer(), buffer(), buffer()}, {a, b}, {}, {output(3, b.key)}), EGraphHazard::RAW);
    }

    void sharing()
    {
        auto a = pass("scene.a", EExecutionScope::SCENE), b = pass("scene.b", EExecutionScope::SCENE);
        auto c = pass("scene.c", EExecutionScope::SCENE);
        a.uses = {use(1, EGraphAccess::READ, ImportedProducer{}), use(2, EGraphAccess::WRITE)};
        a.initial_scalars.resize(4);
        b.uses = {use(3, EGraphAccess::WRITE)};
        c.uses = {use(4, EGraphAccess::WRITE)};
        auto graph = define(
            {buffer(true), buffer(), buffer(), buffer()},
            {a, b, c},
            {{GraphPassId{1}, GraphPassId{2}}, {GraphPassId{2}, GraphPassId{3}}},
            {output(2, a.key), output(3, b.key), output(4, c.key)}
        );
        auto plan = compileLogicalGraph(graph);
        CHECK(plan && plan->sceneSources()[2].size() == 3);
        auto x = makeGraphInvocationData(graph), y = x;
        x.scene = y.scene = 1;
        x.scene_revision = y.scene_revision = 8;
        x.view = 1;
        y.view = 2;
        y.camera[0] = 0.5f;
        std::array imports{GraphImportBinding{GraphResourceId{1}, GraphBackingId{1}, 0, 1}};
        auto changed = imports;
        auto first = FrameGraphBindings::create(*plan, {}, imports, x);
        auto second = FrameGraphBindings::create(*plan, {3, 99, 2}, changed, y);
        CHECK(first && second && mayShareSceneInvocation(GraphPassId{3}, *first, *second));
        y.passes[0].scalars[0] = std::byte{1};
        CHECK(!mayShareSceneInvocation(GraphPassId{3}, *first, *second));
        y.passes[0].scalars[0] = std::byte{0};
        changed[0].backing = GraphBackingId{2};
        CHECK(!mayShareSceneInvocation(GraphPassId{3}, *first, *second));
        changed = imports;
        ++changed[0].ready_epoch;
        CHECK(!mayShareSceneInvocation(GraphPassId{3}, *first, *second));
        changed = imports;
        ++y.scene_revision;
        CHECK(!mayShareSceneInvocation(GraphPassId{3}, *first, *second));
    }

    void conditions()
    {
        auto a = pass("conditional", EExecutionScope::SCENE), b = pass("ordered", EExecutionScope::SCENE);
        auto consumer = pass("consumer", EExecutionScope::VIEW);
        a.condition = graphResourceKey("enabled");
        a.uses = {use(1, EGraphAccess::WRITE)};
        b.uses = {use(2, EGraphAccess::WRITE)};
        auto read = use(1, EGraphAccess::READ, PassProducer{a.key});
        read.fallback = GraphFallback{GraphResourceId{1}, ImportedProducer{}};
        consumer.uses = {read, use(3, EGraphAccess::WRITE)};
        auto make = [&]
        {
            return define(
                {buffer(true), buffer(), buffer()},
                {a, b, consumer},
                {{GraphPassId{1}, GraphPassId{2}}},
                {output(2, b.key), output(3, consumer.key)}
            );
        };
        auto graph = make();
        auto plan = compileLogicalGraph(graph);
        CHECK(plan && plan->livePasses()[0] && plan->sceneSources()[1].size() == 2);
        auto x = makeGraphInvocationData(graph), y = x;
        x.scene = y.scene = 1;
        std::array imports{GraphImportBinding{GraphResourceId{1}, GraphBackingId{1}, 0, 1}};
        for (bool enabled : {false, true})
        {
            x.passes[0].enabled = y.passes[0].enabled = enabled;
            auto first = FrameGraphBindings::create(*plan, {}, imports, x);
            auto second = FrameGraphBindings::create(*plan, {}, imports, y);
            CHECK(first && second && mayShareSceneInvocation(GraphPassId{2}, *first, *second));
            CHECK(first->resourceFor(GraphPassId{3}, 0) == GraphResourceId{1});
            y.passes[0].enabled = !enabled;
            CHECK(!mayShareSceneInvocation(GraphPassId{2}, *first, *second));
        }
        a.scope = EExecutionScope::VIEW;
        rejectScope(make(), EGraphHazard::ORDER);
    }

    void culling()
    {
        auto a = pass("dead.view", EExecutionScope::VIEW), b = pass("live.scene", EExecutionScope::SCENE);
        b.uses = {use(1, EGraphAccess::WRITE)};
        auto graph = define({buffer()}, {a, b}, {{GraphPassId{1}, GraphPassId{2}}}, {output(1, b.key)});
        auto plan = compileLogicalGraph(graph);
        CHECK(plan && !plan->livePasses()[0] && plan->sceneShareEligible()[1]);
        CHECK(!plan->sceneShareEligible()[0] && plan->sceneSources()[0].empty());
        CHECK(plan->sceneSources()[1].size() == 1);
        rejectScope(graph, EGraphHazard::ORDER, {.cull_unused = false});
    }

    void digest()
    {
        auto p = pass("same", EExecutionScope::VIEW);
        GraphResource texture;
        texture.description = TextureDesc{};
        auto graph = define({texture}, {p});
        auto original = compileLogicalGraph(graph);
        CHECK(original);
        p.shader_declarations = "different interface";
        auto changed_shader = define({texture}, {p});
        auto shader_plan = compileLogicalGraph(changed_shader);
        CHECK(shader_plan && original->diagnosticDigest() == shader_plan->diagnosticDigest());
        CHECK(!original->matches(changed_shader) && !explainLogicalCompatibility(*original, changed_shader).empty());
        p.shader_declarations.clear();
        std::get<TextureDesc>(texture.description).format = lux::rdesc::ETextureFormat::RGBA16_SFLOAT;
        auto changed_format = define({texture}, {p});
        auto format_plan = compileLogicalGraph(changed_format);
        CHECK(format_plan && original->diagnosticDigest() == format_plan->diagnosticDigest());
        CHECK(!original->matches(changed_format) && !explainLogicalCompatibility(*original, changed_format).empty());
    }

    void roles()
    {
        using R = lux::rdesc::EPassFieldRole;
        constexpr std::array descriptors{
            R::SAMPLED_READ,
            R::STORAGE_READ,
            R::STORAGE_WRITE,
            R::STORAGE_READ_WRITE,
            R::SAMPLER,
            R::UNIFORM_READ,
            R::READ_ONLY_STORAGE,
            R::READ_WRITE_STORAGE,
            R::INPUT_ATTACHMENT
        };
        constexpr std::array others{
            R::COLOR_ATTACHMENT,
            R::DEPTH_STENCIL,
            R::RESOLVE,
            R::TRANSFER_SOURCE,
            R::TRANSFER_DESTINATION,
            R::VERTEX,
            R::INDEX,
            R::INDIRECT
        };
        for (auto role : descriptors)
        {
            CHECK(lux::rdesc::isShaderDescriptorRole(role) && lux::rdesc::isPassFieldRole(role));
        }
        for (auto role : others)
        {
            CHECK(!lux::rdesc::isShaderDescriptorRole(role) && lux::rdesc::isPassFieldRole(role));
        }
        static_assert(!lux::rdesc::isShaderDescriptorRole(static_cast<R>(-1)));
        static_assert(!lux::rdesc::isPassFieldRole(static_cast<R>(999)));
    }
} // namespace

int main(int argc, char** argv)
{
    const std::string_view name = argc > 1 ? argv[1] : "orders";
    if (name == "orders")
    {
        orders();
    }
    else if (name == "hazards")
    {
        hazards();
    }
    else if (name == "sharing")
    {
        sharing();
    }
    else if (name == "conditions")
    {
        conditions();
    }
    else if (name == "culling")
    {
        culling();
    }
    else if (name == "digest")
    {
        digest();
    }
    else if (name == "roles")
    {
        roles();
    }
    else
    {
        return 2;
    }
    std::printf("F2-FIX %.*s PASS\n", static_cast<int>(name.size()), name.data());
}
