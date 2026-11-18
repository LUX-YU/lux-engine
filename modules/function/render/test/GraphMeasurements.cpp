#include "GraphMeasurements.hpp"
#include "Support.hpp"
#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

namespace foundation_test
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        constexpr std::size_t kWarmup = 12;
        constexpr std::size_t kSamples = 100;

        struct Sample final
        {
            double prepare_ns;
            double record_ns;
            double submit_ns;
            double gpu_ns;
            std::size_t prepare_allocations;
            std::size_t record_allocations;
        };

        double elapsedNs(Clock::time_point start) noexcept
        {
            return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        }

        void report(const std::array<Sample, kSamples>& samples, const char* label, double Sample::* metric)
        {
            std::array<double, kSamples> values{};
            std::ranges::transform(samples, values.begin(), [metric](const auto& sample) { return sample.*metric; });
            std::ranges::sort(values);
            std::printf("%s p50_ns=%.0f p95_ns=%.0f\n", label, values[kSamples / 2 - 1], values[kSamples * 95 / 100 - 1]);
        }
    }

    int measureGraph(DeviceSession& session, const PreparedRenderGraph& candidate)
    {
        auto& queue = session.queue();
        const auto bits = queue.properties().timestamp_bits;
        const auto frequency = session.description().other.timestampFrequencyHz;
        if (bits == 0 || frequency == 0)
        {
            std::puts("Graphics queue timestamps unavailable");
            return 77;
        }

        startGraphMeasurements();
        auto instance = std::make_unique<RenderGraphInstance>(session, 1, candidate);
        const auto construction_allocations = stopGraphMeasurements();
        CHECK(construction_allocations > 0); // Prove the counter observes allocation inside the Render DLL.
        std::printf("construction_allocations=%zu\n", construction_allocations);
        CHECK(instance->initialize());
        CHECK(candidate.executable.submissions().size() == 1);
        CHECK(candidate.executable.submissions().front().queue == EQueueRole::GRAPHICS);
        CHECK(candidate.executable.submissions().front().reusable_recording);

        const auto& core = session.nriCore();
        nri::QueryPool* queries = nullptr;
        nri::Buffer* results = nullptr;
        nri::CommandAllocator* allocator = nullptr;
        nri::CommandBuffer* begin = nullptr;
        nri::CommandBuffer* end = nullptr;
        auto cleanup = lux::cxx::scope_exit([&]() noexcept
        {
            core.DestroyCommandBuffer(end);
            core.DestroyCommandBuffer(begin);
            core.DestroyCommandAllocator(allocator);
            core.DestroyBuffer(results);
            core.DestroyQueryPool(queries);
        });
        CHECK(core.CreateQueryPool(session.device(), {nri::QueryType::TIMESTAMP, 2}, queries) == nri::Result::SUCCESS);
        constexpr auto bytes = 2u * sizeof(std::uint64_t);
        CHECK(core.CreatePlacedBuffer(
            session.device(), NriHostReadbackHeap, {bytes, 0, nri::BufferUsageBits::NONE}, results
        ) == nri::Result::SUCCESS);
        CHECK(core.CreateCommandAllocator(queue.native(), allocator) == nri::Result::SUCCESS);
        CHECK(core.CreateCommandBuffer(*allocator, begin) == nri::Result::SUCCESS);
        CHECK(core.CreateCommandBuffer(*allocator, end) == nri::Result::SUCCESS);
        CHECK(session.beginCommandBuffer(*begin, true) == nri::Result::SUCCESS);
        core.CmdEndQuery(*begin, *queries, 0);
        CHECK(core.EndCommandBuffer(*begin) == nri::Result::SUCCESS);
        CHECK(session.beginCommandBuffer(*end, true) == nri::Result::SUCCESS);
        core.CmdEndQuery(*end, *queries, 1);
        core.CmdCopyQueries(*end, *queries, 0, 2, *results, 0);
        CHECK(core.EndCommandBuffer(*end) == nri::Result::SUCCESS);

        const auto sample = [&]()
        {
            Sample measured{};
            startGraphMeasurements();
            const auto prepare_start = Clock::now();
            const auto prepared = instance->prepareFrame(0);
            measured.prepare_ns = elapsedNs(prepare_start);
            measured.prepare_allocations = stopGraphMeasurements();
            CHECK(prepared && *prepared);

            startGraphMeasurements();
            const auto record_start = Clock::now();
            const auto recorded = instance->recordBatch(0, 0);
            measured.record_ns = elapsedNs(record_start);
            measured.record_allocations = stopGraphMeasurements();
            CHECK(recorded && recorded->size() == 1);
            const std::array commands{begin, recorded->front(), end};

            // The previous sample drained before reset. Graph timestamps wrap at the queue's valid bit count.
            core.ResetQueries(*queries, 0, 2);
            const auto submit_start = Clock::now();
            const auto submitted = runTask(queue.submit(commands));
            measured.submit_ns = elapsedNs(submit_start);
            CHECK(submitted);
            CHECK(session.drain());

            const auto* timestamps = static_cast<const std::uint64_t*>(core.MapBuffer(*results, 0, bytes));
            CHECK(timestamps);
            CHECK(session.invalidateMappedRange(*results, 0, bytes) == nri::Result::SUCCESS);
            const auto mask = bits < 64 ? (std::uint64_t{1} << bits) - 1 : UINT64_MAX;
            measured.gpu_ns = double((timestamps[1] - timestamps[0]) & mask) * 1'000'000'000.0 / frequency;
            core.UnmapBuffer(*results);
            return measured;
        };

        const auto cold = sample();
        std::printf(
            "first_prepare_ns=%.0f first_record_ns=%.0f prepare_allocations=%zu record_allocations=%zu\n",
            cold.prepare_ns, cold.record_ns, cold.prepare_allocations, cold.record_allocations
        );
        for (std::size_t i = 0; i < kWarmup; ++i) sample();

        std::array<Sample, kSamples> samples;
        for (auto& measured : samples)
        {
            measured = sample();
            CHECK(measured.prepare_allocations == 0);
            CHECK(measured.record_allocations == 0);
        }
        std::puts("prepare_ns,record_ns,submit_ns,gpu_ns,prepare_allocations,record_allocations");
        for (const auto& measured : samples)
        {
            std::printf(
                "%.0f,%.0f,%.0f,%.0f,%zu,%zu\n", measured.prepare_ns, measured.record_ns, measured.submit_ns,
                measured.gpu_ns, measured.prepare_allocations, measured.record_allocations
            );
        }
        report(samples, "prepare", &Sample::prepare_ns);
        report(samples, "record_reuse", &Sample::record_ns);
        report(samples, "submit_with_timestamp_commands", &Sample::submit_ns);
        report(samples, "gpu_between_timestamps", &Sample::gpu_ns);
        std::printf("samples=%zu fif=1 prepare_allocations=0 record_allocations=0\n", samples.size());
        return 0;
    }
}
