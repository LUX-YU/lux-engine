#include <lux/engine/flowforge/graph/ArithmeticNode.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
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
        std::vector<Pin*> pins;

        Fixture()
        {
            std::array<std::unique_ptr<Node>, 4> nodes{
                std::make_unique<BranchNode>(),
                std::make_unique<BranchNode>(),
                std::make_unique<BinaryOpNode>(ENodeOperation::ADD, &meta::ref_type_of_v<int>),
                std::make_unique<BinaryOpNode>(ENodeOperation::LOGICAL_AND, &meta::ref_type_of_v<bool>)
            };
            for (auto& node : nodes)
            {
                pins.insert(pins.end(), node->inPins().begin(), node->inPins().end());
                pins.insert(pins.end(), node->outPins().begin(), node->outPins().end());
                require(graph.addNode(std::move(node)).valid());
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
                        require(graph.connect(*pins[2], *pins[4]) == ELinkError::SUCCESS);
                    }
                    else if (occupied == 2)
                    {
                        require(graph.connect(*pins[13], *pins[1]) == ELinkError::SUCCESS);
                    }
                    const auto before = capture(graph);
                    const auto result = graph.connect(*pins[first], *pins[second]);
                    std::printf("%u %zu %zu %u\n", occupied, first, second, static_cast<unsigned>(result));
                    if (result != ELinkError::SUCCESS)
                    {
                        require(capture(graph) == before);
                        continue;
                    }
                    const auto connected = capture(graph);
                    require(connected.links.size() == before.links.size() + 1);
                    require(graph.connect(*pins[second], *pins[first]) == ELinkError::HAS_LINKED);
                    require(capture(graph) == connected);
                    require(graph.disconnect(*pins[second], *pins[first]) == ELinkError::UNLINKED);
                    require(capture(graph) == before);
                    require(graph.disconnect(*pins[first], *pins[second]) == ELinkError::UNMATCHED);
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
