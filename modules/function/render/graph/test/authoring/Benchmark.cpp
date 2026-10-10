#include "../AllocationCounter.hpp"
#include "Tonemap.pass.hpp"
#include <lux/engine/render/graph/Builder.hpp>

using namespace lux::render;

int main()
{
    constexpr unsigned operations = 10000;
    std::array<double, 1000> samples{};
    std::uint64_t checksum = 0;
    auto build = [&]
    {
        RenderGraphBuilder graph;
        Tonemap params;
        params.input.texture = graph.importTexture("test.input", {}, EPersistentScope::SCENE);
        params.linear.sampler = GraphSampler{1};
        params.output.texture = graph.texture({});
        auto pass = graph.addPass(
            "test.pass.1",
            ShaderReference{"test.shader.2", "default"},
            EPassKind::GRAPHICS,
            EExecutionScope::VIEW,
            params
        );
        if (!pass)
        {
            std::abort();
        }
        auto definition = std::move(graph).finish();
        if (!definition)
        {
            std::abort();
        }
        checksum += definition->passes()[0].uses.size();
    };
    for (unsigned i = 0; i < 1000; ++i)
    {
        build();
    }
    allocation_count = 0;
    allocation_bytes = 0;
    measuring = true;
    for (auto& sample : samples)
    {
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 10; ++i)
        {
            build();
        }
        sample = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / 10;
    }
    measuring = false;
    std::sort(samples.begin(), samples.end());
    std::printf(
        "{\"case\":\"cold_tonemap_authoring\",\"operations\":%u,\"p50_ns\":%.3f,\"p95_ns\":%.3f,"
        "\"max_ns\":%.3f,\"allocations\":%llu,\"allocation_bytes\":%llu,\"checksum\":%llu}\n",
        operations,
        samples[500],
        samples[950],
        samples.back(),
        static_cast<unsigned long long>(allocation_count.load()),
        static_cast<unsigned long long>(allocation_bytes.load()),
        static_cast<unsigned long long>(checksum)
    );
    return 0;
}
