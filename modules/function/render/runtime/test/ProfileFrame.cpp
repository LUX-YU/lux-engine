// Test-only wrapper around the exact production frame algorithm. No public seam.
#define executeRuntimeFrame executeRuntimeFrameUninstrumented
#include "../src/Frame.cpp"
#undef executeRuntimeFrame
#include <chrono>
#include <cstdio>
#include <vk_mem_alloc.h>

namespace
{
    constexpr unsigned kFrames = 264;
    using Clock = std::chrono::steady_clock;
    VkDevice device{};
    VkQueryPool queries{};
    double period{};
    std::array<std::array<double, 240>, 5> samples{};
    std::array<bool, 3> active{};
    std::uint64_t peak_bytes{}, previous_time{};
    unsigned completed{};

    void report(const char* name, const std::array<double, 240>& raw)
    {
        auto sorted = raw;
        std::sort(sorted.begin(), sorted.end());
        std::printf(
            "{\"case\":\"%s\",\"p50_ns\":%.3f,\"p95_ns\":%.3f,\"max_ns\":%.3f,\"samples_ns\":[",
            name,
            sorted[120],
            sorted[228],
            sorted.back()
        );
        for (unsigned i = 0; i < raw.size(); ++i)
        {
            std::printf("%s%.3f", i ? "," : "", raw[i]);
        }
        std::puts("]}");
    }
} // namespace

namespace lux::render::detail
{
    RenderResult<void> executeRuntimeFrame(
        RuntimeBackendState& backend,
        std::uint64_t serial,
        std::uint64_t ns
    ) noexcept
    {
        if (!queries)
        {
            device = backend.device.native();
            period = backend.device.properties().limits.timestampPeriod;
            VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            info.queryCount = kFrames * 6;
            if (vkCreateQueryPool(device, &info, nullptr, &queries) != VK_SUCCESS)
            {
                std::terminate();
            }
            for (unsigned q = 0; q < 3; ++q)
            {
                active[q] = std::find(backend.queues.begin(), backend.queues.begin() + q, backend.queues[q]) ==
                            backend.queues.begin() + q;
            }
        }
        const bool sampled = serial <= kFrames;
        const auto query = static_cast<unsigned>((serial - 1) * 6);
        if (sampled)
        {
            for (unsigned q = 0; q < 3; ++q)
            {
                if (!active[q])
                {
                    continue;
                }
                auto batch = backend.queues[q]->begin();
                if (!batch)
                {
                    return cxx::unexpected(batch.error());
                }
                vkCmdResetQueryPool(batch->native(), queries, query + q * 2, 2);
                vkCmdWriteTimestamp2(batch->native(), VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, queries, query + q * 2);
                auto submitted = std::move(*batch).submit();
                if (!submitted)
                {
                    return cxx::unexpected(submitted.error());
                }
            }
        }
        const auto start = Clock::now();
        auto result = executeRuntimeFrameUninstrumented(backend, serial, ns);
        const double elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        if (!result)
        {
            return result;
        }
        if (sampled)
        {
            for (unsigned q = 0; q < 3; ++q)
            {
                if (!active[q])
                {
                    continue;
                }
                auto batch = backend.queues[q]->begin();
                if (!batch)
                {
                    return cxx::unexpected(batch.error());
                }
                vkCmdWriteTimestamp2(
                    batch->native(),
                    VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                    queries,
                    query + q * 2 + 1
                );
                auto submitted = std::move(*batch).submit();
                if (!submitted)
                {
                    return cxx::unexpected(submitted.error());
                }
            }
            VmaTotalStatistics stats{};
            vmaCalculateStatistics(backend.allocator.native(), &stats);
            peak_bytes = std::max(peak_bytes, stats.total.statistics.blockBytes);
            if (serial > 24)
            {
                samples[0][serial - 25] = elapsed;
                samples[1][serial - 25] = double(ns - previous_time);
            }
            completed = static_cast<unsigned>(serial);
        }
        previous_time = ns;
        return {};
    }
} // namespace lux::render::detail

void finishRuntimeProfile()
{
    if (completed != kFrames)
    {
        std::terminate();
    }
    for (unsigned frame = 24; frame < kFrames; ++frame)
    {
        for (unsigned q = 0; q < 3; ++q)
        {
            if (!active[q])
            {
                continue;
            }
            std::array<std::uint64_t, 2> ticks{};
            if (vkGetQueryPoolResults(
                    device,
                    queries,
                    frame * 6 + q * 2,
                    2,
                    sizeof(ticks),
                    ticks.data(),
                    sizeof(std::uint64_t),
                    VK_QUERY_RESULT_64_BIT
                ) != VK_SUCCESS)
            {
                std::terminate();
            }
            samples[2 + q][frame - 24] = double(ticks[1] - ticks[0]) * period;
        }
    }
    report("runtime_CPU_frame_including_import_validation_record_submit", samples[0]);
    report("runtime_pacing_interval", samples[1]);
    const char* names[]{
        "graphics_GPU_interval_including_submission_gaps",
        "compute_GPU_interval_including_submission_gaps",
        "transfer_GPU_interval_including_submission_gaps"
    };
    for (unsigned q = 0; q < 3; ++q)
    {
        if (active[q])
        {
            report(names[q], samples[2 + q]);
        }
    }
    std::printf("PROFILE vma_reserved_peak_bytes=%llu warmup=24 samples=240 per_frame_CPU_wait=0\n", peak_bytes);
    vkDestroyQueryPool(device, queries, nullptr);
    queries = {};
}
