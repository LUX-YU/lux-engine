#include "AllocationCounter.hpp"
#include <lux/engine/render/graph/DefinitionAccess.hpp>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>

#include <psapi.h>
#endif

using namespace lux::render;

int main(int argc, char** argv)
{
    const unsigned count = argc > 1 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)) : 64;
    if (count != 64 && count != 128 && count != 256 && count != 512)
    {
        return 2;
    }
    const unsigned repeats = count <= 128 ? 20 : 5;
    const unsigned mips = count / 64, layers = 2, cells = mips * layers;
    std::vector<GraphResource> resources(16);
    for (unsigned r = 0; r < 16; ++r)
    {
        auto& resource = resources[r];
        resource.origin = EGraphResourceOrigin::IMPORTED;
        if (r % 2 == 0)
        {
            TextureDesc desc;
            desc.mip_count = mips;
            desc.array_layers = layers;
            resource.description = desc;
        }
        else
        {
            resource.description = BufferDesc{cells * 64, 4};
        }
    }
    auto range = [&](unsigned r, unsigned cell) -> VGraphRange
    {
        if (r % 2 == 0)
        {
            return ImageRange{EAspect::COLOR, cell % mips, 1, cell / mips, 1};
        }
        return BufferRange{cell * 64, 64};
    };
    std::vector<GraphPass> passes;
    std::vector<GraphDependency> order;
    std::vector<GraphOutput> outputs;
    std::vector<unsigned> previous(16 * cells);
    for (unsigned p = 0; p < count; ++p)
    {
        const unsigned r = p % 16, cell = (p / 16) % cells;
        auto& source = previous[r * cells + cell];
        GraphPass pass;
        pass.canonical_name = "scale." + std::to_string(1000 + p);
        pass.key = passKey(pass.canonical_name);
        GraphResourceUse use{GraphResourceId{r + 1}, EGraphAccess::READ_WRITE, EGraphUsage::SHADER, range(r, cell)};
        use.producer =
            source ? VGraphProducer{PassProducer{passes[source - 1].key}} : VGraphProducer{ImportedProducer{}};
        if (source)
        {
            order.push_back({GraphPassId{source}, GraphPassId{p + 1}});
        }
        source = p + 1;
        pass.uses.push_back(use);
        passes.push_back(std::move(pass));
    }
    for (unsigned r = 0; r < 16; ++r)
    {
        for (unsigned cell = 0; cell < cells; ++cell)
        {
            outputs.push_back(
                {GraphResourceId{r + 1}, PassProducer{passes[previous[r * cells + cell] - 1].key}, range(r, cell)}
            );
        }
    }
    auto definition = detail::DefinitionAccess::create(resources, passes, order, outputs);
    if (!definition)
    {
        return 3;
    }
    auto warm = compileLogicalGraph(*definition);
    if (!warm || warm->executionOrder().size() != count)
    {
        return 4;
    }
    const auto dependencies = warm->dependencies().size(), versions = warm->versions().size(),
               hazards = warm->hazards().size();
    std::vector<double> samples(repeats);
    allocation_count = 0;
    allocation_bytes = 0;
    measuring = true;
    for (auto& sample : samples)
    {
        const auto start = std::chrono::steady_clock::now();
        auto plan = compileLogicalGraph(*definition);
        if (!plan || plan->executionOrder().size() != count || plan->versions().size() != versions)
        {
            std::abort();
        }
        sample = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    }
    measuring = false;
    std::uint64_t peak_working_set = 0;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
    {
        return 5;
    }
    peak_working_set = memory.PeakWorkingSetSize;
#endif
    std::printf(
        "{\"passes\":%u,\"resources\":16,\"subresource_cells\":%u,\"image_mips\":%u,\"image_layers\":%u,"
        "\"buffer_ranges\":%u,\"dependencies\":%zu,\"versions\":%zu,\"hazards\":%zu,\"repeats\":%u,"
        "\"diagnostics\":true,\"culling\":true,\"warmup\":1,\"allocations\":%llu,\"bytes\":%llu,"
        "\"peak_process_working_set\":%llu,\"samples_us\":[",
        count,
        16 * cells,
        mips,
        layers,
        cells,
        dependencies,
        versions,
        hazards,
        repeats,
        static_cast<unsigned long long>(allocation_count.load()),
        static_cast<unsigned long long>(allocation_bytes.load()),
        static_cast<unsigned long long>(peak_working_set)
    );
    for (unsigned i = 0; i < repeats; ++i)
    {
        std::printf("%s%.3f", i ? "," : "", samples[i]);
    }
    std::sort(samples.begin(), samples.end());
    std::printf(
        "],\"p50_us\":%.3f,\"p95_us\":%.3f,\"max_us\":%.3f}\n",
        samples[repeats / 2],
        samples[repeats * 95 / 100],
        samples.back()
    );
}
