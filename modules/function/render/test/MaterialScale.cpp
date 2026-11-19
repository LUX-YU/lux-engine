#include "MaterialFixture.hpp"
#include "SceneCapture.hpp"

#include <bit>
#include <ctime>
#include <mutex>
#include <set>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/render/features/mesh/GBufferFeature.hpp>
#include <lux/engine/render/features/mesh/MeshVisibilityFeature.hpp>
#include <lux/engine/render/features/mesh/RasterMeshParameters.hpp>
#include <lux/engine/render/features/postprocess/TonemapFeature.hpp>
#ifdef _WIN32
#include "Allocation.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

namespace
{
    using rdesc::EMaterialPurpose;
    using rdesc::ETextureFormat;

    double processCpuMicroseconds() noexcept
    {
#ifdef _WIN32
        FILETIME started{}, exited{}, kernel{}, user{};
        CHECK(GetProcessTimes(GetCurrentProcess(), &started, &exited, &kernel, &user));
        const auto ticks = [](FILETIME time)
        {
            return (std::uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };
        return double(ticks(kernel) + ticks(user)) / 10;
#else
        return double(std::clock()) * (1'000'000.0 / CLOCKS_PER_SEC);
#endif
    }

    struct MemorySample final
    {
        std::uint64_t buffers = 0, views = 0, buffer_bytes = 0, memory_bytes = 0, upload_bytes = 0;
    };

    // Commands are serialized around the allocation sample; the counters are also written by native recording.
    class MaterialAllocationProbe final
    {
    public:
        struct Descriptor final
        {
            using Config = void;
            using Ops = void;
            static inline const ResourceDefinition descriptor{ResourceNameView{"test.material.allocations"}, 0, 0, {}};
        };

        static inline std::atomic<bool> measuring = false;
        static inline std::atomic<std::uint64_t> buffers = 0, views = 0, buffer_bytes = 0;
        static inline std::atomic<std::uint64_t> memory_bytes = 0, upload_bytes = 0;

        static MemorySample sample() noexcept
        {
            return {buffers.load(), views.load(), buffer_bytes.load(), memory_bytes.load(), upload_bytes.load()};
        }

        static bool measuredBuffer(const nri::BufferDesc& desc) noexcept
        {
            constexpr auto material_usage = nri::BufferUsageBits::SHADER_RESOURCE |
                nri::BufferUsageBits::SHADER_RESOURCE_STORAGE;
            return measuring.load(std::memory_order_relaxed) && desc.structureStride != 0 &&
                desc.usage == material_usage;
        }

        explicit MaterialAllocationProbe(const nri::CoreInterface& core) noexcept
            : core_(const_cast<nri::CoreInterface&>(core))
        {
            original_ = core_;
            core_.CreatePlacedBuffer = [](
                nri::Device& device, nri::Memory* memory, std::uint64_t offset,
                const nri::BufferDesc& desc, nri::Buffer*& buffer
            ) noexcept
            {
                const auto result = original_.CreatePlacedBuffer(device, memory, offset, desc, buffer);
                if (result == nri::Result::SUCCESS && measuredBuffer(desc))
                {
                    nri::MemoryDesc memory_desc{};
                    original_.GetBufferMemoryDesc(*buffer, nri::MemoryLocation::DEVICE, memory_desc);
                    ++buffers;
                    buffer_bytes += desc.size;
                    memory_bytes += memory_desc.size;
                }
                return result;
            };
            core_.CreateBufferView = [](const nri::BufferViewDesc& desc, nri::Descriptor*& view) noexcept
            {
                const auto result = original_.CreateBufferView(desc, view);
                if (result == nri::Result::SUCCESS && desc.type == nri::BufferView::STRUCTURED_BUFFER &&
                    measuredBuffer(original_.GetBufferDesc(*desc.buffer)))
                {
                    ++views;
                }
                return result;
            };
            core_.CmdCopyBuffer = [](
                nri::CommandBuffer& command, nri::Buffer& destination, std::uint64_t offset,
                const nri::Buffer& source, std::uint64_t source_offset, std::uint64_t bytes
            ) noexcept
            {
                if (measuredBuffer(original_.GetBufferDesc(destination))) upload_bytes += bytes;
                original_.CmdCopyBuffer(command, destination, offset, source, source_offset, bytes);
            };
        }

        ~MaterialAllocationProbe() noexcept
        {
            core_.CreatePlacedBuffer = original_.CreatePlacedBuffer;
            core_.CreateBufferView = original_.CreateBufferView;
            core_.CmdCopyBuffer = original_.CmdCopyBuffer;
        }

        static RenderResult<std::unique_ptr<MaterialAllocationProbe>>
        create(ResourceCreateContext& context, ResourceInstanceId) noexcept
        {
            return std::make_unique<MaterialAllocationProbe>(context.device.nriCore());
        }

        static ResourceRegistration factory() noexcept
        {
            return makeResourceRegistration<Descriptor, MaterialAllocationProbe, &create>(nullptr);
        }

    private:
        static inline nri::CoreInterface original_{};
        nri::CoreInterface& core_;
    };

    class PipelineIsolationProbe final
    {
    public:
        struct Descriptor final
        {
            using Config = void;
            using Ops = void;
            static inline const ResourceDefinition descriptor{ResourceNameView{"test.material.pipeline_gate"}, 0, 0, {}};
        };

        enum class EGate { OPEN, ARMED, HOLDING };
        static inline std::atomic<EGate> gate = EGate::OPEN;
        static inline std::atomic<std::uint64_t> accepted_submits = 0, raster_submits = 0;
        static inline std::atomic<std::uint64_t> pipeline_calls = 0, created_pipelines = 0, destroyed_pipelines = 0;
        static inline std::atomic<bool> fail_pipeline = false;

        static void waitForDriver() noexcept
        {
            const auto deadline = Clock::now() + 15s;
            while (gate.load() != EGate::HOLDING)
            {
                CHECK(Clock::now() < deadline);
                std::this_thread::sleep_for(1ms);
            }
        }

        static void releaseDriver() noexcept
        {
            gate = EGate::OPEN;
            gate.notify_one();
        }

        explicit PipelineIsolationProbe(const nri::CoreInterface& core) noexcept
            : core_(const_cast<nri::CoreInterface&>(core))
        {
            original_ = core_;
            core_.CreateGraphicsPipeline = [](
                nri::Device& device, const nri::GraphicsPipelineDesc& description, nri::Pipeline*& pipeline
            ) noexcept
            {
                ++pipeline_calls;
                auto armed = EGate::ARMED;
                if (gate.compare_exchange_strong(armed, EGate::HOLDING))
                {
                    gate.wait(EGate::HOLDING);
                }
                if (fail_pipeline.exchange(false)) return nri::Result::OUT_OF_MEMORY;
                const auto result = original_.CreateGraphicsPipeline(device, description, pipeline);
                if (result == nri::Result::SUCCESS) ++created_pipelines;
                return result;
            };
            core_.DestroyPipeline = [](nri::Pipeline* pipeline) noexcept
            {
                if (pipeline) ++destroyed_pipelines;
                original_.DestroyPipeline(pipeline);
            };
            core_.BeginCommandBuffer = [](nri::CommandBuffer& command, const nri::DescriptorPool* pool) noexcept
            {
                {
                    std::lock_guard lock(commands_mutex_);
                    raster_commands_.erase(&command);
                }
                return original_.BeginCommandBuffer(command, pool);
            };
            core_.CmdBeginRendering = [](nri::CommandBuffer& command, const nri::RenderingDesc& scope) noexcept
            {
                {
                    std::lock_guard lock(commands_mutex_);
                    raster_commands_.insert(&command);
                }
                original_.CmdBeginRendering(command, scope);
            };
            core_.QueueSubmit = [](nri::Queue& queue, const nri::QueueSubmitDesc& description) noexcept
            {
                bool has_raster = false;
                {
                    std::lock_guard lock(commands_mutex_);
                    for (const auto* command : std::span(description.commandBuffers, description.commandBufferNum))
                    {
                        has_raster |= raster_commands_.contains(command);
                    }
                }
                const auto result = original_.QueueSubmit(queue, description);
                if (result == nri::Result::SUCCESS)
                {
                    ++accepted_submits;
                    if (has_raster) ++raster_submits;
                }
                return result;
            };
        }

        ~PipelineIsolationProbe() noexcept
        {
            core_.CreateGraphicsPipeline = original_.CreateGraphicsPipeline;
            core_.DestroyPipeline = original_.DestroyPipeline;
            core_.BeginCommandBuffer = original_.BeginCommandBuffer;
            core_.CmdBeginRendering = original_.CmdBeginRendering;
            core_.QueueSubmit = original_.QueueSubmit;
            raster_commands_.clear();
        }

        static RenderResult<std::unique_ptr<PipelineIsolationProbe>>
        create(ResourceCreateContext& context, ResourceInstanceId) noexcept
        {
            return std::make_unique<PipelineIsolationProbe>(context.device.nriCore());
        }

        static ResourceRegistration factory() noexcept
        {
            return makeResourceRegistration<Descriptor, PipelineIsolationProbe, &create>(nullptr);
        }

    private:
        static inline nri::CoreInterface original_{};
        static inline std::mutex commands_mutex_;
        static inline std::set<const nri::CommandBuffer*> raster_commands_;
        nri::CoreInterface& core_;
    };

    using RasterProbeConfig = std::tuple<EMaterialPurpose, CaptureConfig>;

    RenderResult<EffectDefinition> rasterRecipe(
        const EffectOutputDesc& output, const RasterProbeConfig& configuration, std::span<const FeatureId> features
    ) noexcept
    {
        const auto& [purpose, capture] = configuration;
        const bool gbuffer = purpose == EMaterialPurpose::GBUFFER;
        ConfigValue raster = gbuffer ? take(ConfigValue::make(GBufferFeatureConfig{output, false}))
                                     : take(ConfigValue::make(MeshFeatureConfig{output, false}));
        MeshVisibilityFeatureConfig visibility;
        visibility.camera_view = true;
        EffectDefinition recipe;
        recipe.features.push_back({features[0], "raster", std::move(raster)});
        recipe.features.push_back({features[1], "capture", take(ConfigValue::make(capture))});
        recipe.features.push_back({features[2], "visibility", take(ConfigValue::make(visibility))});
        for (auto port : {"groups", "transforms", "properties", "alive", "visible", "commands"})
        {
            recipe.connections.push_back({{2, port}, {0, port}});
        }
        const std::string color = gbuffer ? std::string(kGBufferPorts[0]) : "color";
        const auto captured = purpose == EMaterialPurpose::SHADOW ? "depth" : color;
        recipe.connections.push_back({{0, captured}, {1, "color"}});
        recipe.outputs.push_back({{0, color}, GraphOutputRequirements{output.format, 1}});
        recipe.outputs.push_back({{1, "pixels"}, {}});
        return recipe;
    }

    rdesc::MaterialProgramSet cookProgram(
        lux::shader::ShaderCompiler& compiler, rdesc::EShaderTarget target,
        std::span<const EMaterialPurpose> purposes, unsigned parameter_count, float emissive, unsigned texture_count = 1
    )
    {
        using namespace lux::material;
        MaterialGraph graph;
        graph.shading_model = rdesc::ELightingTechnique::UNLIT;
        graph.render_state.double_sided = true;
        graph.render_state.alpha_mode = rdesc::EAlphaMode::MASK;
        graph.param_slots.reserve(parameter_count);
        for (unsigned index = 0; index < parameter_count; ++index)
        {
            graph.param_slots.push_back({"value" + std::to_string(index), EValueType::FLOAT, {1, 0, 0, 0}});
        }
        for (unsigned index = 0; index < texture_count; ++index)
        {
            const auto binding = static_cast<std::uint8_t>(index + 1);
            graph.texture_slots.push_back({
                "color" + std::to_string(index), lux::asset::AssetId{std::array<std::uint8_t, 16>{binding}}
            });
        }
        const auto surface = material_test::add(graph, MaterialOutputSurface{});
        const auto opacity = material_test::add(graph, MaterialParameter{0, EValueType::FLOAT});
        const auto emission = material_test::add(graph, MaterialConstant{{emissive, 0, 0, 0}, EValueType::VEC3});
        const auto uv = material_test::add(graph, MaterialInput{EMaterialInput::UV0});
        const auto texture = material_test::add(graph, MaterialSampleTexture{});
        CHECK(material_test::connect(graph, opacity, 0, surface, 1));
        CHECK(material_test::connect(graph, emission, 0, surface, 5));
        CHECK(material_test::connect(graph, uv, 0, texture, 0));
        CHECK(material_test::connect(graph, texture, 0, surface, 0));
        auto result = deriveMaterial(compiler, graph, {target, purposes});
        if (!result) std::fprintf(stderr, "Material variant %u: %s\n", parameter_count, result.error().diagnostic.c_str());
        CHECK(result);
        return std::move(result->material.programs);
    }

    rdesc::Mesh makeQuad()
    {
        rdesc::Mesh mesh;
        constexpr std::array<std::array<float, 2>, 4> points{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
        mesh.vertices.resize(points.size());
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            auto& vertex = mesh.vertices[i];
            vertex.position = {points[i][0], points[i][1], .5F};
            vertex.normal = {0, 0, 1};
            vertex.uv = {.5F, .5F};
            vertex.tangent = {1, 0, 0};
            vertex.bitangent = {0, 1, 0};
        }
        mesh.indices = {0, 1, 2, 0, 2, 3};
        mesh.bounds = lux::math::AABB{{-1, -1, .5F}, {1, 1, .5F}};
        return mesh;
    }

    TextureId uploadTexture(ipc::IpcRequester& ipc, const CommandList& commands, ResourceInstanceId textures, unsigned red)
    {
        rdesc::TextureInfo info{};
        info.width = info.height = info.mip_count = 1;
        info.channel = 4;
        info.pixel_format = rdesc::ETexturePixelFormat::RGBA8_UNORM;
        info.color_space = rdesc::ETextureColorSpace::DATA;
        info.mip_ranges[0] = {0, 4, 1, 1};
        const std::array rgba{std::byte(red), std::byte{96}, std::byte{160}, std::byte{255}};
        return call<TextureCommands::Upload>(ipc, commands, {textures, {info, rgba}}).texture;
    }

    using Probes = std::array<RenderProbe<RasterProbeConfig>, 3>;
    constexpr std::uint32_t kFif = 3;
    constexpr TargetDesc kTarget{64, 64, ETextureFormat::RGBA8_UNORM};

    struct RasterSample final
    {
        std::array<std::byte, 4> center;
        std::uint64_t buffer_ranges, buffer_bytes, image_ranges, image_subresources;
        std::uint64_t switched_image_ranges;
    };

    struct InstanceSwitch final
    {
        ResourceInstanceId resource;
        MeshInstanceUpdate update;
        unsigned expected_image_ranges;
    };

    RasterSample sampleRaster(
        RenderServer& server, ipc::IpcRequester& ipc, const CommandList& commands, DeviceSessionId session,
        const RenderProbe<RasterProbeConfig>& probe, EMaterialPurpose purpose, SceneId scene,
        const InstanceSwitch* change = nullptr
    )
    {
        CapturedPixels pixels;
        const auto format = purpose == EMaterialPurpose::SHADOW ? ETextureFormat::D32_SFLOAT : kTarget.format;
        auto request = probeRequest(kTarget, probe, RasterProbeConfig{purpose, CaptureConfig{&pixels, format}},
            {"color", "pixels"});
        const auto effect = call<CreateEffect>(ipc, commands, std::move(request)).effect;
        const auto view = call<CreateView>(ipc, commands, {scene}).view;
        const auto output = call<CreateTarget>(ipc, commands, {session, kTarget}).target;
        call<UpdateViewCamera>(ipc, commands, {{scene, view}, {}});
        const auto job = startRenderJob(ipc, commands, session, scene, view, output, effect);
        const auto before = server.developmentStats();
        CHECK(before);
        for (unsigned slot = 0; slot < kFif; ++slot) renderFrame(ipc, commands, job);
        const auto after = server.developmentStats();
        CHECK(after);
        std::uint64_t switched_image_ranges = 0;
        if (change)
        {
            call<InstanceCommands::Update>(ipc, commands, {change->resource, {}, {&change->update, 1}});
            for (unsigned slot = 0; slot < kFif; ++slot) renderFrame(ipc, commands, job);
            const auto switched = server.developmentStats();
            CHECK(switched);
            CHECK(switched->raster_buffer_ranges - after->raster_buffer_ranges == kFif);
            switched_image_ranges = switched->raster_image_ranges - after->raster_image_ranges;
            CHECK(switched_image_ranges == change->expected_image_ranges);
        }
        stopAndRemoveJob(ipc, commands, job);
        call<RemoveView>(ipc, commands, {scene, view});
        call<RemoveTarget>(ipc, commands, output);
        call<RemoveEffect>(ipc, commands, {effect});
        const auto deadline = Clock::now() + 15s;
        while (!pixels.ready.load(std::memory_order_acquire))
        {
            CHECK(Clock::now() < deadline);
            std::this_thread::yield();
        }
        CHECK(pixels.frames.size() == kFif);
        std::array<std::byte, 4> center{};
        const auto offset = 32 * pixels.pitch + 32 * 4;
        std::memcpy(center.data(), pixels.frames.front().data() + offset, center.size());
        for (const auto& frame : pixels.frames)
        {
            CHECK(std::memcmp(center.data(), frame.data() + offset, center.size()) == 0);
        }
        return {
            center, after->raster_buffer_ranges - before->raster_buffer_ranges,
            after->raster_buffer_bytes - before->raster_buffer_bytes,
            after->raster_image_ranges - before->raster_image_ranges,
            after->raster_image_subresources - before->raster_image_subresources, switched_image_ranges
        };
    }

    void verifyPurposes(
        RenderServer& server, ipc::IpcRequester& ipc, const CommandList& commands, DeviceSessionId session, const Probes& probes,
        SceneId scene, ResourceInstanceId materials, ResourceInstanceId instances, MeshId mesh,
        std::span<const TextureId, 2> textures, lux::shader::ShaderCompiler& compiler,
        rdesc::EShaderTarget target
    )
    {
        constexpr std::array purposes{EMaterialPurpose::FORWARD, EMaterialPurpose::GBUFFER, EMaterialPurpose::SHADOW};
        auto program = cookProgram(compiler, target, purposes, 1, .125F);
        CHECK(program.programs.size() == purposes.size());
        const auto registered = registerMaterialProgram(ipc, commands, materials, std::move(program));
        const auto material = createMaterialInstance(ipc, commands, registered, {{1, 0, 0, 0}}, {textures[0]});
        MeshInstanceData input{mesh, material};
        input.render_enabled = true;
        const auto instance = call<InstanceCommands::CreateInstance>(ipc, commands, {instances, input}).instance;
        const auto forward = sampleRaster(server, ipc, commands, session, probes[0], purposes[0], scene);
        const auto gbuffer = sampleRaster(server, ipc, commands, session, probes[1], purposes[1], scene);
        const auto shadow = sampleRaster(server, ipc, commands, session, probes[2], purposes[2], scene);
        CHECK(std::abs(std::to_integer<int>(forward.center[0]) - 96) <= 1);
        CHECK(std::abs(std::to_integer<int>(gbuffer.center[0]) - 64) <= 1);
        CHECK(std::to_integer<int>(forward.center[1]) == 96 && gbuffer.center[1] == forward.center[1]);
        CHECK(std::to_integer<int>(forward.center[3]) == 255 && std::to_integer<int>(gbuffer.center[3]) == 0);
        CHECK(std::abs(std::bit_cast<float>(shadow.center) - .5F) < 1e-5F);
        CHECK(forward.image_ranges == kFif && gbuffer.image_ranges == kFif);
        CHECK(shadow.image_ranges == 0);
        CHECK(forward.buffer_ranges == kFif && gbuffer.buffer_ranges == kFif && shadow.buffer_ranges == kFif);
        call<MaterialCommands::Update>(ipc, commands, {{material, {{.25F, 0, 0, 0}}, {textures[0]}}});
        const auto masked = sampleRaster(server, ipc, commands, session, probes[2], purposes[2], scene);
        CHECK(std::bit_cast<float>(masked.center) == 1);
        call<MaterialCommands::Update>(ipc, commands, {{material, {{1, 0, 0, 0}}, {textures[0]}}});
        const auto other = createMaterialInstance(ipc, commands, registered, {{.75F, 0, 0, 0}}, {textures[1]});
        input.material = other;
        input.transform[0] = input.transform[5] = .25F;
        input.transform[12] = .5F;
        const auto sibling = call<InstanceCommands::CreateInstance>(ipc, commands, {instances, input}).instance;
        input.material = material;
        const InstanceSwitch change{instances, {sibling, input}, kFif};
        const auto together = sampleRaster(server, ipc, commands, session, probes[0], purposes[0], scene, &change);
        CHECK(together.buffer_ranges == kFif && together.image_ranges == 2 * kFif);
        call<InstanceCommands::ReleaseInstance>(ipc, commands, {sibling});
        call<MaterialCommands::ReleaseInstance>(ipc, commands, {other});
        call<InstanceCommands::ReleaseInstance>(ipc, commands, {instance});
        call<MaterialCommands::ReleaseInstance>(ipc, commands, {material});
        call<MaterialCommands::UnregisterProgram>(ipc, commands, {registered});
        std::printf(
            "MATERIAL_PURPOSES forward_red=%u gbuffer_red=%u shadow_depth=0.5 masked_depth=1 shadow_image_ranges=%llu "
            "group_image_ranges=%llu switched_image_ranges=%llu\n",
            std::to_integer<unsigned>(forward.center[0]), std::to_integer<unsigned>(gbuffer.center[0]),
            static_cast<unsigned long long>(shadow.image_ranges), static_cast<unsigned long long>(together.image_ranges),
            static_cast<unsigned long long>(together.switched_image_ranges)
        );
    }

    std::uint64_t requestFrame(
        ipc::IpcRequester& client, const CommandList& commands, RenderJobId job, FrameData* packet = nullptr
    )
    {
        const auto command = std::ranges::find(commands.commands, RequestRenderJob::name, &CommandInfo::name);
        CHECK(command != commands.commands.end());
        const auto request_id = nextSceneRequest();
        ipc::IpcMessage message;
        message.emplace<RequestRenderJob::Request>(RequestRenderJob::Request{job, packet});
        CHECK(client.trySend(commandTag(command->id), request_id, message));
        return request_id;
    }

    std::optional<JobSubmitted>
    receiveFrameBefore(ipc::IpcRequester& client, std::uint64_t request_id, Clock::time_point deadline)
    {
        auto& notifications = ipc::IpcCore::instance();
        for (ipc::IpcMessage message; Clock::now() < deadline;)
        {
            const auto epoch = notifications.workEpoch();
            std::uint64_t correlation = 0;
            const auto status = client.tryReceive(correlation, message);
            CHECK(status != ipc::EIpcReceive::CLOSED);
            if (status == ipc::EIpcReceive::RECEIVED)
            {
                CHECK(correlation == request_id);
                return message.get<JobSubmitted>();
            }
            notifications.waitUntil(epoch, deadline);
        }
        return std::nullopt;
    }

    struct PipelineJob final
    {
        CapturedPixels pixels;
        MaterialId material;
        MaterialProgramId program;
        EffectId effect;
        RenderJobId job;
        ViewId view;
        TargetId target;
        JobBinding binding;
        ResourceInstanceId instances;
        MeshInstanceId member;
    };

    struct ProbeMaterial final
    {
        MaterialProgramId program;
        MaterialId instance;
    };

    JobSubmitted verifyPreparationLifecycle(
        RenderServer& server, ipc::IpcRequester& client, ipc::IpcRequester& requests, const CommandList& commands, DeviceSessionId session,
        const PipelineJob& job, TargetId input, MeshId mesh, std::span<const ProbeMaterial, 3> variants
    )
    {
        using Probe = PipelineIsolationProbe;
        const auto changeMaterial = [&](MaterialId material)
        {
            MeshInstanceData value{mesh, material};
            value.render_enabled = true;
            const MeshInstanceUpdate update{job.member, value};
            call<InstanceCommands::Update>(client, commands, {job.instances, {}, {&update, 1}});
        };
        const auto snapshot = [&]()
        {
            auto stats = server.developmentStats();
            CHECK(stats);
            return *stats;
        };
        const auto requireBorrowedTarget = [&](TargetId id, ETextureFormat format)
        {
            auto resized = kTarget;
            resized.sampled = true;
            resized.width *= 2;
            resized.format = format;
            CHECK(call<ResizeTarget>(client, commands, {id, resized}, false).error.type == kDeviceBusy);
            CHECK(call<RemoveTarget>(client, commands, id, false).error.type == kDeviceBusy);
        };
        const auto waitForDiscard = [&](std::uint64_t previous)
        {
            const auto deadline = Clock::now() + 15s;
            while (snapshot().discarded_preparations == previous)
            {
                CHECK(Clock::now() < deadline);
                std::this_thread::sleep_for(1ms);
            }
            CHECK(snapshot().discarded_preparations == previous + 1);
        };

        changeMaterial(variants[0].instance);
        const auto stop_before = snapshot();
        const auto stop_creates = Probe::created_pipelines.load();
        const auto stop_destroys = Probe::destroyed_pipelines.load();
        Probe::gate = Probe::EGate::ARMED;
        const auto interrupted = requestFrame(requests, commands, job.job);
        Probe::waitForDriver();
        CHECK(call<RebindRenderJobEffect>(client, commands, {session, job.job, job.effect}, false).error.type == kJobBusy);
        call<StopRenderJob>(client, commands, job.job);
        const auto stopped = receiveFrameBefore(requests, interrupted, Clock::now() + 10s);
        CHECK(stopped && stopped->error.type == kJobStopped);
        CHECK(call<StartRenderJob>(client, commands, {session, job.job, job.target, job.binding}, false).error.type == kJobBusy);
        CHECK(call<RebindRenderJobEffect>(client, commands, {session, job.job, job.effect}, false).error.type == kJobBusy);
        CHECK(call<RemoveRenderJob>(client, commands, job.job, false).error.type == kJobBusy);
        requireBorrowedTarget(job.target, ETextureFormat::RGBA8_UNORM);
        requireBorrowedTarget(input, ETextureFormat::RGBA16_SFLOAT);
        changeMaterial(job.material);
        call<MaterialCommands::ReleaseInstance>(client, commands, {variants[0].instance});
        const auto busy = call<MaterialCommands::UnregisterProgram>(client, commands, {variants[0].program}, false);
        CHECK(busy.error.type == kDeviceBusy); // No live instance remains, only the frozen PSO borrow.
        Probe::releaseDriver();
        waitForDiscard(stop_before.discarded_preparations);
        call<MaterialCommands::UnregisterProgram>(client, commands, {variants[0].program});
        CHECK(Probe::created_pipelines.load() == stop_creates + 1);
        CHECK(Probe::destroyed_pipelines.load() == stop_destroys + 1);
        call<RebindRenderJobEffect>(client, commands, {{}, job.job, job.effect});
        call<StartRenderJob>(client, commands, {session, job.job, job.target, job.binding});
        renderFrame(client, commands, job.job);

        changeMaterial(variants[1].instance);
        const auto failure_before = snapshot();
        const auto calls_before = Probe::pipeline_calls.load();
        Probe::fail_pipeline = true;
        const auto failed = call<RequestRenderJob>(client, commands, {job.job}, false);
        CHECK(failed.error.type == kDeviceFailure);
        const auto repeated = call<RequestRenderJob>(client, commands, {job.job}, false);
        CHECK(repeated.error == failed.error);
        CHECK(Probe::pipeline_calls.load() == calls_before + 1);
        CHECK(snapshot().program_preparations == failure_before.program_preparations + 1);
        changeMaterial(job.material);
        renderFrame(client, commands, job.job);

        const auto layout = call<QueryRenderJobInputLayout>(client, commands, job.job).layout;
        auto packet = take(FrameData::create<std::tuple<InstanceResource, LightResource>>(layout));
        MeshInstanceData value{mesh, variants[1].instance};
        value.render_enabled = true;
        CHECK(packet.addData<InstanceResource>(MeshInstanceUpdate{job.member, value}));
        CHECK(packet.seal());
        const auto stale_before = snapshot();
        const auto stale_destroys = Probe::destroyed_pipelines.load();
        Probe::gate = Probe::EGate::ARMED;
        const auto pending = requestFrame(requests, commands, job.job, &packet);
        Probe::waitForDriver();
        for (unsigned update = 0; update < 2048; ++update) changeMaterial(variants[1 + (update + 1) % 2].instance);
        changeMaterial(variants[2].instance);
        CHECK(packet.sealed());
        CHECK(!receiveFrameBefore(requests, pending, Clock::now() + 20ms));
        Probe::releaseDriver();
        const auto final = receiveFrameBefore(requests, pending, Clock::now() + 15s);
        CHECK(final && !final->error.type && final->job == job.job);
        packet.release();
        CHECK(packet.writable());
        CHECK(!receiveFrameBefore(requests, pending, Clock::now() + 20ms));
        const auto after = snapshot();
        CHECK(after.program_preparations == stale_before.program_preparations + 2);
        CHECK(after.discarded_preparations == stale_before.discarded_preparations + 1);
        CHECK(Probe::destroyed_pipelines.load() == stale_destroys + 1);
        std::puts("PIPELINE_LIFETIME stale_revisions=2048 discarded=1 stopped=1 failure_attempts=1 terminal_replies=1");
        std::puts("MATERIAL_PROGRAM_BORROW unregister_while_borrowed=busy released_after_return=1");
        return *final;
    }

    void verifyPipelineIsolation(
        RenderServer& server, std::thread& server_thread, ipc::IpcRequester& client, const CommandList& commands, DeviceSessionId session,
        std::string_view channel,
        const RenderProbe<RasterProbeConfig>& forward, std::array<SceneId, 2> scenes, ResourceInstanceId materials,
        MeshId mesh, TextureId texture, lux::shader::ShaderCompiler& compiler, rdesc::EShaderTarget target,
        FeatureId clear, bool expect_progress, unsigned workers, bool stop_server
    )
    {
        CapturedPixels imported_pixels;
        std::array<PipelineJob, 2> jobs;
        constexpr std::array purposes{EMaterialPurpose::FORWARD};
        const auto createMaterial = [&](unsigned shape, float emission)
        {
            auto program = cookProgram(compiler, target, purposes, shape, emission);
            auto parameters = program.parameter_defaults;
            const auto id = registerMaterialProgram(client, commands, materials, std::move(program));
            return ProbeMaterial{id, createMaterialInstance(client, commands, id, std::move(parameters), {texture})};
        };
        TargetId source;
        if (expect_progress)
        {
            constexpr auto hdr = ETextureFormat::RGBA16_SFLOAT;
            source = call<CreateTarget>(client, commands, {session, {64, 64, hdr, 1, ETargetKind::OFFSCREEN, true}}).target;
            CreateEffectRequest produce{{hdr, 1, ETargetKind::OFFSCREEN, true}};
            produce.definition.features.push_back({clear, "hdr", take(ConfigValue::make(WindowClear::Config{hdr}))});
            produce.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{hdr, 1}});
            const auto effect = call<CreateEffect>(client, commands, std::move(produce)).effect;
            const auto view = call<CreateView>(client, commands, {scenes[0]}).view;
            const auto writer = startRenderJob(client, commands, session, scenes[0], view, source, effect);
            renderFrame(client, commands, writer);
            stopAndRemoveJob(client, commands, writer);
            call<RemoveView>(client, commands, {scenes[0], view});
            call<RemoveEffect>(client, commands, {effect});
        }
        for (unsigned i = 0; i < jobs.size(); ++i)
        {
            auto& job = jobs[i];
            const auto created = createMaterial(i + 1, float(i + 1) / 8);
            job.program = created.program;
            job.material = created.instance;
            job.instances = call<GetOrCreateResource>(client, commands, {session, scenes[i], kInstanceResourceName}).resource_id;
            MeshInstanceData input{mesh, job.material};
            input.render_enabled = true;
            job.member = call<InstanceCommands::CreateInstance>(client, commands, {job.instances, input}).instance;
            auto request = probeRequest(
                kTarget, forward, RasterProbeConfig{purposes[0], CaptureConfig{&job.pixels, kTarget.format}},
                {"color", "pixels"}
            );
            if (i == 1 && expect_progress)
            {
                const auto tone = static_cast<std::uint32_t>(request.definition.features.size());
                const auto capture = tone + 1;
                const FeaturePortRef destination{tone, "hdr_color"};
                request.definition.features.push_back({
                    forward.features[3], "probe/tone",
                    take(ConfigValue::make(TonemapFeatureConfig{{kTarget.format}, EColorEncoding::LINEAR}))
                });
                request.definition.features.push_back({
                    forward.features[1], "probe/import",
                    take(ConfigValue::make(CaptureConfig{&imported_pixels, kTarget.format}))
                });
                GraphImageDesc image{64, 64, 1, 1, ETextureFormat::RGBA16_SFLOAT};
                image.extent.source = EImageExtentSource::TARGET;
                request.definition.inputs.push_back({
                    destination, {"pipeline_gate.import", image, EGraphOrigin::IMPORTED, {GraphImageRange{}},
                    EGraphUse::SAMPLED}, "lux.color.linear-hdr.v1"
                });
                request.definition.connections.push_back({{tone, "color"}, {capture, "color"}});
                request.definition.outputs.push_back({{capture, "pixels"}, {}});
                job.binding.target_images.push_back({destination, source});
            }
            job.effect = call<CreateEffect>(client, commands, std::move(request)).effect;
            job.view = call<CreateView>(client, commands, {scenes[i]}).view;
            job.target = call<CreateTarget>(client, commands, {session, kTarget}).target;
            call<UpdateViewCamera>(client, commands, {{scenes[i], job.view}, {}});
            job.job = call<CreateRenderJob>(client, commands, {
                scenes[i], job.view, job.effect, EJobCadence::REQUESTED
            }).job;
            call<StartRenderJob>(client, commands, {session, job.job, job.target, job.binding});
        }

        const auto warm = jobs[0].job;
        const auto cold = jobs[1].job;
        for (unsigned slot = 0; slot < kFif; ++slot) renderFrame(client, commands, warm);
        auto cold_client = ipc::IpcCore::instance().createRequester(channel);
        CHECK(cold_client);
        const auto before = server.developmentStats();
        CHECK(before);
        using Probe = PipelineIsolationProbe;
        std::optional<FrameData> stop_packet;
        if (stop_server)
        {
            const auto layout = call<QueryRenderJobInputLayout>(client, commands, cold).layout;
            stop_packet.emplace(take(FrameData::create<std::tuple<InstanceResource, LightResource>>(layout)));
            MeshInstanceData value{mesh, jobs[1].material};
            value.render_enabled = true;
            CHECK(stop_packet->addData<InstanceResource>(MeshInstanceUpdate{jobs[1].member, value}));
            CHECK(stop_packet->seal());
        }
        Probe::gate = Probe::EGate::ARMED;
        const auto cold_request = requestFrame(*cold_client, commands, cold, stop_packet ? &*stop_packet : nullptr);
        Probe::waitForDriver();
        if (expect_progress)
        {
            const auto held = server.developmentStats();
            CHECK(held && held->rounds_opened == before->rounds_opened);
            CHECK(held->program_preparations == before->program_preparations + 1);
        }
        if (stop_server)
        {
            server.stop();
            const auto stopped = receiveFrameBefore(*cold_client, cold_request, Clock::now() + 10s);
            CHECK(stopped && stopped->error.type == kJobStopped);
            stop_packet->release();
            CHECK(stop_packet->writable());
            const auto created = Probe::created_pipelines.load();
            const auto destroyed = Probe::destroyed_pipelines.load();
            Probe::releaseDriver();
            server_thread.join();
            const auto joined = server.developmentStats();
            CHECK(joined && joined->discarded_preparations == before->discarded_preparations + 1);
            CHECK(Probe::created_pipelines.load() == created + 1);
            CHECK(Probe::destroyed_pipelines.load() >= destroyed + 1);
            std::puts("PIPELINE_SHUTDOWN cancelled_request=1 packet_returned=1 candidate_destroyed=1 joined=1");
            return;
        }
        const auto submits_before = Probe::accepted_submits.load();
        const auto raster_submits_before = Probe::raster_submits.load();
        std::optional<FrameData> warm_packet;
        if (expect_progress)
        {
            const auto layout = call<QueryRenderJobInputLayout>(client, commands, warm).layout;
            warm_packet.emplace(take(FrameData::create<std::tuple<InstanceResource, LightResource>>(layout)));
            MeshInstanceData moved{mesh, jobs[0].material};
            moved.render_enabled = true;
            moved.transform[12] = .125F;
            CHECK(warm_packet->addData<InstanceResource>(MeshInstanceUpdate{jobs[0].member, moved}));
            CHECK(warm_packet->seal());
        }
        const auto warm_request = requestFrame(client, commands, warm, warm_packet ? &*warm_packet : nullptr);
        const auto early = receiveFrameBefore(client, warm_request, Clock::now() + 2s);
        if (early && expect_progress)
        {
            for (unsigned i = 1; i < kFif; ++i)
            {
                const auto next = requestFrame(client, commands, warm);
                const auto reply = receiveFrameBefore(client, next, Clock::now() + 2s);
                CHECK(reply && !reply->error.type && reply->job == warm);
                CHECK(reply->frame_index == early->frame_index + i);
                CHECK(reply->slot_index == (early->slot_index + i) % kFif);
            }
        }
        const auto accepted = Probe::accepted_submits.load() - submits_before;
        const auto raster_accepted = Probe::raster_submits.load() - raster_submits_before;
        const auto blocked = server.developmentStats();
        Probe::releaseDriver();

        const auto warm_reply = early ? early : receiveFrameBefore(client, warm_request, Clock::now() + 15s);
        const auto cold_reply = receiveFrameBefore(*cold_client, cold_request, Clock::now() + 15s);
        CHECK(warm_reply && !warm_reply->error.type && warm_reply->job == warm);
        if (warm_packet) warm_packet->release();
        CHECK(cold_reply && !cold_reply->error.type && cold_reply->job == cold);
        CHECK(blocked);
        const auto holding_slot = blocked->program_preparations_holding_slot - before->program_preparations_holding_slot;
        std::printf(
            "PIPELINE_ISOLATION workers=%u warm_reply_while_cold_blocked=%u native_submits=%llu "
            "raster_submits=%llu preparations_holding_slot=%llu\n",
            workers, unsigned(early.has_value()), static_cast<unsigned long long>(accepted),
            static_cast<unsigned long long>(raster_accepted),
            static_cast<unsigned long long>(holding_slot)
        );
        std::fflush(stdout);
        const bool should_submit = expect_progress && workers > 1;
        CHECK(early.has_value() == should_submit);
        CHECK((raster_accepted > 0) == should_submit);
        // One dirty transform can reach the native queue even while the only CPU worker is blocked.
        const bool should_upload = expect_progress && workers == 1;
        if (should_upload) CHECK(accepted == 1);
        else CHECK((accepted > 0) == should_submit);
        CHECK((holding_slot == 0) == expect_progress);

        std::optional<JobSubmitted> newest;
        std::array<ProbeMaterial, 3> variants;
        if (expect_progress)
        {
            variants = {createMaterial(3, .375F), createMaterial(4, .75F), createMaterial(5, .5F)};
            newest = verifyPreparationLifecycle(
                server, client, *cold_client, commands, session, jobs[1], source, mesh,
                variants
            );
        }
        for (unsigned i : {1u, 0u})
        {
            const auto& job = jobs[i];
            stopAndRemoveJob(client, commands, job.job);
            call<RemoveView>(client, commands, {scenes[i], job.view});
            call<RemoveTarget>(client, commands, job.target);
            call<RemoveEffect>(client, commands, {job.effect});
            const auto deadline = Clock::now() + 15s;
            while (!job.pixels.ready.load(std::memory_order_acquire))
            {
                CHECK(Clock::now() < deadline);
                std::this_thread::yield();
            }
            call<RemoveScene>(client, commands, {scenes[i]});
            call<MaterialCommands::ReleaseInstance>(client, commands, {job.material});
            call<MaterialCommands::UnregisterProgram>(client, commands, {job.program});
        }
        if (newest)
        {
            CHECK(imported_pixels.ready.load(std::memory_order_acquire));
            call<RemoveTarget>(client, commands, source);
            const auto& frame = jobs[1].pixels.frames[newest->slot_index];
            const auto offset = 32 * jobs[1].pixels.pitch + 32 * 4;
            CHECK(std::abs(std::to_integer<int>(frame[offset]) - 192) <= 1);
            const auto& historical = imported_pixels.frames[newest->slot_index];
            const auto history_offset = 32 * imported_pixels.pitch + 32 * 4;
            const bool sampled_publication = historical[history_offset] > std::byte{0} &&
                historical[history_offset + 1] > historical[history_offset] &&
                historical[history_offset + 2] > historical[history_offset + 1] &&
                historical[history_offset + 3] == std::byte{255};
            CHECK(sampled_publication);
            for (const auto& variant : std::span(variants).subspan(1))
            {
                call<MaterialCommands::ReleaseInstance>(client, commands, {variant.instance});
                call<MaterialCommands::UnregisterProgram>(client, commands, {variant.program});
            }
            std::printf("PIPELINE_LATEST frame=%llu red=%u sampled_history=1\n",
                static_cast<unsigned long long>(newest->frame_index), std::to_integer<unsigned>(frame[offset]));
        }
    }

    unsigned pagesFor(unsigned records)
    {
        unsigned pages = 1;
        for (unsigned capacity = 16; records > capacity; capacity *= 2)
        {
            records -= capacity;
            ++pages;
        }
        return pages;
    }

    void verifyScale(
        RenderServer& server, ipc::IpcRequester& ipc, const CommandList& commands, DeviceSessionId session,
        const RenderProbe<RasterProbeConfig>& forward, SceneId scene, ResourceInstanceId materials,
        ResourceInstanceId instances, MeshId mesh, std::span<const TextureId> textures,
        lux::shader::ShaderCompiler& compiler, rdesc::EShaderTarget target, unsigned count, bool varied
    )
    {
        constexpr std::array purposes{EMaterialPurpose::FORWARD};
        const auto abi_count = varied ? std::min(count, 128u) : 1u;
        struct Program final
        {
            MaterialProgramId id;
            MaterialParameters defaults;
            std::uint32_t texture_count;
            std::uint32_t stride;
        };
        std::vector<Program> programs;
        programs.reserve(count);
        std::set<std::size_t> binaries;
        std::set<std::pair<std::size_t, std::uint32_t>> projections;
        std::uint64_t record_bytes = 0;
        for (unsigned index = 0; index < count; ++index)
        {
            const auto shape = index % abi_count;
            // DXIL limits structured records to 2048 bytes. The last shape uses one
            // more texture and shares a stride with an existing, different ABI.
            const bool extra_texture = shape == 127;
            const auto parameters = extra_texture ? 1 : shape + 1;
            const auto texture_count = extra_texture ? 2u : 1u;
            auto program = cookProgram(compiler, target, purposes, parameters, float(index + 1) / 4096, texture_count);
            CHECK(binaries.emplace(program.programs.front().shader.binaryDigest()).second);
            CHECK(program.parameter_defaults.size() == parameters && program.texture_count == texture_count);
            projections.emplace(program.parameter_defaults.size(), program.texture_count);
            record_bytes += program.parameters.size;
            const auto stride = program.parameters.size;
            auto defaults = program.parameter_defaults;
            const auto id = registerMaterialProgram(ipc, commands, materials, std::move(program));
            programs.push_back({id, std::move(defaults), texture_count, stride});
        }
        CHECK(projections.size() == abi_count && textures.size() == 9);
        const auto pages = abi_count * pagesFor(count / abi_count);
        std::vector<MaterialId> ids;
        ids.reserve(count);
        std::uint64_t resident_buffer_bytes = 0;
        for (unsigned round = 0; round < 5; ++round)
        {
            const auto before = MaterialAllocationProbe::sample();
            MaterialAllocationProbe::measuring = true;
            const auto start = Clock::now();
            const auto cpu_before = processCpuMicroseconds();
            for (std::size_t i = 0; i < programs.size(); ++i)
            {
                const auto& program = programs[i];
                std::vector<TextureId> bindings(program.texture_count);
                bindings.front() = textures[std::min<std::size_t>(i, textures.size() - 1)];
                ids.push_back(createMaterialInstance(ipc, commands, program.id, program.defaults, std::move(bindings)));
            }
            const auto cpu_us = processCpuMicroseconds() - cpu_before;
            const auto wall_us = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
            MaterialAllocationProbe::measuring = false;
            const auto after = MaterialAllocationProbe::sample();
            const auto allocated = after.buffers - before.buffers;
            const auto views = after.views - before.views;
            const auto buffer_bytes = after.buffer_bytes - before.buffer_bytes;
            const auto memory_bytes = after.memory_bytes - before.memory_bytes;
            const auto uploaded = after.upload_bytes - before.upload_bytes;
            CHECK(allocated == pages * kFif && views == allocated);
            CHECK(memory_bytes >= buffer_bytes && uploaded == record_bytes);
            resident_buffer_bytes = buffer_bytes;
            std::printf(
                "MATERIAL_ALLOC programs=%u abis=%u round=%u pages=%u fif_buffers=%llu descriptors=%llu "
                "buffer_bytes=%llu native_memory_requirement_bytes=%llu upload_bytes=%llu utilization=%.6f "
                "create_wall_us=%.3f create_cpu_us=%.3f\n",
                count, abi_count, round, pages, static_cast<unsigned long long>(allocated),
                static_cast<unsigned long long>(views), static_cast<unsigned long long>(buffer_bytes),
                static_cast<unsigned long long>(memory_bytes), static_cast<unsigned long long>(uploaded),
                double(record_bytes * kFif) / double(buffer_bytes), wall_us, cpu_us
            );
            std::fflush(stdout);
            if (round == 4) break;
            for (const auto id : ids) call<MaterialCommands::ReleaseInstance>(ipc, commands, {id});
            ids.clear();
        }

        std::vector<MeshInstanceData> input;
        input.reserve(count);
        for (unsigned index = 0; index < count; ++index)
        {
            MeshInstanceData value{mesh, ids[index]};
            value.render_enabled = true;
            if (index == 0)
            {
                // The readback point stays well inside this quad on either backend.
                value.transform[0] = value.transform[5] = .125F;
            }
            else
            {
                const auto pixel = index - 1;
                value.transform[0] = value.transform[5] = 1.0F / 64;
                value.transform[12] = (2 * float(pixel % 64) + 1) / 64 - 1;
                value.transform[13] = 1 - (2 * float(pixel / 64) + 1) / 64;
            }
            input.push_back(value);
        }
        constexpr unsigned sparse = 8;
        std::size_t previous = 0;
        for (const auto active : {sparse, count})
        {
            const auto created = call<InstanceCommands::CreateInstances>(
                ipc, commands, {instances, std::span<const MeshInstanceData>(input).subspan(previous, active - previous)}
            );
            CHECK(created.instances.size() == active - previous);
            previous = active;
            const auto reads = sampleRaster(server, ipc, commands, session, forward, purposes[0], scene);
            const auto expected_pages = active == count ? pages : (varied ? sparse : 1);
            const auto expected_images = std::min<unsigned>(active, 9);
            CHECK(reads.buffer_ranges == expected_pages * kFif);
            if (active == count)
            {
                CHECK(reads.buffer_bytes == resident_buffer_bytes);
            }
            else
            {
                std::uint64_t expected_bytes = programs.front().stride * 16ull;
                if (varied)
                {
                    expected_bytes = 0;
                    for (unsigned index = 0; index < active; ++index)
                    {
                        expected_bytes += programs[index].stride * 16ull;
                    }
                }
                CHECK(reads.buffer_bytes == expected_bytes * kFif);
            }
            CHECK(reads.image_ranges == expected_images * kFif && reads.image_ranges == reads.image_subresources);
            CHECK(reads.center[1] == std::byte{96} && reads.center[3] == std::byte{255});
            std::printf(
                "MATERIAL_READS programs=%u abis=%u active=%u accepted_frames=%u "
                "buffer_ranges=%llu buffer_bytes=%llu image_ranges=%llu image_subresources=%llu\n",
                count, abi_count, active, kFif, static_cast<unsigned long long>(reads.buffer_ranges),
                static_cast<unsigned long long>(reads.buffer_bytes), static_cast<unsigned long long>(reads.image_ranges),
                static_cast<unsigned long long>(reads.image_subresources)
            );
            std::fflush(stdout);
        }
        call<RemoveScene>(ipc, commands, {scene});
        for (const auto id : ids) call<MaterialCommands::ReleaseInstance>(ipc, commands, {id});
        for (const auto& program : programs) call<MaterialCommands::UnregisterProgram>(ipc, commands, {program.id});
    }

    void profileInstances(
        RenderServer& server, ipc::IpcRequester& ipc, const CommandList& commands, DeviceSessionId session,
        const RenderProbe<RasterProbeConfig>& forward, SceneId scene, ResourceInstanceId materials,
        ResourceInstanceId instances, MeshId mesh, TextureId texture, lux::shader::ShaderCompiler& compiler,
        rdesc::EShaderTarget target
    )
    {
        using Register = MaterialCommands::RegisterProgram;
        using Create = MaterialCommands::CreateInstance;
        static_assert(ipc::IpcMessage::stores_inline<Create::Request>);
        constexpr unsigned count = 10'000;
        constexpr std::array purposes{EMaterialPurpose::FORWARD};
        auto program = cookProgram(compiler, target, purposes, 1, .125F);
        const auto stride = program.parameters.size;
        const auto values = program.parameter_defaults;
        const auto registration_bytes = sizeof(Register::Request) - sizeof(program) +
            take(ConfigValueTraits<rdesc::MaterialProgramSet>::retainedBytes(program));
        const auto logical_instance_bytes = sizeof(Create::Request) + values.size() * sizeof(values.front()) +
            sizeof(TextureId);
        std::size_t shader_bytes = 0;
        for (const auto& purpose : program.programs)
            for (const auto& stage : purpose.shader.stages)
                shader_bytes += stage.words.size() * sizeof(stage.words.front());
        const auto registration_start = Clock::now();
        const auto id = registerMaterialProgram(ipc, commands, materials, std::move(program));
        const auto registration_us = std::chrono::duration<double, std::micro>(Clock::now() - registration_start);

        std::vector<MaterialId> ids;
        ids.reserve(count);
        std::vector<double> samples;
        samples.reserve(count);
        const auto before = MaterialAllocationProbe::sample();
        MaterialAllocationProbe::measuring = true;
        const auto start = Clock::now();
        const auto cpu_before = processCpuMicroseconds();
#ifdef _WIN32
        ipc_allocation::start();
#endif
        for (unsigned i = 0; i < count; ++i)
        {
            const auto request_start = Clock::now();
            ids.push_back(createMaterialInstance(ipc, commands, id, values, {texture}));
            samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - request_start).count());
        }
#ifdef _WIN32
        const auto client_allocations = ipc_allocation::stop();
        CHECK(client_allocations == count * 2);
        std::printf("MATERIAL_INSTANCE_CLIENT allocations=%zu count=%u\n", client_allocations, count);
#endif
        const auto cpu_us = processCpuMicroseconds() - cpu_before;
        const auto wall_us = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
        MaterialAllocationProbe::measuring = false;
        const auto after = MaterialAllocationProbe::sample();
        const auto buffers = after.buffers - before.buffers;
        const auto descriptors = after.views - before.views;
        const auto allocation = after.memory_bytes - before.memory_bytes;
        const auto uploaded = after.upload_bytes - before.upload_bytes;
        CHECK(buffers == pagesFor(count) * kFif);
        CHECK(buffers == descriptors && uploaded == count * stride);
        CHECK(call<MaterialCommands::UnregisterProgram>(ipc, commands, {id}, false).error.type == kDeviceBusy);
        std::ranges::sort(ids, std::less<>{});
        CHECK(std::ranges::adjacent_find(ids) == ids.end());
        std::ranges::sort(samples);
        std::printf(
            "MATERIAL_INSTANCE_PROFILE programs=1 instances=%u register_retained_bytes=%zu register_shader_bytes=%zu "
            "request_inline_bytes=%zu request_logical_bytes=%zu request_shader_bytes=0 "
            "register_us=%.3f create_wall_us=%.3f create_cpu_us=%.3f create_p50_us=%.3f create_p95_us=%.3f "
            "pages=%u buffers=%llu descriptors=%llu native_memory_requirement_bytes=%llu upload_bytes=%llu\n",
            count, registration_bytes, shader_bytes, sizeof(Create::Request), logical_instance_bytes,
            registration_us.count(), wall_us, cpu_us, samples[count / 2 - 1], samples[count * 95 / 100 - 1],
            pagesFor(count), static_cast<unsigned long long>(buffers), static_cast<unsigned long long>(descriptors),
            static_cast<unsigned long long>(allocation), static_cast<unsigned long long>(uploaded)
        );
        std::fflush(stdout);

        std::array live{MeshInstanceData{mesh, ids.front()}, MeshInstanceData{mesh, ids.back()}};
        for (auto& instance : live) instance.render_enabled = true;
        call<InstanceCommands::CreateInstances>(ipc, commands, {instances, live});
        const auto sampled = sampleRaster(server, ipc, commands, session, forward, purposes[0], scene);
        CHECK(sampled.buffer_ranges == 2 * kFif);
        CHECK(sampled.image_ranges == kFif);
        CHECK(std::abs(std::to_integer<int>(sampled.center[0]) - 96) <= 1);
        call<RemoveScene>(ipc, commands, {scene});
        for (const auto instance : ids) call<MaterialCommands::ReleaseInstance>(ipc, commands, {instance});
        call<MaterialCommands::UnregisterProgram>(ipc, commands, {id});
        std::puts("MATERIAL_INSTANCE_READBACK first_and_last_pages=1 releases=10000 program_unregistered=1");
    }
} // namespace

int main(int argc, char** argv)
{
    DeviceOptions device;
    device.multiple_queues = true;
    device.texture_descriptors = true;
    device.cached_unused_programs = 4096;
    unsigned count = 0;
    bool varied = false;
    bool instance_profile = false;
    std::optional<bool> pipeline_isolation;
    unsigned workers = 2;
    bool stop_server = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "d3d12") device.backend = EGraphicsBackend::D3D12;
        else if (arg == "--varied") varied = true;
        else if (arg == "--instance-profile") instance_profile = true;
        else if (arg == "--pipeline-isolation") pipeline_isolation = true;
        else if (arg == "--expect-pipeline-blocking") pipeline_isolation = false;
        else if (arg == "--single-worker") workers = 1;
        else if (arg == "--pipeline-shutdown") { pipeline_isolation = true; stop_server = true; }
        else
        {
            count = static_cast<unsigned>(std::atoi(argv[i]));
            CHECK(count == 32 || count == 128 || count == 1024);
        }
    }
    const auto shader_target = device.backend == EGraphicsBackend::D3D12
        ? rdesc::EShaderTarget::DXIL : rdesc::EShaderTarget::SPIRV;
    auto compiler = lux::shader::ShaderCompiler::create();
    CHECK(compiler);
    auto runtime = lux::execution::ExecutionRuntime::create({workers, 128, {128}});
    CHECK(runtime);
    RenderServerConfig config{"test.material.scale", nativeValidationEnabled()};
    config.frames_in_flight = kFif;
    config.uploads.emplace();
    config.uploads->chunk_bytes = 4 * 1024 * 1024;
    config.uploads->window_regions = 8192;
    auto server = take(RenderServer::create(config, **runtime));
    take(server->registerResource<MaterialAllocationProbe>());
    if (pipeline_isolation.has_value()) take(server->registerResource<PipelineIsolationProbe>());
    const auto clear = pipeline_isolation.value_or(false) ? take(server->registerFeature<WindowClear>()) : FeatureId{};
    take(server->registerResource<CaptureBuffer>());
    const auto visibility = take(server->featureId("lux.render.feature.MeshVisibilityFeature"));
    const auto capture = take(server->registerFeature<CaptureFeature>());
    const auto probe = [&](std::string_view feature)
    {
        std::vector features{take(server->featureId(feature)), capture, visibility};
        if (pipeline_isolation.value_or(false))
            features.push_back(take(server->featureId("lux.render.feature.TonemapFeature")));
        return registerRenderProbe<RasterProbeConfig>(
            *server, std::string(feature), std::move(features),
            {"color", "pixels"}, rasterRecipe
        );
    };
    const Probes probes{
        probe("lux.render.feature.MaterialMeshRaster"), probe("lux.render.feature.GBufferFeature"),
        probe("lux.render.feature.MeshDepthRaster")
    };
    auto ipc = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(ipc);
    std::thread thread([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*ipc);
    const auto session = call<CreateDeviceSession>(*ipc, commands, device).device;
    call<GetOrCreateResource>(*ipc, commands, {session, DeviceScope{}, MaterialAllocationProbe::Descriptor::descriptor.type});
    if (pipeline_isolation.has_value())
    {
        CHECK(!count && !varied);
        call<GetOrCreateResource>(
            *ipc, commands, {session, DeviceScope{}, PipelineIsolationProbe::Descriptor::descriptor.type}
        );
    }
    const auto scene = call<CreateScene>(*ipc, commands, {}).scene;
    const auto instances = call<GetOrCreateResource>(*ipc, commands, {session, scene, kInstanceResourceName}).resource_id;
    const auto meshes = call<GetOrCreateResource>(*ipc, commands, {session, DeviceScope{}, kMeshResourceName}).resource_id;
    const auto textures = call<GetOrCreateResource>(*ipc, commands, {session, DeviceScope{}, kTextureResourceName}).resource_id;
    const auto materials =
        call<GetOrCreateResource>(*ipc, commands, {session, DeviceScope{}, kMaterialResourceName}).resource_id;
    const auto quad = makeQuad();
    const auto mesh = call<MeshCommands::UploadMesh>(*ipc, commands, {meshes, &quad}).mesh;
    std::vector<TextureId> images;
    for (unsigned index = 0; index < (count ? 9u : 2u); ++index)
    {
        images.push_back(uploadTexture(*ipc, commands, textures, 64 + index));
    }
    if (pipeline_isolation.has_value())
    {
        const auto other = call<CreateScene>(*ipc, commands, {}).scene;
        verifyPipelineIsolation(
            *server, thread, *ipc, commands, session, config.channel_name, probes[0], {scene, other}, materials, mesh,
            images.front(), **compiler, shader_target, clear, *pipeline_isolation, workers, stop_server
        );
    }
    else if (instance_profile)
    {
        CHECK(!count && !varied);
        profileInstances(*server, *ipc, commands, session, probes[0], scene, materials, instances, mesh, images.front(),
            **compiler, shader_target);
    }
    else if (count)
    {
        verifyScale(*server, *ipc, commands, session, probes[0], scene, materials, instances, mesh, images, **compiler,
            shader_target, count, varied);
    }
    else
    {
        verifyPurposes(*server, *ipc, commands, session, probes, scene, materials, instances, mesh,
            std::span<const TextureId, 2>(images.data(), 2), **compiler, shader_target);
        call<RemoveScene>(*ipc, commands, {scene});
    }
    if (thread.joinable())
    {
        call<MeshCommands::ReleaseMesh>(*ipc, commands, {mesh});
        for (const auto image : images) call<TextureCommands::Release>(*ipc, commands, {image});
        server->stop();
        thread.join();
    }
    CHECK(server->stats().validation_errors == 0);
}
