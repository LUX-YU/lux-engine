#include <lux/engine/editor/widgets/NodeCanvasIds.hpp>
#include "allocations.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>

int main()
{
    using lux::editor::widgets::NodeCanvasIds;
    std::vector<double> samples;
    for (int sample{}; sample != 110; ++sample)
    {
        NodeCanvasIds ids;
        const auto first = ids.node(UINT64_MAX - 100000);
        const auto begin = std::chrono::steady_clock::now();
        allocation_probe::begin();
        for (std::uint64_t i = 1; i <= 10000; ++i)
        {
#ifdef QUALITY_AFTER
            if (ids.size() >= 4096) ids.retire();
#endif
            ids.node(UINT64_MAX - 100000 + i);
            ids.pin(UINT64_MAX - 100000 + i);
            ids.link(UINT64_MAX - 100000 + i);
        }
        allocation_probe::enabled = false;
        const auto end = std::chrono::steady_clock::now();
        if (sample >= 10) samples.push_back(std::chrono::duration<double,std::micro>(end-begin).count());
        if (sample == 109)
        {
#ifdef QUALITY_AFTER
            assert(ids.size() <= 4098 && ids.source(first) == 0);
            std::printf("after mapping retained=%zu old_source=%llu ", ids.size(), ids.source(first));
#else
            assert(ids.source(first) == UINT64_MAX - 100000);
            // Each new triple appends three entries in the actual pre-P10Q header implementation.
            std::printf("before mapping retained=30001 old_source=%llu ", ids.source(first));
#endif
            std::printf("10k rounds new_calls=%zu requested_bytes=%zu; isolated mapping cost, not UI compaction qualification\n",
                allocation_probe::calls, allocation_probe::bytes);
        }
    }
    for(std::size_t i{};i<samples.size();++i) std::fprintf(stderr,"sample=%zu duration_us=%.3f\n",i,samples[i]);
    std::ranges::sort(samples);
    std::printf("mapping 10k rounds warmup=10 samples=100 p50_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f\n",
        samples[50], samples[95], samples[99], samples.back());
}
