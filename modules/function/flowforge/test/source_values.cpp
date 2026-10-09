#include <lux/engine/flowforge/graph/FlowSource.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <source_location>

namespace
{
    using namespace lux::flowforge;

    void require(bool condition, std::source_location at = std::source_location::current()) noexcept
    {
        if (!condition)
        {
            std::fprintf(stderr, "source value contract failed at %u\n", at.line());
            std::exit(42);
        }
    }

    void checkPins(const std::vector<FlowSourcePin>& pins, bool input) noexcept
    {
        for (std::size_t index = 0; index != pins.size(); ++index)
        {
            const auto& pin = pins[index];
            const bool is_execution =
                pin.kind == EFlowSourcePinKind::EXEC_IN || pin.kind == EFlowSourcePinKind::EXEC_OUT;
            const auto category = (is_execution ? 2U : 4U) + (input ? 0U : 1U);
            require(pin.semantic.value == (std::uint64_t{category} << 56U | (index + 1U)));
        }
    }
} // namespace

int main(int argc, char** argv)
{
    static_assert(static_cast<unsigned>(EFlowSourcePinKind::EXEC_IN) == 1);
    static_assert(static_cast<unsigned>(EFlowSourcePinKind::EXEC_OUT) == 2);
    static_assert(static_cast<unsigned>(EFlowSourcePinKind::DATA_IN) == 3);
    static_assert(static_cast<unsigned>(EFlowSourcePinKind::DATA_OUT) == 4);
    require(argc == 2);
    std::size_t count{};
    for (const auto& entry : std::filesystem::directory_iterator(argv[1]))
    {
        if (entry.path().extension() != ".luxflow")
        {
            continue;
        }
        std::ifstream stream(entry.path(), std::ios::binary);
        require(stream.good());
        const std::string bytes{std::istreambuf_iterator<char>{stream}, {}};
        auto source = decodeFlowSource(bytes);
        require(source.has_value());
        for (const auto& node : source->nodes)
        {
            checkPins(node.inputs, true);
            checkPins(node.outputs, false);
        }
        auto encoded = encodeFlowSource(*source);
        require(encoded.has_value() && encoded->find("version = 2") != std::string::npos);
        auto decoded = decodeFlowSource(*encoded);
        require(decoded.has_value() && *decoded == *source);
        require(*encodeFlowSource(*decoded) == *encoded);
        auto invalid = *source;
        bool changed{};
        for (auto& node : invalid.nodes)
        {
            auto& pins = node.inputs.empty() ? node.outputs : node.inputs;
            if (!pins.empty())
            {
                pins.front().kind = EFlowSourcePinKind::UNKNOWN;
                changed = true;
                break;
            }
        }
        require(!changed || !encodeFlowSource(invalid));
        ++count;
    }
    require(count == 33);
    std::puts("PASS: 33 frozen v1 sources, wire kinds, semantic ordinals, value roundtrip and invalid-kind refusal");
}
