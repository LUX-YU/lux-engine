#include "AllocationCounter.hpp"
#include <lux/engine/render/graph/DefinitionAccess.hpp>

using namespace lux::render;

int main()
{
    std::vector<GraphResource> resources(16);
    for (auto& resource : resources)
    {
        resource.description = BufferDesc{1024, 16};
        resource.origin = EGraphResourceOrigin::IMPORTED;
    }
    std::vector<GraphPass> passes;
    std::vector<GraphDependency> order;
    std::vector<GraphOutput> outputs;
    for (unsigned p = 0; p < 64; ++p)
    {
        GraphPass pass;
        pass.canonical_name = "compute." + std::to_string(100 + p);
        pass.key = passKey(pass.canonical_name);
        GraphResourceUse
            use{GraphResourceId{p % 16 + 1}, EGraphAccess::READ_WRITE, EGraphUsage::SHADER, BufferRange{0, 1024}};
        if (p >= 16)
        {
            use.producer = PassProducer{passes[p - 16].key};
            order.push_back({GraphPassId{p - 16 + 1}, GraphPassId{p + 1}});
        }
        else
        {
            use.producer = ImportedProducer{};
        }
        pass.uses.push_back(use);
        passes.push_back(std::move(pass));
        if (p >= 48)
        {
            outputs.push_back({GraphResourceId{p % 16 + 1}, PassProducer{passes[p].key}, BufferRange{0, 1024}});
        }
    }
    auto definition = detail::DefinitionAccess::create(resources, passes, order, outputs);
    if (!definition)
    {
        std::abort();
    }
    std::array<double, 200> samples{};
    std::uint64_t checksum = 0;
    for (unsigned i = 0; i < 10; ++i)
    {
        auto plan = compileLogicalGraph(*definition);
        if (!plan)
        {
            std::abort();
        }
    }
    allocation_count = 0;
    allocation_bytes = 0;
    measuring = true;
    for (auto& sample : samples)
    {
        const auto start = std::chrono::steady_clock::now();
        auto plan = compileLogicalGraph(*definition);
        if (!plan)
        {
            std::abort();
        }
        sample = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        checksum += plan->executionOrder().size() + plan->versions().size();
    }
    measuring = false;
    std::sort(samples.begin(), samples.end());
    std::printf(
        "{\"case\":\"cold_logical_compile\",\"operations\":200,\"passes\":64,\"resources\":16,"
        "\"p50_us\":%.3f,\"p95_us\":%.3f,\"max_us\":%.3f,\"allocations\":%llu,\"bytes\":%llu,\"checksum\":%llu}\n",
        samples[100],
        samples[190],
        samples.back(),
        static_cast<unsigned long long>(allocation_count.load()),
        static_cast<unsigned long long>(allocation_bytes.load()),
        static_cast<unsigned long long>(checksum)
    );
}
