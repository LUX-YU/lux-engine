#include <lux/engine/flowforge/ControlNodes.hpp>
#include <lux/engine/flowforge/ScalarNodes.hpp>
#include <lux/engine/flowforge/graph/FlowSource.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace
{
    using namespace lux;
    using namespace lux::flowforge;

    void require(bool accepted, std::source_location location = std::source_location::current()) noexcept
    {
        if (!accepted)
        {
            std::fprintf(stderr, "connection contract failed at %u\n", location.line());
            std::abort();
        }
    }

    FlowSource capture(const FlowGraph& graph)
    {
        const asset::AssetId id{std::array<std::uint8_t, 16>{1}};
        auto source = captureFlowSource(id, "connections", graph);
        require(source.has_value());
        return std::move(*source);
    }

    struct Fixture final
    {
        FlowGraph graph;
        std::vector<PinId> pins;

        Fixture()
        {
            FlowNodeCatalog catalog;
            require(catalog.add(scalarNodeRegistrations()).has_value());
            require(catalog.add(controlNodeRegistrations()).has_value());
            auto make = [&](std::string_view name, const meta::RefType* type = nullptr)
            {
                auto definition = catalog.find(graph::nodeTypeId(name));
                auto payload = definition->create();
                require(payload.has_value());
                if (type)
                {
                    payload->get<ScalarNodePayload>()->operand_type = type;
                }
                auto node = createFlowNode(definition, std::move(*payload));
                require(node.has_value());
                return std::move(*node);
            };
            std::array<FlowNode, 4> nodes{
                make("lux.flow.branch"),
                make("lux.flow.branch"),
                make("lux.flow.add", &meta::ref_type_of_v<int>),
                make("lux.flow.and", &meta::ref_type_of_v<bool>)
            };
            for (auto& node : nodes)
            {
                auto schema = node.definition->describePins(node.payload);
                require(schema.has_value());
                auto id = graph.addNode(std::move(node));
                require(id.has_value());
                for (const auto& pin : *schema)
                {
                    pins.push_back(graph.pinId(*id, pin.semantic));
                }
            }
            require(pins.size() == 14);
        }
    };

    void matrix()
    {
        for (unsigned occupied = 0; occupied != 3; ++occupied)
        {
            for (std::size_t first = 0; first != 14; ++first)
            {
                for (std::size_t second = 0; second != 14; ++second)
                {
                    Fixture fixture;
                    auto& graph = fixture.graph;
                    auto& pins = fixture.pins;
                    if (occupied == 1)
                    {
                        require(graph.connect(pins[2], pins[4]).has_value());
                    }
                    else if (occupied == 2)
                    {
                        require(graph.connect(pins[13], pins[1]).has_value());
                    }
                    const auto before = capture(graph);
                    const auto result = graph.connect(pins[first], pins[second]);
                    std::printf("%u %zu %zu %u\n", occupied, first, second, static_cast<unsigned>(result.has_value()));
                    if (!result)
                    {
                        require(capture(graph) == before);
                        continue;
                    }
                    const auto connected = capture(graph);
                    require(connected.links.size() == before.links.size() + 1);
                    const auto duplicate = graph.connect(pins[second], pins[first]);
                    require(!duplicate);
                    const auto* duplicate_error = std::get_if<graph::GraphTopologyFailure>(&duplicate.error());
                    require(duplicate_error && duplicate_error->code == graph::EGraphTopologyError::DUPLICATE_LINK);
                    require(capture(graph) == connected);
                    require(graph.disconnect(pins[second], pins[first]).has_value());
                    require(capture(graph) == before);
                    const auto missing = graph.disconnect(pins[first], pins[second]);
                    require(!missing);
                    const auto* missing_error = std::get_if<graph::GraphTopologyFailure>(&missing.error());
                    require(missing_error && missing_error->code == graph::EGraphTopologyError::UNKNOWN_LINK);
                    require(capture(graph) == before);
                }
            }
        }
    }
} // namespace

int main()
{
    meta::meta_module_init();
    matrix();
    meta::meta_module_deinit();
    std::puts("PASS 588 actual graph admissions and complete source preservation");
}
