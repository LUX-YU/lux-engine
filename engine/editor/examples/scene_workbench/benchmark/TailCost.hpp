#pragma once
#include "CostSample.hpp"
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <atomic>
#include <array>
#include <string_view>
#include <utility>

namespace er1_cost
{
    // Test-owned provider, backed by two real immutable paks. No Render/Session fault hooks.
    class RecoveryProvider final : public lux::asset::IAssetProvider
    {
    public:
        std::shared_ptr<lux::asset::IAssetProvider> initial, complete;
        std::atomic<bool> recovered{};
        mutable std::atomic<std::uint64_t> opens{}, missing{};
        const lux::asset::IAssetProvider& current() const noexcept
        {
            return *(recovered.load(std::memory_order_acquire) ? complete : initial);
        }
        std::optional<lux::asset::AssetId> resolve(std::string_view path) const override
        {
            return current().resolve(path);
        }
        bool contains(const lux::asset::AssetId& id) const override
        {
            const bool found = current().contains(id);
            if (!found) missing.fetch_add(1, std::memory_order_relaxed);
            return found;
        }
        lux::cxx::expected<lux::asset::AssetBlob, lux::asset::EAssetStorageError>
        open(const lux::asset::AssetId& id) const override
        {
            opens.fetch_add(1, std::memory_order_relaxed);
            return current().open(id);
        }
        void enumerate(const std::function<void(const lux::asset::ProviderEntry&)>& visitor) const override
        {
            current().enumerate(visitor);
        }
        std::optional<std::string> pathOf(const lux::asset::AssetId& id) const override { return current().pathOf(id); }
        bool validInputs(bool retry) const
        {
            std::array<std::uint8_t, 16> ground{};
            ground[0] = 0x53;
            ground[1] = 0x56;
            ground[2] = 1;
            ground.back() = 11;
            const lux::asset::AssetId id{ground};
            return complete->contains(id) && initial->contains(id) == !retry;
        }
    };
    struct TailState final
    {
        unsigned width{}, height{};
        std::size_t ready{}, failed{}, requests{}, handles{};
        std::uint64_t serials{}, descriptors_created{}, descriptors_retired{};
        bool resizing{};
        std::uint64_t backend_frames{};
    };
    struct TailStage final
    {
        const char* name{};
        Sample sample;
        TailState before, after;
        std::size_t attempts{}, ready_frame{};
        ProcessMemory memory_before, memory_after;
    };
    struct TailReport final
    {
        std::vector<TailStage> stages;
        std::uint64_t initial_checksum{}, descriptors_created{}, descriptors_retired{};
        double close_wall{};
        ProcessMemory closed_memory;
        template<class Frames, class Frame, class Drain, class State, class Readback>
        void measure(const char* name, unsigned w, unsigned h, Frames frames, Frame frame, Drain drain,
                     State state, Readback readback, const std::uint64_t& polls)
        {
            TailStage stage;
            stage.name = name;
            stage.before = state();
            stage.memory_before = processMemory();
            stage.sample.begin(frames(), polls);
            while (stage.sample.iterations != 120)
            {
                const auto tick = Clock::now();
                bool submitted{};
                stage.sample.work([&] {
                    submitted = frame();
                    const auto observed = state();
                    if (!stage.ready_frame && observed.width == w && observed.height == h &&
                        observed.ready == 3 && !observed.failed && !observed.resizing)
                        stage.ready_frame = stage.sample.iterations + 1;
                });
                ++stage.attempts;
                stage.sample.wait([&] { drain(); std::this_thread::sleep_until(tick + std::chrono::milliseconds{8}); });
                stage.sample.iterations += submitted;
            }
            stage.after = state();
            assert(stage.ready_frame && stage.after.width == w && stage.after.height == h &&
                   stage.after.ready == 3 && !stage.after.failed && !stage.after.resizing);
            stage.sample.frames_after = frames();
            assert(stage.sample.frames_after - stage.sample.frames_before == 120);
            stage.sample.wait([&] { stage.sample.checksum = readback(); });
            assert(state().backend_frames == stage.after.backend_frames + 8 && stage.sample.checksum);
            stage.sample.owner_used = cpu(false) - stage.sample.owner_cpu;
            stage.sample.process_used = cpu(true) - stage.sample.process_cpu;
            stage.sample.elapsed = seconds(Clock::now() - stage.sample.started);
            stage.memory_after = processMemory();
            stages.push_back(stage);
        }
        void write(const char* path, const char* stack, const RecoveryProvider& provider) const
        {
            std::ofstream out(path);
            out.precision(12);
            out << "{\n\"stack\":\"" << stack << "\",\"configuration\":\"RelWithDebInfo\","
                << "\"warmup\":100,\"window\":[1600,900],\"view_count\":1,\"cadence_seconds\":0.008,"
                << "\"initial_checksum\":\"" << initial_checksum << "\",\"provider_opens\":" << provider.opens.load()
                << ",\"provider_missing\":" << provider.missing.load() << ",\"close_completed\":true,"
                << "\"close_wall\":" << close_wall << ",\"descriptors_created\":" << descriptors_created
                << ",\"descriptors_retired\":" << descriptors_retired << ",\"stages\":[\n";
            for (std::size_t i = 0; i < stages.size(); ++i)
            {
                const auto& s = stages[i];
                if (i) out << ",\n";
                out << "{\"name\":\"" << s.name << "\",\"extent\":[" << s.after.width << ',' << s.after.height
                    << "],\"completed_scene_frames\":120,\"verification_frames\":8,\"attempts\":" << s.attempts
                    << ",\"other_backend_frames\":" << s.after.backend_frames - s.before.backend_frames - 120
                    << ",\"ready_frame\":" << s.ready_frame << ",\"work_wall\":" << s.sample.work_wall
                    << ",\"wait_wall\":" << s.sample.wait_wall << ",\"owner_cpu\":" << s.sample.owner_used
                    << ",\"process_cpu\":" << s.sample.process_used << ",\"work_cycles\":" << s.sample.work_cycles
                    << ",\"wait_cycles\":" << s.sample.wait_cycles << ",\"work_polls\":" << s.sample.work_polls
                    << ",\"wait_polls\":" << s.sample.wait_polls << ",\"ready\":" << s.after.ready
                    << ",\"failed\":" << s.after.failed << ",\"requests\":" << s.after.requests
                    << ",\"handles\":" << s.after.handles << ",\"serials_before\":" << s.before.serials
                    << ",\"serials_after\":" << s.after.serials
                    << ",\"private_bytes_before\":" << s.memory_before.private_bytes
                    << ",\"private_bytes_after\":" << s.memory_after.private_bytes
                    << ",\"working_set_after\":" << s.memory_after.working_set
                    << ",\"peak_working_set\":" << s.memory_after.peak_working_set
                    << ",\"descriptors_created\":" << s.after.descriptors_created
                    << ",\"descriptors_retired\":" << s.after.descriptors_retired
                    << ",\"checksum\":\"" << s.sample.checksum << "\"}";
            }
            out << "\n],\"closed_private_bytes\":" << closed_memory.private_bytes
                << ",\"closed_working_set\":" << closed_memory.working_set << "}\n";
            assert(out.good());
        }
    };
} // namespace er1_cost
