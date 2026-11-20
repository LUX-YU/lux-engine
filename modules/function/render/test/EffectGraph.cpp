#include <lux/engine/render/effect/EffectValidation.hpp>
#include <lux/engine/render/extensions/effect/EffectImplementation.hpp>
#include <lux/engine/render/resource/transform/TransformSlotRetirement.hpp>

#include <array>
#include <cstdlib>
#include <utility>

namespace
{
    using namespace lux::render;
    using namespace lux::render::effect_detail;
    namespace graph = lux::graph;

    constexpr auto kInput = effectPortSemantic("input");
    constexpr auto kOutput = effectPortSemantic("output");
    constexpr auto kMotion = effectPortSemantic("motion");
    constexpr std::string_view kImplementation = "test.copy.variant";

    template <typename T>
    void check(const T& value)
    {
        if (!static_cast<bool>(value)) std::abort();
    }

    template <typename T>
    [[nodiscard]] T take(RenderResult<T> result)
    {
        check(result);
        return std::move(*result);
    }

    [[nodiscard]] EffectPortDefinition port(
        std::string name, std::string type, graph::EPinDirection direction, bool required = false
    )
    {
        const auto semantic = effectPortSemantic(name);
        return {semantic, std::move(name), {std::move(type), 1}, direction, graph::kUnlimitedFan, required};
    }

    [[nodiscard]] std::shared_ptr<const CapabilityDefinition> registerCapability(
        EffectCatalog& catalog, std::string name, std::vector<EffectPortDefinition> ports
    )
    {
        auto definition = std::make_unique<CapabilityDefinition>();
        const auto id = graph::nodeTypeId(name);
        definition->identity = {id, std::move(name), 1};
        definition->ports = std::move(ports);
        check(catalog.addCapability(std::move(definition)));

        auto implementation = std::make_unique<EffectImplementation>();
        implementation->identity = {std::string{kImplementation}, 1};
        implementation->capability = id;
        implementation->supported_outputs = {kOutput};
        implementation->initialize = [](EffectInitializationContext&, std::string_view) noexcept -> RenderResult<void>
        {
            std::abort();
        };
        implementation->build = [](EffectBuildContext&, std::string_view) noexcept -> RenderResult<void>
        {
            std::abort();
        };
        check(catalog.addImplementation(std::move(implementation)));
        return catalog.findCapability(id, 1);
    }

    [[nodiscard]] graph::NodeId add(EffectGraphEdit& edit, const CapabilityDefinition& capability)
    {
        return take(edit.addNode(capability, {std::string{kImplementation}, 1}));
    }

    [[nodiscard]] EEffectIssue rejected(const EffectGraph& graph, const EffectCatalog& catalog)
    {
        auto result = validateEffectGraph(graph, catalog);
        check(!result);
        return result.error().issue;
    }
}

static void validateTransformSlotRetirement()
{
    using namespace lux::render::detail;
    int domain = 0;
    TransformSlotRetirement retirement;
    SubmissionFrontiers completed(&domain, 2);
    std::array submitted{SubmissionFrontiers(&domain, 2), SubmissionFrontiers(&domain, 2)};
    std::vector<std::uint32_t> free;

    retirement.release(9);
    submitted[0].accept({&domain, 0, 4});
    completed.accept({&domain, 0, 4});
    retirement.collect(true, submitted, completed, free);
    check(free.empty());
    submitted[0].accept({&domain, 1, 6});
    retirement.collect(false, submitted, completed, free);
    check(free.empty());

    retirement.release(11);
    submitted[1].accept({&domain, 0, 5});
    retirement.collect(false, submitted, completed, free);
    check(free.empty());

    completed.accept({&domain, 1, 6});
    retirement.collect(false, submitted, completed, free);
    check(free == std::vector<std::uint32_t>{9});
    completed.accept({&domain, 0, 5});
    retirement.collect(false, submitted, completed, free);
    check((free == std::vector<std::uint32_t>{9, 11}) && retirement.empty());
}

int main()
{
    validateTransformSlotRetirement();
    constexpr auto input = graph::EPinDirection::INPUT;
    constexpr auto output = graph::EPinDirection::OUTPUT;
    EffectCatalog catalog;
    const auto source = registerCapability(catalog, "test.effect.source", {
        port("output", "test.image", output), port("motion", "test.motion", output)
    });
    const auto copy = registerCapability(catalog, "test.effect.copy", {
        port("input", "test.image", input, true), port("output", "test.image", output)
    });
    const auto motion = registerCapability(catalog, "test.effect.motion", {
        port("input", "test.motion", input, true), port("output", "test.image", output)
    });

    EffectGraph graph;
    graph::NodeId abandoned;
    {
        EffectGraphEdit draft{graph};
        abandoned = add(draft, *source);
        check(!draft.connect({}, {}));
        check(!draft.commit());
    }

    graph::NodeId producer;
    graph::NodeId active;
    graph::NodeId disconnected;
    {
        EffectGraphEdit edit{graph};
        producer = add(edit, *source);
        active = add(edit, *copy);
        disconnected = add(edit, *copy);
        check(producer > abandoned);
        check(edit.connect(edit.pinId(producer, kOutput), edit.pinId(active, kInput)));
        check(edit.setOutput("color", edit.pinId(active, kOutput)));
        check(edit.commit());
    }
    check(graph.findNode(abandoned) == nullptr);
    check(rejected(graph, catalog) == EEffectIssue::CATALOG_NOT_SEALED);
    catalog.seal();
    check(!catalog.addCapability(std::make_unique<CapabilityDefinition>()));
    check(catalog.listImplementations(source->identity.id, 1).size() == 1);

    const auto validated = validateEffectGraph(graph, catalog);
    check(validated);
    const auto& compiled = *validated;
    check(compiled.nodes.size() == 3);
    check(compiled.nodes[0].source == producer && compiled.nodes[0].live);
    check(compiled.nodes[1].source == active && compiled.nodes[1].live);
    check(compiled.nodes[2].source == disconnected && !compiled.nodes[2].live);
    const auto pins = compiled.nodes[0].pins.view(compiled.pins);
    check(pins[0].demanded && !pins[1].demanded);

    {
        auto missing = graph;
        EffectGraphEdit edit{missing};
        check(edit.setOutput("color", edit.pinId(disconnected, kOutput)));
        check(edit.commit());
        check(rejected(missing, catalog) == EEffectIssue::MISSING_INPUT);
    }
    {
        auto unsupported = graph;
        EffectGraphEdit edit{unsupported};
        const auto consumer = add(edit, *motion);
        check(edit.connect(edit.pinId(producer, kMotion), edit.pinId(consumer, kInput)));
        check(edit.setOutput("color", edit.pinId(consumer, kOutput)));
        check(edit.commit());
        check(rejected(unsupported, catalog) == EEffectIssue::UNSUPPORTED_OUTPUT);
    }
    {
        auto incorrect = graph;
        EffectGraphEdit edit{incorrect};
        const auto consumer = add(edit, *motion);
        check(edit.connect(edit.pinId(producer, kOutput), edit.pinId(consumer, kInput)));
        check(edit.commit());
        check(rejected(incorrect, catalog) == EEffectIssue::TYPE_MISMATCH);
    }
    {
        EffectGraphEdit edit{graph};
        const auto other = add(edit, *copy);
        check(edit.connect(edit.pinId(disconnected, kOutput), edit.pinId(other, kInput)));
        check(edit.connect(edit.pinId(other, kOutput), edit.pinId(disconnected, kInput)));
        check(edit.commit());
        check(rejected(graph, catalog) == EEffectIssue::CYCLE);
    }
}
