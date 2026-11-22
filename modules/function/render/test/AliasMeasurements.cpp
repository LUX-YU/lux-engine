#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/engine/render/device/JobGpuResources.hpp>
#include "BufferReadback.hpp"
#include "FrameHarness.hpp"
#include "GraphAliasLoad.hpp"
#include "NativeDevice.hpp"
#include "RetirementFence.hpp"
#include <lux/engine/render/extensions/graph/RenderGraphBuilder.hpp>
#include <lux/engine/render/graph/compile/GraphCompiler.hpp>
#include <lux/engine/render/graph/execute/ResizeResources.hpp>
#include <lux/engine/render/target/OffscreenTarget.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace
{
    using namespace foundation_test;
    using lux::render::runtime_detail::JobExecution;
    constexpr std::array group_counts{512u, 511u};
    constexpr unsigned warmup = 8;
    constexpr unsigned samples = 32;

    struct Workload final
    {
        RenderGraph graph;
        std::array<GraphResourceId, 2> words;
        std::array<GraphResourceId, 2> pixels;
        std::array<GraphPassId, 2> dispatches;
    };

    Workload buildWorkload(EGraphicsBackend backend)
    {
        RenderGraphBuilder builder;
        Workload result;
        const auto target = backend == EGraphicsBackend::D3D12 ? lux::rdesc::EShaderTarget::DXIL :
            lux::rdesc::EShaderTarget::SPIRV;
        const auto shader = take(builder.addProgram(GraphProgram{kGraphAliasLoad.load(target)}));
        const auto colors = [&](unsigned index)
        {
            const auto name = std::to_string(index);
            const unsigned width = index ? 256 : 128;
            GraphImageDesc desc{width, width};
            if (index) desc.extent.source = EImageExtentSource::TARGET;
            const auto image = take(builder.declareResource({"temporary " + name, desc}));
            const auto pixels = take(builder.declareResource({"pixel " + name, GraphBufferDesc{256}}));
            result.pixels[index] = pixels;
            const std::array color{index ? 0.f : 1.f, index ? 1.f : 0.f, 0.f, 1.f};
            const auto clear_work = TransferWork{{ClearImage{{{image, 0}}, color}}};
            const auto clear = take(builder.addTransferPass("color " + name, clear_work));
            const auto copy = take(builder.addTransferPass("save " + name, TransferWork{{CopyImageBuffer{
                {clear.written.front()}, {{pixels, 0}}, 256, 256, ImageCopyRegion{0, 0, 1, 1}
            }}}));
            CHECK(builder.exportOutput({
                copy.written.front(), GraphBufferRange{0, 4}, {}, EGraphUse::TRANSFER_DESTINATION
            }));
            return std::pair{clear.pass, copy.pass};
        };

        const auto first = colors(0);
        const auto seed = take(builder.declareResource({"seed", GraphBufferDesc{group_counts[0] * 64u * 4u}}));
        const auto fill = take(builder.addTransferPass("seed load", TransferWork{{FillBuffer{{{seed, 0}}, 17}}}));
        CHECK(builder.dependsOn(first.second, fill.pass));
        for (unsigned i = 0; i < group_counts.size(); ++i)
        {
            const auto name = std::to_string(i);
            const auto bytes = group_counts[i] * 64u * 4u;
            const auto output = take(builder.declareResource({"result " + name, GraphBufferDesc{bytes}}));
            ComputeWork work;
            work.allow_async_compute = i == 0;
            work.interface.program = shader;
            work.interface.resources = {
                {"seed", {fill.written.front()}},
                {"result", {{output, 0}, {}, EGraphAccess::WRITE}}
            };
            work.dispatch = DispatchArguments{{group_counts[i], 1, 1}};
            const auto pass = take(builder.addComputePass("hash " + name, std::move(work)));
            result.words[i] = output;
            result.dispatches[i] = pass.pass;
            CHECK(builder.exportOutput({pass.written.front(), {}, {}, EGraphUse::TRANSFER_DESTINATION}));
        }
        const auto last = colors(1);
        for (const auto pass : result.dispatches) CHECK(builder.dependsOn(pass, last.first));
        result.graph = take(std::move(builder).finish());
        return result;
    }

    struct QueryPair final
    {
        nri::QueryPool* pool = nullptr;
        nri::Buffer* readback = nullptr;
    };

    struct QueueClock final
    {
        std::uint64_t origin = 0;
        std::uint64_t frequency = 0;
        std::uint32_t bits = 64;
        double offset_ns = 0;
        double calibration_window_ns = 0;

        [[nodiscard]] double nanoseconds(std::uint64_t tick) const noexcept
        {
            auto delta = tick - origin;
            if (bits < 64)
            {
                const auto wrap = UINT64_C(1) << bits;
                delta &= wrap - 1;
                if (delta >= wrap / 2) delta -= wrap;
            }
            return offset_ns + double(std::bit_cast<std::int64_t>(delta)) * 1e9 / double(frequency);
        }
    };

    QueueClock queueClock(DeviceSession& device, SubmissionQueue& queue)
    {
        QueueClock clock;
        clock.frequency = device.description().other.timestampFrequencyHz;
        clock.bits = queue.properties().timestamp_bits;
        CHECK(clock.bits && clock.frequency);
        if (device.description().graphicsAPI == nri::GraphicsAPI::D3D12)
        {
            auto* native = static_cast<ID3D12CommandQueue*>(device.nriCore().GetQueueNativeObject(&queue.native()));
            LARGE_INTEGER qpc_frequency, start, end;
            UINT64 cpu = 0;
            CHECK(QueryPerformanceFrequency(&qpc_frequency));
            CHECK(SUCCEEDED(native->GetTimestampFrequency(&clock.frequency)));
            CHECK(QueryPerformanceCounter(&start));
            CHECK(SUCCEEDED(native->GetClockCalibration(&clock.origin, &cpu)));
            CHECK(QueryPerformanceCounter(&end));
            clock.offset_ns = double(cpu) * 1e9 / double(qpc_frequency.QuadPart);
            clock.calibration_window_ns = double(end.QuadPart - start.QuadPart) * 1e9 / double(qpc_frequency.QuadPart);
        }
        return clock;
    }

    struct SubmitCounts final
    {
        std::uint32_t submissions = 0;
        std::uint32_t waits = 0;
    };

    // Installed only on this isolated test session. Timestamps are recorded in the actual graph CBs.
    class NativeCapture final
    {
    public:
        NativeCapture(DeviceSession& device, bool timestamps) noexcept
            : device_(device), core_(const_cast<nri::CoreInterface&>(device.nriCore())), original_(core_)
        {
            CHECK(current_ == nullptr);
            for (auto& pair : queries_)
            {
                const nri::QueryPoolDesc desc{nri::QueryType::TIMESTAMP, 2};
                CHECK(core_.CreateQueryPool(device.device(), desc, pair.pool) == nri::Result::SUCCESS);
                CHECK(core_.CreatePlacedBuffer(
                    device.device(), NriHostReadbackHeap, {16, 0, nri::BufferUsageBits::NONE}, pair.readback
                ) == nri::Result::SUCCESS);
            }
            current_ = this;
            if (timestamps) core_.CmdDispatch = dispatch;
            core_.QueueSubmit = submit;
            core_.AllocateMemory = allocate;
            core_.FreeMemory = release;
        }

        ~NativeCapture()
        {
            core_.CmdDispatch = original_.CmdDispatch;
            core_.QueueSubmit = original_.QueueSubmit;
            core_.AllocateMemory = original_.AllocateMemory;
            core_.FreeMemory = original_.FreeMemory;
            current_ = nullptr;
            for (auto& pair : queries_)
            {
                core_.DestroyQueryPool(pair.pool);
                core_.DestroyBuffer(pair.readback);
            }
        }

        NativeCapture(const NativeCapture&) = delete;
        NativeCapture& operator=(const NativeCapture&) = delete;

        void resetQueries()
        {
            for (const auto& pair : queries_) core_.ResetQueries(*pair.pool, 0, 2);
        }

        std::array<std::uint64_t, 2> read(unsigned lane)
        {
            auto* buffer = queries_[lane].readback;
            const auto* values = static_cast<const std::uint64_t*>(core_.MapBuffer(*buffer, 0, 16));
            CHECK(values && device_.invalidateMappedRange(*buffer, 0, 16) == nri::Result::SUCCESS);
            const std::array result{values[0], values[1]};
            core_.UnmapBuffer(*buffer);
            return result;
        }

        SubmitCounts counts() const noexcept { return {submissions_.load(), waits_.load()}; }

        std::uint64_t describeMemory(const char* phase)
        {
            const std::lock_guard lock(memory_mutex_);
            std::printf("MEMORY,%s,%zu,%llu,%llu\n", phase, bytes_.size(),
                        static_cast<unsigned long long>(live_), static_cast<unsigned long long>(peak_));
            return live_;
        }

    private:
        static void NRI_CALL dispatch(nri::CommandBuffer& cb, const nri::DispatchDesc& desc)
        {
            auto& self = *current_;
            const unsigned lane = desc.workGroupNumX == group_counts[0] ? 0 : 1;
            CHECK(desc.workGroupNumX == group_counts[lane]);
            auto& pair = self.queries_[lane];
            self.original_.CmdEndQuery(cb, *pair.pool, 0);
            self.original_.CmdDispatch(cb, desc);
            self.original_.CmdEndQuery(cb, *pair.pool, 1);
            self.original_.CmdCopyQueries(cb, *pair.pool, 0, 2, *pair.readback, 0);
        }

        static nri::Result NRI_CALL submit(nri::Queue& queue, const nri::QueueSubmitDesc& desc)
        {
            auto& self = *current_;
            const auto result = self.original_.QueueSubmit(queue, desc);
            if (result == nri::Result::SUCCESS)
            {
                ++self.submissions_;
                self.waits_ += desc.waitFenceNum;
            }
            return result;
        }

        static nri::Result NRI_CALL
        allocate(nri::Device& device, const nri::AllocateMemoryDesc& desc, nri::Memory*& memory)
        {
            auto& self = *current_;
            const auto result = self.original_.AllocateMemory(device, desc, memory);
            if (result == nri::Result::SUCCESS)
            {
                const std::lock_guard lock(self.memory_mutex_);
                CHECK(self.bytes_.emplace(memory, desc.size).second);
                self.live_ += desc.size;
                self.peak_ = std::max(self.peak_, self.live_);
            }
            return result;
        }

        static void NRI_CALL release(nri::Memory* memory)
        {
            auto& self = *current_;
            const std::lock_guard lock(self.memory_mutex_);
            self.original_.FreeMemory(memory);
            if (const auto it = self.bytes_.find(memory); it != self.bytes_.end())
            {
                self.live_ -= it->second;
                self.bytes_.erase(it);
            }
        }

        inline static NativeCapture* current_ = nullptr;
        DeviceSession& device_;
        nri::CoreInterface& core_;
        nri::CoreInterface original_;
        std::array<QueryPair, 2> queries_{};
        std::atomic_uint32_t submissions_{0}, waits_{0};
        std::mutex memory_mutex_;
        std::unordered_map<nri::Memory*, std::uint64_t> bytes_;
        std::uint64_t live_ = 0, peak_ = 0;
    };

    struct SampleContext final
    {
        DeviceSession& device;
        FrameHarness& frames;
        JobExecution& job;
        JobGpuResources& gpu;
        NativeCapture& capture;
        std::array<SubmissionQueue*, 2> queues;
    };

    void submitFrame(SampleContext& context)
    {
        const auto instances = std::array{&context.job};
        auto submitted = take(runTask(context.frames.render(instances, {})));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (context.frames.submitting())
        {
            CHECK(std::chrono::steady_clock::now() < deadline);
            CHECK(context.device.progressCompletions());
            submitted += take(runTask(context.frames.render({}, {})));
            if (context.frames.submitting()) take(context.frames.waitForProgress(1'000'000));
        }
        CHECK(submitted);
    }

    void measureExtent(SampleContext& context, unsigned width)
    {
        for (unsigned i = 0; i < warmup + samples; ++i)
        {
            auto& device = context.device;
            auto& capture = context.capture;
            capture.resetQueries();
            auto clocks = std::array{queueClock(device, *context.queues[0]), queueClock(device, *context.queues[1])};
            if (context.queues[0] == context.queues[1]) clocks[1] = clocks[0];
            const auto before = capture.counts();
            submitFrame(context);
            CHECK(device.drain());
            const auto after = capture.counts();
            const std::array ticks{capture.read(0), capture.read(1)};
            if (device.description().graphicsAPI == nri::GraphicsAPI::VK)
            {
                // All timestamps of a Vulkan physical device use one device time domain.
                // Compare the common valid low bits relative to the first timestamp, also across wrap.
                clocks[0].origin = clocks[1].origin = ticks[0][0];
                clocks[0].bits = clocks[1].bits = std::min(clocks[0].bits, clocks[1].bits);
            }
            const double a0 = clocks[0].nanoseconds(ticks[0][0]), a1 = clocks[0].nanoseconds(ticks[0][1]);
            const double b0 = clocks[1].nanoseconds(ticks[1][0]), b1 = clocks[1].nanoseconds(ticks[1][1]);
            CHECK(a1 > a0 && b1 > b0 && after.submissions > before.submissions);
            if (i < warmup) continue;
            std::printf(
                "SAMPLE,%u,%u,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%u,%u\n",
                width, i - warmup, a0, a1, b0, b1, clocks[0].calibration_window_ns,
                clocks[1].calibration_window_ns, after.submissions - before.submissions, after.waits - before.waits
            );
        }
    }

    void checkOutputs(DeviceSession& device, const JobExecution& job, const Workload& workload)
    {
        std::array<std::uint32_t, 4> expected{};
        for (unsigned x = 0; x < expected.size(); ++x)
        {
            auto value = 17u ^ x;
            for (unsigned i = 0; i < 4096; ++i) value = (value ^ (value >> 13)) * 1664525u + i;
            expected[x] = value;
        }
        for (unsigned i = 0; i < group_counts.size(); ++i)
        {
            CHECK(readBufferWords<4>(device, *job.resourceInput(workload.words[i], 0).buffer) == expected);
            const auto pixel = readBufferWords<1>(device, *job.resourceInput(workload.pixels[i], 0).buffer);
            CHECK(pixel[0] == (i ? 0xff00ff00u : 0xff0000ffu));
        }
    }

    void measureInFlightResize(
        SampleContext& sample, const Workload& workload, const PreparedRenderGraph& plan, TargetOutput final_target
    )
    {
        auto& device = sample.device;
        auto& graph = sample.job.instance;
        auto& capture = sample.capture;
        auto& queue = device.queue();
        auto middle_desc = final_target.description;
        middle_desc.width = middle_desc.height = 384;
        auto middle = take(OffscreenTarget::create(device, middle_desc, 1));
        RetirementFence gate(device);
        const nri::FenceSubmitDesc wait{gate.fence, 1, nri::StageBits::ALL};
        auto retiring_target = take(OffscreenTarget::create(device, graph.target(), 1));
        std::uint64_t gate_serial = 0;
        std::uint64_t before_retirement = 0;
        {
            // The 256 frame has completed. This later wait belongs to another submission.
            auto first = take(RenderGraphInstance::ResizeResources::prepare(sample.gpu, graph, middle.output()));
            CHECK(graph.prepareResizeAdoption(first));
            graph.commitResize(std::move(first));
            gate_serial = take(runTask(queue.submit({}, std::span{&wait, 1})));
            before_retirement = capture.describeMemory("384-adopted-with-old-256");
        }
        CHECK(device.progressCompletions());
        const auto live_middle = capture.describeMemory("384-after-completed-256-release");
        CHECK(live_middle < before_retirement && queue.completed() < gate_serial);
        {
            FrameHarness retiring_frames(device, 1);
            JobGpuResources retiring_gpu(device, 1);
            JobExecution retiring_job{retiring_gpu, plan.graph, testInstallation(), retiring_target.output()};
            auto& retiring = retiring_job.instance;
            CHECK(retiring_job.initialize());
            SampleContext pending{device, retiring_frames, retiring_job, retiring_gpu, capture, sample.queues};
            submitFrame(pending);
            CHECK(queue.submitted() > gate_serial);
            before_retirement = capture.describeMemory("384-live-256-submitted");
        }
        CHECK(device.progressCompletions());
        CHECK(capture.describeMemory("384-live-256-retiring") == before_retirement);
        submitFrame(sample);
        const auto accepted = queue.submitted();
        CHECK(accepted > gate_serial && queue.poll() && queue.completed() < gate_serial);
        CHECK(capture.describeMemory("384-accepted-with-retiring-256") == before_retirement);
        {
            auto second = take(RenderGraphInstance::ResizeResources::prepare(sample.gpu, graph, final_target));
            const auto peak = capture.describeMemory("384-live-512-candidate-256-retiring");
            CHECK(peak > before_retirement);
            CHECK(device.progressCompletions() && queue.completed() < gate_serial);
            CHECK(capture.describeMemory("before-host-signal") == peak);
            std::printf("FRONTIER,held,%llu,%llu,%llu\n",
                        static_cast<unsigned long long>(gate_serial), static_cast<unsigned long long>(accepted),
                        static_cast<unsigned long long>(queue.completed()));
            gate.signal();
            CHECK(take(queue.wait(accepted, 5'000'000'000)) && device.drain());
            std::printf("FRONTIER,released,%llu,%llu,%llu\n",
                        static_cast<unsigned long long>(gate_serial), static_cast<unsigned long long>(accepted),
                        static_cast<unsigned long long>(queue.completed()));
            CHECK(capture.describeMemory("384-live-512-candidate") < peak);
            checkOutputs(device, sample.job, workload);
            CHECK(graph.prepareResizeAdoption(second));
            graph.commitResize(std::move(second));
        }
        CHECK(device.drain());
    }

    int run(DeviceSession& device, EGraphicsBackend backend, bool multiple, bool inflight)
    {
        using namespace lux::render;
        const auto workload = buildWorkload(backend);
        const auto analysis = take(analyzeRenderGraph(workload.graph));
        const TargetDesc target{256, 256, lux::rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
        const EffectOutputDesc contract{target.format, 1, target.kind, target.sampled};
        auto candidate = stdexec::sync_wait(GraphCompiler::compile(
            device, lux::execution::ExecutionRuntime::instance().cpuScheduler(), {workload.graph, analysis},
            {contract, ETransientAllocation::ALIAS_MEMORY, multiple}
        ));
        CHECK(candidate);
        auto plan = take(std::move(std::get<0>(*candidate)));
        std::array<SubmissionQueue*, 2> queues;
        for (unsigned i = 0; i < queues.size(); ++i)
        {
            const auto slot = plan.diagnostics.passSlot(workload.dispatches[i]);
            queues[i] = &device.queue(plan.graph.passes()[slot].queue);
        }
        const bool distinct = queues[0] != queues[1];
        std::printf("TOPOLOGY,%s,%s,%u,%zu,%zu,%zu\n", backend == EGraphicsBackend::VULKAN ? "Vulkan" : "D3D12",
                    device.description().adapterDesc.name, unsigned(multiple), device.queueIndex(*queues[0]),
                    device.queueIndex(*queues[1]), plan.graph.submissions().size());
        if (multiple && !distinct) return 77;
        CHECK(multiple == distinct);
        if (!queues[0]->properties().timestamp_bits || !queues[1]->properties().timestamp_bits) return 77;

        auto initial = take(OffscreenTarget::create(device, target, 1));
        auto larger = target;
        larger.width = larger.height = 512;
        auto replacement = take(OffscreenTarget::create(device, larger, 1));
        FrameHarness frames(device, 1);
        NativeCapture capture(device, !inflight);
        {
            JobGpuResources instance_gpu(device, 1);
            JobExecution instance_job{instance_gpu, plan.graph, testInstallation(), initial.output()};
            auto& instance = instance_job.instance;
            CHECK(instance_job.initialize());
            std::string dump;
            CHECK(instance_job.dumpResources({&dump, [](void* p, std::string_view value) noexcept
            {
                static_cast<std::string*>(p)->append(value);
                return true;
            }}));
            CHECK(dump.find("\"reason\":\"aliased\"") != std::string::npos);
            std::printf("PLACEMENT,%s\n", dump.c_str());
            capture.describeMemory("initial");
            SampleContext sample{device, frames, instance_job, instance_gpu, capture, queues};
            if (inflight)
            {
                capture.resetQueries();
                submitFrame(sample);
                CHECK(device.drain());
            }
            else measureExtent(sample, target.width);
            checkOutputs(device, instance_job, workload);
            if (inflight) measureInFlightResize(sample, workload, plan, replacement.output());
            else
            {
                auto resized = take(RenderGraphInstance::ResizeResources::prepare(
                    instance_gpu, instance, replacement.output()
                ));
                capture.describeMemory("candidate");
                CHECK(instance.prepareResizeAdoption(resized));
                instance.commitResize(std::move(resized));
                capture.describeMemory("adopted-before-retirement");
            }
            CHECK(device.drain());
            capture.describeMemory("resized");
            if (inflight)
            {
                capture.resetQueries();
                submitFrame(sample);
                CHECK(device.drain());
            }
            else measureExtent(sample, larger.width);
            checkOutputs(device, instance_job, workload);
        }
        CHECK(device.drain());
        CHECK(capture.describeMemory("drained") == 0);
        return 0;
    }
}

int main(int argc, char** argv)
{
    using namespace foundation_test;
    const bool inflight = argc == 4 && std::string_view{argv[3]} == "inflight-resize";
    if (argc != 3 && !inflight) return 2;
    const std::string_view api{argv[1]}, topology{argv[2]};
    if ((api != "d3d12" && api != "vulkan") || (topology != "single" && topology != "multi")) return 2;
    auto execution = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    CHECK(execution);
    Validation validation;
    DeviceOptions options;
    options.backend = api == "d3d12" ? EGraphicsBackend::D3D12 : EGraphicsBackend::VULKAN;
    options.multiple_queues = topology == "multi";
    TestDevice session(options, diagnostics(validation), nativeValidationEnabled());
    const auto status = run(*session, options.backend, options.multiple_queues, inflight);
    session.reset();
    CHECK(validation.errors == 0);
    return status;
}
