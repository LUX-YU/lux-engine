#include "MaterialFixture.hpp"
#include "SceneCapture.hpp"

#include <bit>
#include <ctime>
#include <set>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/render/features/mesh/GBufferFeature.hpp>
#include <lux/engine/render/features/mesh/MeshVisibilityFeature.hpp>
#include <lux/engine/render/features/mesh/RasterMeshParameters.hpp>
#ifdef _WIN32
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
        std::span<const EMaterialPurpose> purposes, unsigned parameter_count, float emissive
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
        graph.texture_slots.push_back({"color", lux::asset::AssetId{std::array<std::uint8_t, 16>{1}}});
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
        RenderServer& server, ipc::IpcRequester& ipc, const CommandList& commands,
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
        const auto output = call<CreateTarget>(ipc, commands, {kTarget}).target;
        call<UpdateViewCamera>(ipc, commands, {{scene, view}, {}});
        const auto job = startRenderJob(ipc, commands, scene, view, output, effect);
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
        RenderServer& server, ipc::IpcRequester& ipc, const CommandList& commands, const Probes& probes,
        SceneId scene, ResourceInstanceId materials, ResourceInstanceId instances, MeshId mesh,
        std::span<const TextureId, 2> textures, lux::shader::ShaderCompiler& compiler,
        rdesc::EShaderTarget target, bool legacy
    )
    {
        constexpr std::array purposes{EMaterialPurpose::FORWARD, EMaterialPurpose::GBUFFER, EMaterialPurpose::SHADOW};
        auto program = cookProgram(compiler, target, purposes, 1, .125F);
        CHECK(program.programs.size() == purposes.size());
        const auto material = call<MaterialCommands::Create>(
            ipc, commands, {materials, {program, {{1, 0, 0, 0}}, {textures[0]}}}
        ).material;
        MeshInstanceData input{mesh, material};
        input.render_enabled = true;
        const auto instance = call<InstanceCommands::CreateInstance>(ipc, commands, {instances, input}).instance;
        const auto forward = sampleRaster(server, ipc, commands, probes[0], purposes[0], scene);
        const auto gbuffer = sampleRaster(server, ipc, commands, probes[1], purposes[1], scene);
        const auto shadow = sampleRaster(server, ipc, commands, probes[2], purposes[2], scene);
        CHECK(std::abs(std::to_integer<int>(forward.center[0]) - 96) <= 1);
        CHECK(std::abs(std::to_integer<int>(gbuffer.center[0]) - 64) <= 1);
        CHECK(std::to_integer<int>(forward.center[1]) == 96 && gbuffer.center[1] == forward.center[1]);
        CHECK(std::to_integer<int>(forward.center[3]) == 255 && std::to_integer<int>(gbuffer.center[3]) == 0);
        CHECK(std::abs(std::bit_cast<float>(shadow.center) - .5F) < 1e-5F);
        CHECK(forward.image_ranges == kFif && gbuffer.image_ranges == kFif);
        CHECK(shadow.image_ranges == (legacy ? kFif : 0));
        CHECK(forward.buffer_ranges == kFif && gbuffer.buffer_ranges == kFif && shadow.buffer_ranges == kFif);
        call<MaterialCommands::Update>(ipc, commands, {{material, {{.25F, 0, 0, 0}}, {textures[0]}}});
        const auto masked = sampleRaster(server, ipc, commands, probes[2], purposes[2], scene);
        CHECK(std::bit_cast<float>(masked.center) == 1);
        call<MaterialCommands::Update>(ipc, commands, {{material, {{1, 0, 0, 0}}, {textures[0]}}});
        const auto other = call<MaterialCommands::Create>(
            ipc, commands, {materials, {std::move(program), {{.75F, 0, 0, 0}}, {textures[1]}}}
        ).material;
        input.material = other;
        input.transform[0] = input.transform[5] = .25F;
        input.transform[12] = .5F;
        const auto sibling = call<InstanceCommands::CreateInstance>(ipc, commands, {instances, input}).instance;
        input.material = material;
        const InstanceSwitch change{instances, {sibling, input}, legacy ? 2 * kFif : kFif};
        const auto together = sampleRaster(server, ipc, commands, probes[0], purposes[0], scene, &change);
        CHECK(together.buffer_ranges == kFif && together.image_ranges == 2 * kFif);
        call<InstanceCommands::ReleaseInstance>(ipc, commands, {sibling});
        call<MaterialCommands::Release>(ipc, commands, {other});
        call<InstanceCommands::ReleaseInstance>(ipc, commands, {instance});
        call<MaterialCommands::Release>(ipc, commands, {material});
        std::printf(
            "MATERIAL_PURPOSES forward_red=%u gbuffer_red=%u shadow_depth=0.5 masked_depth=1 shadow_image_ranges=%llu "
            "group_image_ranges=%llu switched_image_ranges=%llu\n",
            std::to_integer<unsigned>(forward.center[0]), std::to_integer<unsigned>(gbuffer.center[0]),
            static_cast<unsigned long long>(shadow.image_ranges), static_cast<unsigned long long>(together.image_ranges),
            static_cast<unsigned long long>(together.switched_image_ranges)
        );
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
        RenderServer& server, ipc::IpcRequester& ipc, const CommandList& commands,
        const RenderProbe<RasterProbeConfig>& forward, SceneId scene, ResourceInstanceId materials,
        ResourceInstanceId instances, MeshId mesh, std::span<const TextureId> textures,
        lux::shader::ShaderCompiler& compiler, rdesc::EShaderTarget target, unsigned count, bool varied, bool legacy
    )
    {
        constexpr std::array purposes{EMaterialPurpose::FORWARD};
        const auto abi_count = varied ? std::min(count, 128u) : 1u;
        std::vector<rdesc::MaterialProgramSet> programs;
        std::set<std::size_t> binaries;
        std::set<std::uint32_t> strides;
        std::uint64_t record_bytes = 0;
        for (unsigned index = 0; index < count; ++index)
        {
            auto program = cookProgram(compiler, target, purposes, 1 + index % abi_count, float(index + 1) / 4096);
            CHECK(binaries.emplace(program.programs.front().shader.binaryDigest()).second);
            strides.insert(program.parameters.size);
            record_bytes += program.parameters.size;
            programs.push_back(std::move(program));
        }
        CHECK(strides.size() == abi_count && textures.size() == 9);
        const auto pages = legacy ? count : abi_count * pagesFor(count / abi_count);
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
                const auto texture = textures[std::min<std::size_t>(i, textures.size() - 1)];
                ids.push_back(call<MaterialCommands::Create>(
                    ipc, commands, {materials, {program, program.parameter_defaults, {texture}}}
                ).material);
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
            for (const auto id : ids) call<MaterialCommands::Release>(ipc, commands, {id});
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
            const auto reads = sampleRaster(server, ipc, commands, forward, purposes[0], scene);
            const auto expected_pages = active == count ? pages : (legacy || varied ? sparse : 1);
            const auto expected_images = legacy ? active : std::min<unsigned>(active, 9);
            CHECK(reads.buffer_ranges == expected_pages * kFif);
            if (active == count)
            {
                CHECK(reads.buffer_bytes == resident_buffer_bytes);
            }
            else
            {
                std::uint64_t expected_bytes = programs.front().parameters.size * 16ull;
                if (legacy || varied)
                {
                    expected_bytes = 0;
                    for (unsigned index = 0; index < active; ++index)
                    {
                        expected_bytes += programs[index].parameters.size * 16ull;
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
        for (const auto id : ids) call<MaterialCommands::Release>(ipc, commands, {id});
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
    bool legacy = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "d3d12") device.backend = EGraphicsBackend::D3D12;
        else if (arg == "--varied") varied = true;
        else if (arg == "--legacy-pages") legacy = true;
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
    auto runtime = lux::execution::ExecutionRuntime::create({2, 128, {128}});
    CHECK(runtime);
    RenderServerConfig config{"test.material.scale", nativeValidationEnabled()};
    config.frames_in_flight = kFif;
    config.uploads.emplace();
    config.uploads->chunk_bytes = 4 * 1024 * 1024;
    config.uploads->window_regions = 8192;
    auto server = take(RenderServer::create(config, **runtime, device));
    take(server->registerResource<MaterialAllocationProbe>());
    take(server->registerResource<CaptureBuffer>());
    const auto visibility = take(server->featureId("lux.render.feature.MeshVisibilityFeature"));
    const auto capture = take(server->registerFeature<CaptureFeature>());
    const auto probe = [&](std::string_view feature)
    {
        return registerRenderProbe<RasterProbeConfig>(
            *server, std::string(feature), {take(server->featureId(feature)), capture, visibility},
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
    call<GetOrCreateResource>(*ipc, commands, {GlobalRenderScope{}, MaterialAllocationProbe::Descriptor::descriptor.type});
    const auto scene = call<CreateScene>(*ipc, commands, {}).scene;
    const auto instances = call<GetOrCreateResource>(*ipc, commands, {scene, kInstanceResourceName}).resource_id;
    const auto meshes = call<GetOrCreateResource>(*ipc, commands, {GlobalRenderScope{}, kMeshResourceName}).resource_id;
    const auto textures = call<GetOrCreateResource>(*ipc, commands, {GlobalRenderScope{}, kTextureResourceName}).resource_id;
    const auto materials =
        call<GetOrCreateResource>(*ipc, commands, {GlobalRenderScope{}, kMaterialResourceName}).resource_id;
    const auto quad = makeQuad();
    const auto mesh = call<MeshCommands::UploadMesh>(*ipc, commands, {meshes, &quad}).mesh;
    std::vector<TextureId> images;
    for (unsigned index = 0; index < (count ? 9u : 2u); ++index)
    {
        images.push_back(uploadTexture(*ipc, commands, textures, 64 + index));
    }
    if (count)
    {
        verifyScale(*server, *ipc, commands, probes[0], scene, materials, instances, mesh, images, **compiler,
            shader_target, count, varied, legacy);
    }
    else
    {
        verifyPurposes(*server, *ipc, commands, probes, scene, materials, instances, mesh,
            std::span<const TextureId, 2>(images.data(), 2), **compiler, shader_target, legacy);
        call<RemoveScene>(*ipc, commands, {scene});
    }
    call<MeshCommands::ReleaseMesh>(*ipc, commands, {mesh});
    for (const auto image : images) call<TextureCommands::Release>(*ipc, commands, {image});
    server->stop();
    thread.join();
    CHECK(server->stats().validation_errors == 0);
}
