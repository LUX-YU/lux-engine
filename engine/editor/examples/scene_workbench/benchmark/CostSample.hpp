#pragma once
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

namespace er1_cost
{
    using Clock = std::chrono::steady_clock;
    inline constexpr unsigned warmup = 100, measured = 500;
    inline constexpr unsigned width = 1024, height = 576;
    inline constexpr double delta = 0.008;
    inline double seconds(Clock::duration value)
    {
        return std::chrono::duration<double>(value).count();
    }
    double cpu(bool process);
    std::uint64_t cycles();
    struct Sample final
    {
        Clock::time_point started;
        double owner_cpu{}, process_cpu{}, work_wall{}, wait_wall{};
        double elapsed{}, owner_used{}, process_used{};
        std::uint64_t frames_before{}, frames_after{}, checksum{};
        std::uint64_t work_cycles{}, wait_cycles{};
        std::uint64_t work_polls{}, wait_polls{};
        const std::uint64_t *poll_count{};
        unsigned iterations{};
        void begin(std::uint64_t frames, const std::uint64_t &polls)
        {
            poll_count = &polls;
            frames_before = frames;
            owner_cpu = cpu(false);
            process_cpu = cpu(true);
            started = Clock::now();
        }
        template <class F> void work(F &&fn)
        {
            const auto before = Clock::now();
            const auto first_cycle = cycles();
            const auto first_poll = *poll_count;
            fn();
            work_polls += *poll_count - first_poll;
            work_cycles += cycles() - first_cycle;
            work_wall += seconds(Clock::now() - before);
        }
        template <class F> void wait(F &&fn)
        {
            const auto before = Clock::now();
            const auto first_cycle = cycles();
            const auto first_poll = *poll_count;
            fn();
            wait_polls += *poll_count - first_poll;
            wait_cycles += cycles() - first_cycle;
            wait_wall += seconds(Clock::now() - before);
        }
        void finish(std::uint64_t frames)
        {
            frames_after = frames;
            assert(iterations == measured && frames_after - frames_before == measured && checksum);
            owner_used = cpu(false) - owner_cpu;
            process_used = cpu(true) - process_cpu;
            elapsed = seconds(Clock::now() - started);
        }
        void write(const char *path, const char *stack, double close_wall)
        {
            std::ofstream output(path);
            output.precision(12);
            output << "{\n\"stack\":\"" << stack << "\",\n\"configuration\":\"RelWithDebInfo\","
                   << "\n\"warmup\":" << warmup << ",\n\"iterations\":" << iterations
                   << ",\n\"view_count\":1,\n\"window\":[1600,900],\n\"extent\":[1024,576],"
                   << "\n\"verification_frames\":8,"
                   << "\n\"wait_boundary\":\"per-frame-recorded\","
                   << "\n\"cadence_seconds\":0.008,\n\"completed_scene_frames\":" << frames_after - frames_before
                   << ",\n\"work_wall_seconds\":" << work_wall << ",\n\"explicit_wait_seconds\":" << wait_wall
                   << ",\n\"elapsed_seconds\":" << elapsed << ",\n\"owner_cpu_seconds\":" << owner_used
                   << ",\n\"process_cpu_seconds\":" << process_used << ",\n\"close_wall_seconds\":" << close_wall
                   << ",\n\"owner_work_cycles\":" << work_cycles << ",\n\"owner_wait_cycles\":" << wait_cycles
                   << ",\n\"work_polls\":" << work_polls << ",\n\"wait_polls\":" << wait_polls
                   << ",\n\"close_completed\":true,\n\"checksum\":\"" << checksum << "\"\n}\n";
            assert(output.good());
        }
    };
    inline std::uint64_t checksum(const std::vector<std::byte> &pixels)
    {
        std::uint64_t result = 14695981039346656037ULL;
        // The actual target uses BGRA; compare RGB only, matching existing GPU readback evidence.
        for (std::size_t i = 0; i < pixels.size(); i += 4)
            for (auto channel : {2, 1, 0})
                result = (result ^ std::to_integer<unsigned char>(pixels[i + channel])) * 1099511628211ULL;
        return result;
    }
    inline void savePixels(const char *sample_path, const std::vector<std::byte> &pixels)
    {
        std::ofstream output(std::string(sample_path) + ".ppm", std::ios::binary);
        output << "P6\n" << width << ' ' << height << "\n255\n";
        for (std::size_t i = 0; i < pixels.size(); i += 4)
        {
            const char rgb[]{char(pixels[i + 2]), char(pixels[i + 1]), char(pixels[i])};
            output.write(rgb, 3);
        }
        assert(output.good());
    }
} // namespace er1_cost
