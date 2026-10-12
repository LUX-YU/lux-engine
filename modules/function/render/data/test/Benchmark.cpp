#include "Allocation.hpp"
#include "Golden.data.hpp"
#include <lux/engine/render/data/Schema.hpp>

int main()
{
    using namespace lux::render;
    ComplexGpu value{};
    std::array<std::byte, DataSchema<ComplexGpu>::gpuLayout().size> output{};
    std::array<double, 1000> samples{};
    for (unsigned i = 0; i < 100; ++i)
    {
        if (!DataSchema<ComplexGpu>::pack(value, output))
        {
            std::abort();
        }
    }
    allocation_count = allocation_bytes = 0;
    measuring = true;
    for (unsigned i = 0; i < samples.size(); ++i)
    {
        value.basis[1][1] = float(i);
        const auto start = std::chrono::steady_clock::now();
        auto result = DataSchema<ComplexGpu>::pack(value, output);
        samples[i] = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
        if (!result)
        {
            std::abort();
        }
        float actual{};
        std::memcpy(&actual, output.data() + 52, 4);
        if (actual != float(i))
        {
            std::abort();
        }
    }
    measuring = false;
    if (allocation_count != 0 || allocation_bytes != 0)
    {
        std::abort();
    }
    auto sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    std::printf(
        "{\"case\":\"data_gpu_pack\",\"p50_ns\":%.0f,\"p95_ns\":%.0f,\"max_ns\":%.0f,\"allocations\":0,\"bytes\":0,"
        "\"raw\":[",
        sorted[500],
        sorted[950],
        sorted.back()
    );
    for (unsigned i = 0; i < samples.size(); ++i)
    {
        std::printf("%s%.0f", i ? "," : "", samples[i]);
    }
    std::puts("]}");
}
