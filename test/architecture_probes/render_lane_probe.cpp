#include "DeviceRenderFixture.hpp"

#include <lux/engine/scene/LatestSpscExchange.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <thread>

namespace
{
    using Clock = std::chrono::steady_clock;

    struct PresentationState final
    {
        double value{};
        std::uint64_t revision{};
    };

    struct Metrics final
    {
        std::uint64_t simulation_steps{};
        std::uint64_t publishes{};
        std::uint64_t acquires{};
        std::uint64_t exchange_skips{};
        std::uint64_t presentation_frames{};
        std::uint64_t render_replies{};
        std::uint64_t frame_ring_backpressure{};
        std::uint64_t frame_skips{};
        std::uint64_t max_publish_nanoseconds{};
        std::uint64_t max_presentation_latency_nanoseconds{};
        double elapsed_seconds{};
    };

    struct SimulationCounters final
    {
        std::atomic_uint64_t steps{};
        std::atomic_uint64_t publishes{};
        std::atomic_uint64_t max_publish_nanoseconds{};
    };

    void updateMaximum(std::atomic_uint64_t& target, std::uint64_t value) noexcept
    {
        std::uint64_t observed = target.load(std::memory_order_relaxed);
        while (observed < value &&
               !target.compare_exchange_weak(observed, value, std::memory_order_relaxed))
        {
        }
    }

    void runSimulation(
        std::stop_token stop,
        lux::scene::LatestSpscExchange<PresentationState>& exchange,
        SimulationCounters& counters,
        std::chrono::milliseconds step_duration
    )
    {
        std::uint64_t revision = 0U;
        while (!stop.stop_requested())
        {
            ++revision;
            exchange.write() = PresentationState{
                std::sin(static_cast<double>(revision) * 0.25),
                revision
            };
            const auto publish_started = Clock::now();
            exchange.publish();
            const auto publish_finished = Clock::now();
            counters.steps.store(revision, std::memory_order_relaxed);
            counters.publishes.fetch_add(1U, std::memory_order_relaxed);
            updateMaximum(
                counters.max_publish_nanoseconds,
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(publish_finished - publish_started).count()
                )
            );
            std::this_thread::sleep_for(step_duration);
        }
    }

    [[nodiscard]] bool submitPresentationFrame(
        lux::rendertest::DeviceRenderFixture& fixture,
        Metrics& metrics
    )
    {
        auto& session = fixture.session();
        if (!session.trySubmitFrame())
        {
            ++metrics.frame_ring_backpressure;
            ++metrics.frame_skips;
            return true;
        }

        ++metrics.presentation_frames;
        if (!session.beginFrame({}))
            return false;
        return true;
    }

    [[nodiscard]] bool exerciseFrameRingBackpressure(
        lux::rendertest::DeviceRenderFixture& fixture,
        Metrics& metrics
    )
    {
        constexpr std::size_t MaxBurst = 4096U;
        auto& session = fixture.session();
        for (std::size_t frame = 0U; frame < MaxBurst; ++frame)
        {
            if (!session.trySubmitFrame())
            {
                ++metrics.frame_ring_backpressure;
                ++metrics.frame_skips;
                return true;
            }
            ++metrics.presentation_frames;
            if (!session.beginFrame({}))
                return false;
        }
        return false;
    }

    void pumpLanes(lux::rendertest::DeviceRenderFixture& fixture, Metrics& metrics)
    {
        fixture.window().pollEvents();
        metrics.render_replies += fixture.session().pumpReplies();
        fixture.control().pumpReplies();
        fixture.upload().pumpReplies();
    }

    [[nodiscard]] int runProbe(bool qualification)
    {
        constexpr std::uint32_t Width = 64U;
        constexpr std::uint32_t Height = 64U;
        constexpr int GpuUnavailable = 77;
        const auto simulation_period = qualification ? std::chrono::milliseconds(1000)
                                                     : std::chrono::milliseconds(40);
        const auto run_duration = qualification ? std::chrono::milliseconds(2200)
                                                : std::chrono::milliseconds(300);
        const std::uint32_t presentation_hz = qualification ? 144U : 120U;
        const auto presentation_period = std::chrono::nanoseconds(1'000'000'000ULL / presentation_hz);

        static std::atomic_int validation_errors{};
        Metrics metrics;
        bool gpu_available = false;
        bool readback_ok = false;

        {
            lux::rendertest::DeviceRenderFixture fixture(
                Width,
                Height,
                "architecture_probe_render_lanes",
                {.enable_validation = true, .validation_errors = &validation_errors}
            );
            if (!fixture.ok())
            {
                std::puts("SKIP: Vulkan device or validation layer unavailable");
                return qualification ? 5 : GpuUnavailable;
            }
            gpu_available = true;
            const auto scene = fixture.makeSceneWithView("LaneProbeScene", "LaneProbeView");

            lux::scene::LatestSpscExchange<PresentationState> exchange;
            SimulationCounters simulation_counters;
            std::jthread simulation(
                runSimulation,
                std::ref(exchange),
                std::ref(simulation_counters),
                simulation_period
            );

            std::uint64_t last_revision = 0U;
            const auto started = Clock::now();
            const auto deadline = started + run_duration;
            auto next_presentation = started;
            if (!exerciseFrameRingBackpressure(fixture, metrics))
                return 6;
            while (Clock::now() < deadline)
            {
                const auto iteration_started = Clock::now();
                pumpLanes(fixture, metrics);
                if (exchange.acquireLatest())
                {
                    const PresentationState& state = exchange.read();
                    if (last_revision != 0U && state.revision > last_revision + 1U)
                        metrics.exchange_skips += state.revision - last_revision - 1U;
                    last_revision = state.revision;
                    ++metrics.acquires;
                }

                if (!submitPresentationFrame(fixture, metrics))
                    return 6;

                const auto iteration_finished = Clock::now();
                metrics.max_presentation_latency_nanoseconds = std::max(
                    metrics.max_presentation_latency_nanoseconds,
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            iteration_finished - iteration_started
                        ).count()
                    )
                );
                next_presentation += presentation_period;
                std::this_thread::sleep_until(next_presentation);
            }

            const auto presentation_finished = Clock::now();
            simulation.request_stop();
            simulation.join();
            metrics.elapsed_seconds = std::chrono::duration<double>(presentation_finished - started).count();
            metrics.simulation_steps = simulation_counters.steps.load(std::memory_order_relaxed);
            metrics.publishes = simulation_counters.publishes.load(std::memory_order_relaxed);
            metrics.max_publish_nanoseconds =
                simulation_counters.max_publish_nanoseconds.load(std::memory_order_relaxed);

            const auto drain_deadline = Clock::now() + std::chrono::milliseconds(500);
            while (metrics.render_replies < metrics.presentation_frames && Clock::now() < drain_deadline)
            {
                pumpLanes(fixture, metrics);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            const auto pixels = fixture.readback(scene);
            readback_ok = fixture.lastReadback().status == 0U &&
                fixture.lastReadback().bytes_written == pixels.size() &&
                pixels.size() == static_cast<std::size_t>(Width) * Height * 4U;
        }

        const double presentation_fps = metrics.presentation_frames / metrics.elapsed_seconds;
        const double render_fps = metrics.render_replies / metrics.elapsed_seconds;
        std::printf(
            "gpu=%u,simulation_steps=%llu,publishes=%llu,acquires=%llu,exchange_skips=%llu,"
            "presentation_frames=%llu,render_replies=%llu,presentation_fps=%.3f,render_fps=%.3f,"
            "frame_ring_backpressure=%llu,frame_skips=%llu,max_publish_ns=%llu,max_presentation_latency_ns=%llu,"
            "validation_errors=%d,readback=%u\n",
            gpu_available ? 1U : 0U,
            static_cast<unsigned long long>(metrics.simulation_steps),
            static_cast<unsigned long long>(metrics.publishes),
            static_cast<unsigned long long>(metrics.acquires),
            static_cast<unsigned long long>(metrics.exchange_skips),
            static_cast<unsigned long long>(metrics.presentation_frames),
            static_cast<unsigned long long>(metrics.render_replies),
            presentation_fps,
            render_fps,
            static_cast<unsigned long long>(metrics.frame_ring_backpressure),
            static_cast<unsigned long long>(metrics.frame_skips),
            static_cast<unsigned long long>(metrics.max_publish_nanoseconds),
            static_cast<unsigned long long>(metrics.max_presentation_latency_nanoseconds),
            validation_errors.load(std::memory_order_relaxed),
            readback_ok ? 1U : 0U
        );

        const bool has_independent_simulation = metrics.publishes > 0U && metrics.acquires > 0U;
        const bool presentation_outpaces_simulation = metrics.presentation_frames > metrics.simulation_steps * 2U;
        const bool rendered = metrics.render_replies > 0U && readback_ok;
        const bool validation_clean = validation_errors.load(std::memory_order_relaxed) == 0;
        const bool backpressure_observed = metrics.frame_ring_backpressure > 0U && metrics.frame_skips > 0U;
        return has_independent_simulation && presentation_outpaces_simulation && rendered && validation_clean &&
                backpressure_observed
            ? 0
            : 7;
    }
}

int main(int argc, char** argv)
{
    const bool qualification = argc == 2 && std::string_view(argv[1]) == "--qualification";
    return runProbe(qualification);
}
