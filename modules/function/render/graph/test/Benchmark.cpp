#include <lux/engine/render/graph/DefinitionAccess.hpp>
#include "AllocationCounter.hpp"

using namespace lux::render;

int main()
{
    auto definition = detail::DefinitionAccess::create(
        {{EGraphResourceKind::BUFFER, EGraphResourceOrigin::IMPORTED}},
        {{{{GraphResourceId{1}, EGraphAccess::READ, EGraphUsage::UNIFORM}}}}
    );
    if (!definition) std::abort();
    auto plan = CompiledGraphPlan::compile(*definition);
    if (!plan) std::abort();
    std::array<double, 1000> samples{};
    std::uint64_t checksum = 0;
    std::uint64_t frame = 0;
    auto bind = [&] {
        ++frame;
        const std::array imports{GraphImportBinding{GraphResourceId{1}, GraphBackingId{frame}, frame * 256}};
        auto binding = FrameGraphBindings::create(*plan, {frame, frame * 1000, 0}, imports);
        if (!binding) std::abort();
        checksum += binding->frame().frame_serial + binding->imports()[0].dynamic_offset;
    };
    for (unsigned warm = 0; warm < 10000; ++warm) bind();
    allocation_count = 0;
    allocation_bytes = 0;
    measuring = true;
    for (auto& sample : samples)
    {
        const auto start = std::chrono::steady_clock::now();
        for (unsigned index = 0; index < 1000; ++index) bind();
        sample = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / 1000;
    }
    measuring = false;
    std::sort(samples.begin(), samples.end());
    std::printf(
        "{\"case\":\"graph_frame_binding\",\"operations\":1000000,\"batch_size\":1000,"
        "\"p50_ns_per_op\":%.3f,\"p95_ns_per_op\":%.3f,\"max_ns_per_op\":%.3f,"
        "\"allocations\":%llu,\"allocation_bytes\":%llu,\"checksum\":%llu}\n",
        samples[500], samples[950], samples.back(),
        static_cast<unsigned long long>(allocation_count.load()),
        static_cast<unsigned long long>(allocation_bytes.load()), static_cast<unsigned long long>(checksum)
    );
    return allocation_count.load() == 0 ? 0 : 1;
}
