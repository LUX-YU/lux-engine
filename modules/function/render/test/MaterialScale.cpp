#include <lux/engine/render/runtime/commands/InstanceCommands.hpp>
#include <lux/engine/render/runtime/commands/MaterialCommands.hpp>
#include <lux/engine/render/runtime/commands/MeshCommands.hpp>
#include <lux/engine/render/runtime/commands/TextureCommands.hpp>
#include <lux/engine/render/runtime/commands/TransformCommands.hpp>
#include <lux/engine/render/runtime/JobId.hpp>
#include <lux/engine/render/runtime/TargetId.hpp>
#include <lux/engine/render/device/DeviceId.hpp>
#include <lux/engine/render/effect/EffectId.hpp>
#include <lux/engine/render/resource/transform/Resource.hpp>
#include "MaterialFixture.hpp"
#include "SceneCapture.hpp"

#include <bit>
#include <ctime>
#include <mutex>
#include <set>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/render/effect/builtin/DepthPyramid.hpp>
#include <lux/engine/render/effect/builtin/GBuffer.hpp>
#include <lux/engine/render/effect/builtin/Lighting.hpp>
#include <lux/engine/render/effect/builtin/MeshRaster.hpp>
#include <lux/engine/render/effect/builtin/MeshVisibility.hpp>
#include <lux/engine/render/effect/builtin/ToneMapping.hpp>
#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/render/effect/EffectImplementation.hpp>
#include <lux/engine/render/resource/transform/Binding.hpp>
#include <lux/engine/render/resource/view/Resource.hpp>
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
            return makeResourceRegistration<Descriptor, MaterialAllocationProbe, &create>();
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

            static inline const ResourceDefinition descriptor{ResourceNameView{"test.material.pipeline_gate"}, 0, 0, {}};
        };

        enum class EGate { OPEN, ARMED, HOLDING };
        static inline std::atomic<EGate> gate = EGate::OPEN;
        static inline std::atomic<std::uint64_t> accepted_submits = 0, raster_submits = 0;
        static inline std::atomic<std::uint64_t> pipeline_calls = 0, created_pipelines = 0, destroyed_pipelines = 0;
        static inline std::atomic<bool> fail_pipeline = false;

        static void trackCreations(bool enabled)
        {
            const std::lock_guard lock(commands_mutex_);
            track_creations_ = enabled;
        }

        static std::size_t trackedPipelines()
        {
            const std::lock_guard lock(commands_mutex_);
            return tracked_.size();
        }

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
                if (result == nri::Result::SUCCESS)
                {
                    ++created_pipelines;
                    const std::lock_guard lock(commands_mutex_);
                    if (track_creations_) tracked_.insert(pipeline);
                }
                return result;
            };
            core_.DestroyPipeline = [](nri::Pipeline* pipeline) noexcept
            {
                if (pipeline)
                {
                    ++destroyed_pipelines;
                    const std::lock_guard lock(commands_mutex_);
                    tracked_.erase(pipeline);
                }
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
            return makeResourceRegistration<Descriptor, PipelineIsolationProbe, &create>();
        }

    private:
        static inline nri::CoreInterface original_{};
        static inline std::mutex commands_mutex_;
        static inline bool track_creations_ = false;
        static inline std::set<nri::Pipeline*> tracked_;
        static inline std::set<const nri::CommandBuffer*> raster_commands_;
        nri::CoreInterface& core_;
    };

    namespace graph = lux::graph;

    constexpr std::uint32_t kFif = 3;
    constexpr TargetDesc kTarget{64, 64, ETextureFormat::RGBA8_UNORM};
    constexpr std::uint32_t row_pitch = 256;
    constexpr std::uint32_t capture_bytes = row_pitch * 64;
    constexpr std::array<std::string_view, 2> capture_bindings{"pixels", "sampled_pixels"};
    constexpr std::string_view imported_hdr = "sampled_hdr";
    constexpr std::string_view hdr_capability = "test.material.hdr";
    constexpr std::string_view clear_hdr = "test.material.hdr.clear";
    constexpr std::string_view sample_hdr = "test.material.hdr.sample";
    constexpr auto capture_input = effectPortSemantic("input");
    constexpr auto capture_output = effectPortSemantic("bytes");

    using MaterialFrameResources = std::tuple<
        MaterialResource, InstanceResource, TransformResource, ViewParameterResource
    >;

    struct MaterialReadbackDescriptor final
    {
        using Config = void;

        static inline const ResourceDefinition descriptor{
            ResourceNameView{"test.material.readback"}, 0, 0, {}, "View-owned material capture.", EResourceDomain::VIEW
        };
    };

    struct MaterialReadback final
    {
        using Descriptor = MaterialReadbackDescriptor;
        using Configuration = std::array<CaptureConfig, 2>;

        // The fixture supplies the configuration through the synchronous Resource creation call.
        static inline std::atomic<const Configuration*> creation_input{};

        explicit MaterialReadback(ResourceCreateContext& context, const Configuration& config) noexcept
            : storage(context), configuration(config)
        {
        }

        static RenderResult<std::unique_ptr<MaterialReadback>>
        create(ResourceCreateContext& context, ResourceInstanceId) noexcept
        {
            const auto* config = creation_input.load();
            if (!config) return lux::cxx::unexpected(RenderError{kInvalidConfig});
            return std::make_unique<MaterialReadback>(context, *config);
        }

        static ResourceRegistration factory() noexcept
        {
            static const StorageBindingOps binding{
                .describeGraphImport = [](const void* object, std::uint32_t projection) noexcept
                    -> RenderResult<StorageGraphImport>
                {
                    const auto& capture = *static_cast<const MaterialReadback*>(object);
                    if (projection >= capture.configuration.size() || !capture.configuration[projection].result)
                        return lux::cxx::unexpected(RenderError{kStorageContract});
                    return StorageGraphImport::bufferOutput(capture_bytes, EGraphUse::TRANSFER_DESTINATION);
                },
                .resolveGraphImport = [](
                    void* object, std::uint32_t projection, const VGraphResourceDesc& description,
                    std::span<ResourceInput> output
                ) noexcept
                {
                    auto& capture = *static_cast<MaterialReadback*>(object);
                    return CaptureBuffer::resolve(
                        &capture.storage, description, &capture.configuration[projection], output
                    );
                }
            };
            return makeResourceRegistration<Descriptor, MaterialReadback, &create>(&binding);
        }

        CaptureBuffer storage;
        Configuration configuration;
    };

    EffectNodeDefinition captureDefinition(EMaterialPurpose purpose)
    {
        constexpr auto direction = graph::EPinDirection::INPUT;
        const auto name = purpose == EMaterialPurpose::GBUFFER ? "test.material.capture.gbuffer" :
            purpose == EMaterialPurpose::SHADOW ? "test.material.capture.depth" : "test.material.capture.color";
        const auto type = purpose == EMaterialPurpose::GBUFFER ? GBufferBundle::type :
            purpose == EMaterialPurpose::SHADOW ? DepthPyramid::depth_type : ToneMapping::display_type;
        return {
            .identity = {graph::nodeTypeId(name), name, 1},
            .ports = {
                {capture_input, "input", {std::string{type}, 1}, direction, 1, true},
                {capture_output, "bytes", {"test.material.readback.bytes", 1}, graph::EPinDirection::OUTPUT}
            }
        };
    }

    template <EMaterialPurpose Purpose>
    RenderResult<void> buildMaterialCapture(EffectCompileContext& context, std::string_view arguments) noexcept
    {
        constexpr bool depth = Purpose == EMaterialPurpose::SHADOW;
        constexpr auto member = Purpose == EMaterialPurpose::GBUFFER ?
            effectPortSemantic(GBufferBundle::members.front()) : depth ? MeshRaster::depth : MeshRaster::color;
        const auto inputs = context.inputs(capture_input);
        CHECK(inputs.size() == 1);
        const auto source = std::ranges::find(inputs.front(), member, &EffectResourceValue::member);
        CHECK(source != inputs.front().end());
        const std::uint32_t projection = arguments == "extra" ? 1 : 0;
        const auto readback = take(context.importFinalBuffer(
            capture_bindings[projection], {ResourceName{MaterialReadbackDescriptor::descriptor.type.name()}},
            {"", GraphBufferDesc{capture_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION}, projection
        ));
        const auto copied = take(context.addTransferPass(
            "copy " + std::string{capture_bindings[projection]}, TransferWork{{
                CopyImageBuffer{
                    {source->version, GraphImageRange{depth ? 2u : 1u}},
                    {readback}, row_pitch, capture_bytes, ImageCopyRegion{0, 0, 64, 64}
                }
            }}
        ));
        return context.publish(capture_output, {{capture_output, copied.written.front()}});
    }

    EffectNodeDefinition hdrDefinition()
    {
        return {
            .identity = {graph::nodeTypeId(hdr_capability), std::string{hdr_capability}, 1},
            .ports = {{Lighting::hdr, "hdr_color", {std::string{HdrColorBundle::type}, HdrColorBundle::version}, graph::EPinDirection::OUTPUT}}
        };
    }

    template <bool Import>
    RenderResult<void> buildHdr(EffectCompileContext& context, std::string_view) noexcept
    {
        ResourceVersion color;
        if constexpr (Import)
        {
            const auto imported = context.importTargetImage(imported_hdr, ETextureFormat::RGBA16_SFLOAT);
            if (!imported) return lux::cxx::unexpected(imported.error());
            color = *imported;
        }
        else
        {
            GraphImageDesc image{0, 0, 1, 1, ETextureFormat::RGBA16_SFLOAT};
            image.extent.source = EImageExtentSource::TARGET;
            const auto output = take(context.declareResource({"HDR source", image}));
            const auto cleared = take(context.addTransferPass(
                "clear HDR", TransferWork{{ClearImage{{{output, 0}}, {.25F, .5F, .75F, 1}}}}
            ));
            color = cleared.written.front();
        }
        return context.publish(Lighting::hdr, {{Lighting::hdr, color}});
    }

    void registerMaterialNode(
        RenderServer& server, graph::NodeTypeId type, std::string_view name, graph::PinSemanticId output,
        EffectImplementation::Build build
    )
    {
        EffectImplementation implementation;
        implementation.capability = type;
        implementation.identity = {std::string{name}, 1};
        implementation.supported_outputs = {output};
        implementation.initialize = [](EffectInitializationContext&, std::string_view) noexcept -> RenderResult<void>
        {
            return {};
        };
        implementation.build = build;
        CHECK(server.registerEffectImplementation(std::move(implementation)));
    }

    template <EMaterialPurpose Purpose> void registerMaterialCapture(RenderServer& server)
    {
        const auto definition = captureDefinition(Purpose);
        CHECK(server.registerEffectCapability(definition));
        registerMaterialNode(
            server, definition.identity.id, definition.identity.canonical_name, capture_output,
            buildMaterialCapture<Purpose>
        );
    }

    graph::PinId addMaterialCapture(EffectGraphEdit& edit, EMaterialPurpose purpose, graph::PinId source,
                                    std::string arguments = {})
    {
        const auto definition = captureDefinition(purpose);
        const auto node = take(edit.addNode(definition, {definition.identity.canonical_name, 1}, std::move(arguments)));
        CHECK(edit.connect(source, edit.pinId(node, capture_input)));
        return edit.pinId(node, capture_output);
    }

    CreateEffectRequest rasterEffect(EMaterialPurpose purpose, bool samples_hdr = false)
    {
        CreateEffectRequest request;
        request.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*request.graph};
        graph::PinId output;
        if (purpose == EMaterialPurpose::GBUFFER)
        {
            const auto node = take(edit.addNode(
                GBuffer::definition(), {std::string{GBuffer::indexed_clockwise}, GBuffer::version}
            ));
            output = edit.pinId(node, GBuffer::surface);
        }
        else
        {
            const bool forward = purpose == EMaterialPurpose::FORWARD;
            const auto name = forward ? MeshRaster::forward_clockwise : MeshRaster::depth_only_clockwise;
            const auto node = take(edit.addNode(MeshRaster::definition(), {std::string{name}, MeshRaster::version}));
            output = edit.pinId(node, forward ? MeshRaster::color : MeshRaster::depth);
            if (forward) CHECK(edit.setOutput("color", output, true));
        }
        CHECK(edit.setOutput("pixels", addMaterialCapture(edit, purpose, output)));
        if (samples_hdr)
        {
            const auto input = take(edit.addNode(hdrDefinition(), {std::string{sample_hdr}, 1}));
            const auto tone = take(edit.addNode(
                ToneMapping::definition(), {std::string{ToneMapping::linear}, ToneMapping::version}
            ));
            CHECK(edit.connect(edit.pinId(input, Lighting::hdr), edit.pinId(tone, ToneMapping::hdr_color)));
            const auto converted = edit.pinId(tone, ToneMapping::color);
            CHECK(edit.setOutput("sampled_pixels", addMaterialCapture(edit, EMaterialPurpose::FORWARD, converted, "extra")));
        }
        CHECK(edit.commit());
        return request;
    }

    CreateEffectRequest hdrEffect()
    {
        CreateEffectRequest request;
        request.output = {ETextureFormat::RGBA16_SFLOAT, 1, ETargetKind::OFFSCREEN, true};
        request.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*request.graph};
        const auto node = take(edit.addNode(hdrDefinition(), {std::string{clear_hdr}, 1}));
        CHECK(edit.setOutput("color", edit.pinId(node, Lighting::hdr), true));
        CHECK(edit.commit());
        return request;
    }

    JobBinding materialBinding(
        CaptureDevice device, ViewScope view,
        const MaterialReadback::Configuration& capture
    )
    {
        const auto& [ipc, commands, session] = device;
        const auto local = [&](ResourceNameView name)
        {
            return call<GetOrCreateResource>(ipc, commands, {session, view, name}).resource_id;
        };
        JobBinding binding;
        MaterialReadback::creation_input.store(&capture);
        const auto readback = local(MaterialReadbackDescriptor::descriptor.type);
        MaterialReadback::creation_input.store(nullptr);
        const auto instances = local(kInstanceResourceName);
        binding.resources = {
            {std::string{ViewParameterResource::snapshot_binding}, local(kViewParameterResourceName),
                0, EJobResourceUse::CPU_SNAPSHOT},
            {std::string{MeshVisibility::instances_binding}, instances, 0, EJobResourceUse::CPU_SNAPSHOT},
            {std::string{MeshVisibility::transforms_binding}, local(kTransformResourceName),
                static_cast<std::uint32_t>(ETransformGraphProjection::RECORDS)},
            {std::string{MeshVisibility::properties_binding}, instances,
                static_cast<std::uint32_t>(EMeshInstanceBinding::PROPERTIES)},
            {std::string{MeshVisibility::alive_binding}, instances,
                static_cast<std::uint32_t>(EMeshInstanceBinding::ALIVE)},
            {std::string{GBuffer::geometry_binding}, local(kMeshResourceName), 0, EJobResourceUse::DRAW_RESOURCE},
            {std::string{GBuffer::materials_binding}, local(kMaterialResourceName), 0, EJobResourceUse::DRAW_RESOURCE},
            {std::string{GBuffer::textures_binding}, local(kTextureResourceName), 0, EJobResourceUse::DRAW_RESOURCE},
            {std::string{capture_bindings[0]}, readback}
        };
        if (capture[1].result) binding.resources.push_back({std::string{capture_bindings[1]}, readback, 1});
        return binding;
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

    struct MaterialTestScene final
    {
        ipc::IpcRequester& ipc;
        const CommandList& commands;
        DeviceSessionId session;
        SceneId scene;
    };

    class PurposeCapture final
    {
    public:
        PurposeCapture(MaterialTestScene scene, EMaterialPurpose purpose) : scene_(scene)
        {
            const auto& [ipc, commands, session, scene_id] = scene_;
            const auto format = purpose == EMaterialPurpose::SHADOW ? ETextureFormat::D32_SFLOAT : kTarget.format;
            effect_ = createPreparedEffect(ipc, commands, session, rasterEffect(purpose));
            view_ = call<CreateView>(ipc, commands, {scene_id}).view;
            output_ = call<CreateTarget>(ipc, commands, {session, kTarget}).target;
            const MaterialReadback::Configuration capture{{{&pixels_, format}, {}}};
            auto binding = materialBinding({ipc, commands, session}, {scene_id, view_}, capture);
            job = startRenderJob(ipc, commands, session, scene_id, view_, output_, effect_, std::move(binding));
        }

        void renderReplicas()
        {
            for (unsigned slot = 0; slot < kFif; ++slot) renderFrame(scene_.ipc, scene_.commands, job);
        }

        std::array<std::byte, 4> finish()
        {
            const auto& [ipc, commands, session, scene] = scene_;
            stopAndRemoveJob(ipc, commands, job);
            call<RemoveView>(ipc, commands, {scene, view_});
            call<RemoveTarget>(ipc, commands, output_);
            call<RemoveEffect>(ipc, commands, {effect_});
            const auto deadline = Clock::now() + 15s;
            while (!pixels_.ready.load(std::memory_order_acquire))
            {
                CHECK(Clock::now() < deadline);
                std::this_thread::yield();
            }
            CHECK(pixels_.frames.size() == kFif);
            std::array<std::byte, 4> center{};
            const auto offset = 32 * pixels_.pitch + 32 * 4;
            std::memcpy(center.data(), pixels_.frames.front().data() + offset, center.size());
            for (const auto& frame : pixels_.frames)
            {
                CHECK(std::memcmp(center.data(), frame.data() + offset, center.size()) == 0);
            }
            return center;
        }

        RenderJobId job;

    private:
        MaterialTestScene scene_;
        CapturedPixels pixels_;
        EffectId effect_;
        ViewId view_;
        TargetId output_;
    };

    // One asset is visible to both installed Jobs. Only the requested GBuffer specialization fails.
    void verifyPurposeFailure(
        MaterialTestScene scene, ResourceInstanceId instances, MeshInstanceUpdate old, MaterialId replacement
    )
    {
        PurposeCapture gbuffer{scene, EMaterialPurpose::GBUFFER};
        PurposeCapture shadow{scene, EMaterialPurpose::SHADOW};
        PipelineIsolationProbe::trackCreations(true);
        gbuffer.renderReplicas();
        PipelineIsolationProbe::trackCreations(false);
        CHECK(PipelineIsolationProbe::trackedPipelines() == 2);
        shadow.renderReplicas();
        const auto source = call<GetOrCreateResource>(
            scene.ipc, scene.commands, {scene.session, DeviceScope{}, kMaterialResourceName}
        ).resource_id;
        const auto warm_calls = PipelineIsolationProbe::pipeline_calls.load();
        const std::array changed_colors{MaterialColorUpdate{old.description.material, 0, {.75F, 0, 0, 0}}};
        call<MaterialCommands::UpdateColor>(scene.ipc, scene.commands, {source, changed_colors});
        gbuffer.renderReplicas();
        shadow.renderReplicas();
        CHECK(PipelineIsolationProbe::pipeline_calls.load() == warm_calls);
        const std::array original_colors{MaterialColorUpdate{old.description.material, 0, {1, 0, 0, 0}}};
        call<MaterialCommands::UpdateColor>(scene.ipc, scene.commands, {source, original_colors});

        auto changed = old;
        changed.description.material = replacement;
        call<InstanceCommands::Update>(scene.ipc, scene.commands, {instances, {&changed, 1}});
        shadow.renderReplicas();
        PipelineIsolationProbe::fail_pipeline = true;
        const auto rejected = call<RequestRenderJob>(scene.ipc, scene.commands, {gbuffer.job}, false);
        CHECK(rejected.error.type == kDeviceFailure && !PipelineIsolationProbe::fail_pipeline.load());
        const auto calls = PipelineIsolationProbe::pipeline_calls.load();
        CHECK(call<RequestRenderJob>(scene.ipc, scene.commands, {gbuffer.job}, false).error == rejected.error);
        shadow.renderReplicas();
        const auto depth = std::bit_cast<float>(shadow.finish());
        CHECK(depth == 1.F);
        call<InstanceCommands::Update>(scene.ipc, scene.commands, {instances, {&old, 1}});
        gbuffer.renderReplicas();
        const auto color = gbuffer.finish();
        CHECK(std::abs(std::to_integer<int>(color[0]) - 64) <= 1);
        CHECK(PipelineIsolationProbe::pipeline_calls.load() == calls);
        std::printf("PURPOSE FAILURE PASS: ready Shadow depth=%g, restored GBuffer red=%u, parameter PSOs=0, extra PSOs=0\n",
            depth, std::to_integer<unsigned>(color[0]));

        CHECK(PipelineIsolationProbe::trackedPipelines() == 2);
        PurposeCapture restarted{scene, EMaterialPurpose::GBUFFER};
        const auto before_restart_draw = PipelineIsolationProbe::pipeline_calls.load();
        restarted.renderReplicas();
        const auto restart_calls = PipelineIsolationProbe::pipeline_calls.load() - before_restart_draw;
        CHECK(restart_calls == 0 && restarted.finish() == color);
        std::printf("MATERIAL READY RESIDENCY idle_cache=0 first_draw_after_restart_psos=%llu\n",
            static_cast<unsigned long long>(restart_calls));
    }

    RasterSample sampleRaster(
        RenderServer& server, ipc::IpcRequester& ipc, const CommandList& commands, DeviceSessionId session,
        EMaterialPurpose purpose, SceneId scene,
        const InstanceSwitch* change = nullptr,
        std::span<const MaterialColorUpdate> colors = {},
        bool reject_transform = false
    )
    {
        PurposeCapture capture{{ipc, commands, session, scene}, purpose};
        const auto job = capture.job;
        if (!colors.empty())
        {
            const auto schema = call<QueryRenderJobInputLayout>(ipc, commands, job).layout;
            auto packet = take(FrameData::create<MaterialFrameResources>(schema));
            for (const auto& value : colors) CHECK(packet.addData<MaterialResource>(value));
            if (reject_transform) CHECK(packet.addData<TransformResource>(TransformRecordUpdate{}));
            CHECK(packet.seal());
            const auto reply = call<RequestRenderJob>(ipc, commands, {job, &packet}, !reject_transform);
            if (reject_transform) CHECK(reply.error.type == kStorageIdentity);
            else CHECK(reply.job == job);
            packet.release();
        }
        const auto before = server.developmentStats();
        CHECK(before);
        for (unsigned slot = 0; slot < kFif; ++slot) renderFrame(ipc, commands, job);
        const auto after = server.developmentStats();
        CHECK(after);
        std::uint64_t switched_image_ranges = 0;
        if (change)
        {
            call<InstanceCommands::Update>(ipc, commands, {change->resource, {&change->update, 1}});
            for (unsigned slot = 0; slot < kFif; ++slot) renderFrame(ipc, commands, job);
            const auto switched = server.developmentStats();
            CHECK(switched);
            CHECK(switched->raster_buffer_ranges - after->raster_buffer_ranges == kFif);
            switched_image_ranges = switched->raster_image_ranges - after->raster_image_ranges;
            CHECK(switched_image_ranges == change->expected_image_ranges);
        }
        const auto center = capture.finish();
        return {
            center, after->raster_buffer_ranges - before->raster_buffer_ranges,
            after->raster_buffer_bytes - before->raster_buffer_bytes,
            after->raster_image_ranges - before->raster_image_ranges,
            after->raster_image_subresources - before->raster_image_subresources, switched_image_ranges
        };
    }

    void verifyPurposes(
        RenderServer& server, ipc::IpcRequester& ipc, const CommandList& commands, DeviceSessionId session,
        SceneId scene, ResourceInstanceId materials, ResourceInstanceId instances, MeshId mesh,
        std::span<const TextureId, 2> textures, lux::shader::ShaderCompiler& compiler,
        rdesc::EShaderTarget target
    )
    {
        constexpr std::array purposes{EMaterialPurpose::FORWARD, EMaterialPurpose::GBUFFER, EMaterialPurpose::SHADOW};
        auto program = cookProgram(compiler, target, purposes, 1, .125F);
        CHECK(program.programs.size() == purposes.size());
        const auto registered = registerMaterialProgram(ipc, commands, materials, program);
        const auto equal_code = registerMaterialProgram(ipc, commands, materials, std::move(program));
        CHECK(equal_code != registered);
        const auto material = createMaterialInstance(ipc, commands, registered, {{1, 0, 0, 0}}, {textures[0]});
        const auto transform_storage = sceneTransformStorage(ipc, commands, session, scene);
        MeshInstanceData input{mesh, material};
        input.transform_id = createTransform(ipc, commands, transform_storage);
        const auto original_transform = input.transform_id;
        input.render_enabled = true;
        const auto instance = call<InstanceCommands::CreateInstance>(ipc, commands, {instances, input}).instance;
        {
            auto changed = cookProgram(compiler, target, purposes, 2, .75F);
            auto values = changed.parameter_defaults;
            values.front()[0] = .25F;
            const auto code = registerMaterialProgram(ipc, commands, materials, std::move(changed));
            const auto replacement = createMaterialInstance(ipc, commands, code, std::move(values), {textures[0]});
            verifyPurposeFailure({ipc, commands, session, scene}, instances, {instance, input}, replacement);
            call<MaterialCommands::ReleaseInstance>(ipc, commands, {replacement});
            call<MaterialCommands::UnregisterProgram>(ipc, commands, {code});
        }
        const auto forward = sampleRaster(server, ipc, commands, session, purposes[0], scene);
        const auto gbuffer = sampleRaster(server, ipc, commands, session, purposes[1], scene);
        const auto shadow = sampleRaster(server, ipc, commands, session, purposes[2], scene);
        CHECK(std::abs(std::to_integer<int>(forward.center[0]) - 96) <= 1);
        CHECK(std::abs(std::to_integer<int>(gbuffer.center[0]) - 64) <= 1);
        CHECK(std::to_integer<int>(forward.center[1]) == 96 && gbuffer.center[1] == forward.center[1]);
        CHECK(std::to_integer<int>(forward.center[3]) == 255 && std::to_integer<int>(gbuffer.center[3]) == 0);
        CHECK(std::abs(std::bit_cast<float>(shadow.center) - .5F) < 1e-5F);
        CHECK(forward.image_ranges == kFif && gbuffer.image_ranges == kFif);
        CHECK(shadow.image_ranges == 0);
        CHECK(forward.buffer_ranges == kFif && gbuffer.buffer_ranges == kFif && shadow.buffer_ranges == kFif);
        const std::array mask_color{MaterialColorUpdate{material, 0, {.25F, 0, 0, 0}}};
        call<MaterialCommands::UpdateColor>(ipc, commands, {materials, mask_color});
        const auto masked = sampleRaster(server, ipc, commands, session, purposes[2], scene);
        CHECK(std::bit_cast<float>(masked.center) == 1);
        const std::array invalid_batch{
            MaterialColorUpdate{material, 0, {1, 0, 0, 0}}, MaterialColorUpdate{material, 1, {1, 0, 0, 0}}
        };
        CHECK(call<MaterialCommands::UpdateColor>(ipc, commands, {materials, invalid_batch}, false).error.type ==
            kStorageContract);
        CHECK(sampleRaster(server, ipc, commands, session, purposes[2], scene).center == masked.center);
        const std::array colors{
            MaterialColorUpdate{material, 0, {.5F, 0, 0, 0}}, MaterialColorUpdate{material, 0, {1, 0, 0, 0}}
        };
        const auto restored = sampleRaster(server, ipc, commands, session, purposes[0], scene, nullptr, colors);
        CHECK(restored.center == forward.center);
        const auto restored_shadow = sampleRaster(server, ipc, commands, session, purposes[2], scene);
        CHECK(restored_shadow.center == shadow.center);
        const auto partial = sampleRaster(
            server, ipc, commands, session, purposes[2], scene, nullptr, mask_color, true
        );
        CHECK(partial.center == masked.center);
        call<MaterialCommands::UpdateColor>(ipc, commands, {materials, colors});
        std::puts("MATERIAL COLOR PASS: Command, FrameData and partial Resource adoption reached forward and shadow");
        const auto other = createMaterialInstance(ipc, commands, registered, {{.75F, 0, 0, 0}}, {textures[1]});
        input.material = other;
        auto sibling_matrix = kIdentityTransform;
        sibling_matrix[0] = sibling_matrix[5] = .25F;
        sibling_matrix[12] = .5F;
        input.transform_id = createTransform(ipc, commands, transform_storage, sibling_matrix);
        const auto sibling = call<InstanceCommands::CreateInstance>(ipc, commands, {instances, input}).instance;
        input.material = material;
        const InstanceSwitch change{instances, {sibling, input}, kFif};
        const auto together = sampleRaster(server, ipc, commands, session, purposes[0], scene, &change);
        if (together.buffer_ranges != kFif || together.image_ranges != 2 * kFif)
            std::fprintf(stderr, "Grouped material reads: buffers=%llu images=%llu\n",
                static_cast<unsigned long long>(together.buffer_ranges),
                static_cast<unsigned long long>(together.image_ranges)
            );
        CHECK(together.buffer_ranges == kFif && together.image_ranges == 2 * kFif);
        call<InstanceCommands::ReleaseInstance>(ipc, commands, {sibling});
        call<TransformCommands::Release>(ipc, commands, input.transform_id);
        call<MaterialCommands::ReleaseInstance>(ipc, commands, {other});
        const auto twin = createMaterialInstance(ipc, commands, equal_code, {{1, 0, 0, 0}}, {textures[0]});
        MeshInstanceData same_mesh{mesh, twin};
        same_mesh.transform_id = original_transform;
        same_mesh.render_enabled = true;
        const MeshInstanceUpdate use_twin{instance, same_mesh};
        call<InstanceCommands::Update>(ipc, commands, {instances, {&use_twin, 1}});
        const auto reuse = [&]
        {
            PurposeCapture capture{{ipc, commands, session, scene}, EMaterialPurpose::GBUFFER};
            const auto before = PipelineIsolationProbe::pipeline_calls.load();
            capture.renderReplicas();
            CHECK(PipelineIsolationProbe::pipeline_calls.load() == before);
            CHECK(capture.finish() == gbuffer.center && PipelineIsolationProbe::trackedPipelines() == 2);
        };
        reuse();
        call<MaterialCommands::ReleaseInstance>(ipc, commands, {material});
        call<MaterialCommands::UnregisterProgram>(ipc, commands, {registered});
        reuse();
        call<InstanceCommands::ReleaseInstance>(ipc, commands, {instance});
        call<TransformCommands::Release>(ipc, commands, original_transform);
        call<MaterialCommands::ReleaseInstance>(ipc, commands, {twin});
        call<MaterialCommands::UnregisterProgram>(ipc, commands, {equal_code});
        const auto retired = Clock::now() + 10s;
        while (PipelineIsolationProbe::trackedPipelines())
        {
            CHECK(Clock::now() < retired);
            std::this_thread::yield();
        }
        std::puts("MATERIAL READY RETIREMENT shared_assets=2 survived_first_unregister=2 destroyed_after_last_unregister=2");
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
        TransformId transform;
    };

    struct ProbeMaterial final
    {
        MaterialProgramId program;
        MaterialId instance;
    };

    struct PipelineInterruption final
    {
        RenderServer& server;
        ipc::IpcRequester& requests;
        TargetId input;
        MeshId mesh;
        bool replace_job;
    };

    JobSubmitted verifyPreparationLifecycle(
        MaterialTestScene owner, PipelineJob& job, const PipelineInterruption& interruption,
        std::span<const ProbeMaterial, 3> variants
    )
    {
        const auto& [client, commands, session, scene] = owner;
        const auto& [server, requests, input, mesh, replace_job] = interruption;
        using Probe = PipelineIsolationProbe;
        const auto changeMaterial = [&](MaterialId material)
        {
            MeshInstanceData value{mesh, material};
            value.transform_id = job.transform;
            value.render_enabled = true;
            const MeshInstanceUpdate update{job.member, value};
            call<InstanceCommands::Update>(client, commands, {job.instances, {&update, 1}});
        };
        const auto snapshot = [&]()
        {
            auto stats = server.developmentStats();
            CHECK(stats);
            return *stats;
        };
        const auto resizeDuringCompilation = [&]
        {
            // The imported target must stay sampleable. Rejection leaves its previously published pixels intact.
            auto invalid = kTarget;
            invalid.format = ETextureFormat::RGBA16_SFLOAT;
            invalid.sampled = false;
            CHECK(call<ResizeTarget>(client, commands, {input, invalid}, false).error.type == kCommandContract);
            CHECK(call<RemoveTarget>(client, commands, input, false).error.type == kDeviceBusy);
            auto larger = kTarget;
            larger.height *= 2;
            const auto changed = call<ResizeTarget>(client, commands, {job.target, larger});
            const auto restored = call<ResizeTarget>(client, commands, {job.target, kTarget});
            CHECK(restored.epoch == changed.epoch + 1 && restored.description == kTarget);
            call<SetTargetActive>(client, commands, {job.target, true, restored.epoch});
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

        EffectId successor;
        if (replace_job)
            successor = createPreparedEffect(client, commands, session, rasterEffect(EMaterialPurpose::FORWARD, true));
        changeMaterial(variants[0].instance);
        const auto stop_before = snapshot();
        const auto stop_creates = Probe::created_pipelines.load();
        const auto stop_destroys = Probe::destroyed_pipelines.load();
        Probe::gate = Probe::EGate::ARMED;
        const auto interrupted = requestFrame(requests, commands, job.job);
        Probe::waitForDriver();
        CHECK(call<RebindRenderJobEffect>(client, commands, {session, job.job, job.effect}, false).error.type == kJobBusy);
        resizeDuringCompilation();
        call<StopRenderJob>(client, commands, job.job);
        const auto stopped = receiveFrameBefore(requests, interrupted, Clock::now() + 10s);
        CHECK(stopped && stopped->error.type == kJobStopped);
        changeMaterial(job.material);
        call<MaterialCommands::ReleaseInstance>(client, commands, {variants[0].instance});
        call<MaterialCommands::UnregisterProgram>(client, commands, {variants[0].program});
        if (replace_job)
        {
            const auto previous = job.job;
            call<RemoveRenderJob>(client, commands, previous);
            call<RemoveEffect>(client, commands, {job.effect});
            job.effect = successor;
            job.job = call<CreateRenderJob>(client, commands,
                {scene, job.view, job.effect, EJobCadence::REQUESTED}
            ).job;
            CHECK(job.job != previous);
        }
        call<RebindRenderJobEffect>(client, commands, {{}, job.job, job.effect});
        call<StartRenderJob>(client, commands, {session, job.job, job.target, job.binding});
        Probe::releaseDriver();
        waitForDiscard(stop_before.discarded_preparations);
        CHECK(Probe::created_pipelines.load() == stop_creates + 1);
        CHECK(Probe::destroyed_pipelines.load() == stop_destroys + 1);
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
        auto packet = take(FrameData::create<MaterialFrameResources>(layout));
        MeshInstanceData value{mesh, variants[1].instance};
        value.transform_id = job.transform;
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
        std::printf("MATERIAL_PROGRAM_INPUT unregistered_during_native_compile=1 stale_candidate_destroyed=1 "
                    "resize_epochs=2 replaced_job_and_effect=%u\n", replace_job ? 1u : 0u);
        return *final;
    }

    void verifyPipelineIsolation(
        RenderServer& server, std::thread& server_thread, ipc::IpcRequester& client, const CommandList& commands, DeviceSessionId session,
        std::string_view channel,
        std::array<SceneId, 2> scenes, ResourceInstanceId materials,
        MeshId mesh, TextureId texture, lux::shader::ShaderCompiler& compiler, rdesc::EShaderTarget target,
        bool expect_progress, unsigned workers, bool stop_server
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
            const auto effect = createPreparedEffect(client, commands, session, hdrEffect());
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
            job.transform = createTransform(client, commands, sceneTransformStorage(client, commands, session, scenes[i]));
            input.transform_id = job.transform;
            input.render_enabled = true;
            job.member = call<InstanceCommands::CreateInstance>(client, commands, {job.instances, input}).instance;
            const bool samples_hdr = i == 1 && expect_progress;
            job.effect = createPreparedEffect(client, commands, session, rasterEffect(purposes[0], samples_hdr));
            job.view = call<CreateView>(client, commands, {scenes[i]}).view;
            job.target = call<CreateTarget>(client, commands, {session, kTarget}).target;
            const MaterialReadback::Configuration capture{{
                {&job.pixels, kTarget.format}, {samples_hdr ? &imported_pixels : nullptr, kTarget.format}
            }};
            job.binding = materialBinding({client, commands, session}, {scenes[i], job.view}, capture);
            if (samples_hdr) job.binding.target_images.push_back({.target = source, .binding = std::string{imported_hdr}});
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
            stop_packet.emplace(take(FrameData::create<MaterialFrameResources>(layout)));
            MeshInstanceData value{mesh, jobs[1].material};
            value.transform_id = jobs[1].transform;
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
            warm_packet.emplace(take(FrameData::create<MaterialFrameResources>(layout)));
            auto moved = kIdentityTransform;
            moved[12] = .125F;
            CHECK(warm_packet->addData<TransformResource>(TransformRecordUpdate{jobs[0].transform, moved}));
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
        if (cold_reply && cold_reply->error.type)
            std::fprintf(stderr, "Cold Job error %llu with arguments %llu/%llu/%llu\n",
                static_cast<unsigned long long>(cold_reply->error.type),
                static_cast<unsigned long long>(cold_reply->error.args[0]),
                static_cast<unsigned long long>(cold_reply->error.args[1]),
                static_cast<unsigned long long>(cold_reply->error.args[2])
            );
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
                {client, commands, session, scenes[1]}, jobs[1], {server, *cold_client, source, mesh, workers == 1},
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
        SceneId scene, ResourceInstanceId materials,
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
        std::vector<TransformMatrix> matrices;
        matrices.reserve(count);
        for (unsigned index = 0; index < count; ++index)
        {
            MeshInstanceData value{mesh, ids[index]};
            value.render_enabled = true;
            auto matrix = kIdentityTransform;
            if (index == 0)
            {
                // The readback point stays well inside this quad on either backend.
                matrix[0] = matrix[5] = .125F;
            }
            else
            {
                const auto pixel = index - 1;
                matrix[0] = matrix[5] = 1.0F / 64;
                matrix[12] = (2 * float(pixel % 64) + 1) / 64 - 1;
                matrix[13] = 1 - (2 * float(pixel / 64) + 1) / 64;
            }
            input.push_back(value);
            matrices.push_back(matrix);
        }
        bindTransforms(ipc, commands, sceneTransformStorage(ipc, commands, session, scene), input, matrices);
        constexpr unsigned sparse = 8;
        std::size_t previous = 0;
        for (const auto active : {sparse, count})
        {
            const auto created = call<InstanceCommands::CreateInstances>(
                ipc, commands, {instances, std::span<const MeshInstanceData>(input).subspan(previous, active - previous)}
            );
            CHECK(created.instances.size() == active - previous);
            previous = active;
            const auto reads = sampleRaster(server, ipc, commands, session, purposes[0], scene);
            const auto expected_pages = active == count ? pages : (varied ? sparse : 1);
            const auto expected_images = std::min<unsigned>(active, 9);
            CHECK(reads.buffer_ranges == expected_pages * kFif);
            std::uint64_t expected_bytes = 0;
            for (const auto& program : std::span{programs}.first(active)) expected_bytes += program.stride;
            CHECK(reads.buffer_bytes == expected_bytes * kFif);
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
        SceneId scene, ResourceInstanceId materials,
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
        const auto pose = createTransform(ipc, commands, sceneTransformStorage(ipc, commands, session, scene));
        for (auto& instance : live)
        {
            instance.transform_id = pose;
            instance.render_enabled = true;
        }
        call<InstanceCommands::CreateInstances>(ipc, commands, {instances, live});
        const auto sampled = sampleRaster(server, ipc, commands, session, purposes[0], scene);
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
    // Purpose isolation must preserve real ready users even when unused native programs are evicted.
    device.cached_unused_programs = pipeline_isolation.has_value() || count || instance_profile ? 4096 : 0;
    const auto shader_target = device.backend == EGraphicsBackend::D3D12
        ? rdesc::EShaderTarget::DXIL : rdesc::EShaderTarget::SPIRV;
    auto compiler = lux::shader::ShaderCompiler::create();
    CHECK(compiler);
    auto runtime = lux::execution::ExecutionRuntime::create({workers, 128, {128}});
    CHECK(runtime);
    RenderServerConfig config{"test.material.scale", nativeValidationEnabled()};
    config.diagnostics.report = [](void*, const RenderDiagnostic& value) noexcept
    {
        if (value.severity == ERenderDiagnosticSeverity::ERROR_MESSAGE)
            std::fprintf(stderr, "%.*s\n", int(value.message.size()), value.message.data());
    };
    config.frames_in_flight = kFif;
    config.uploads.emplace();
    config.uploads->chunk_bytes = 4 * 1024 * 1024;
    config.uploads->window_regions = 8192;
    auto server = take(RenderServer::create(config, **runtime));
    take(server->registerResource<MaterialAllocationProbe>());
    const bool observe_pipelines = pipeline_isolation.has_value() || (!count && !instance_profile);
    if (observe_pipelines) take(server->registerResource<PipelineIsolationProbe>());
    take(server->registerResource(MaterialReadback::factory()));
    registerMaterialCapture<EMaterialPurpose::FORWARD>(*server);
    registerMaterialCapture<EMaterialPurpose::GBUFFER>(*server);
    registerMaterialCapture<EMaterialPurpose::SHADOW>(*server);
    if (pipeline_isolation.value_or(false))
    {
        const auto hdr = hdrDefinition();
        CHECK(server->registerEffectCapability(hdr));
        registerMaterialNode(*server, hdr.identity.id, clear_hdr, Lighting::hdr, buildHdr<false>);
        registerMaterialNode(*server, hdr.identity.id, sample_hdr, Lighting::hdr, buildHdr<true>);
    }
    auto ipc = ipc::IpcCore::instance().createRequester(config.channel_name);
    CHECK(ipc);
    std::thread thread([&] { CHECK(server->run()); });
    const auto commands = queryCommands(*ipc);
    const auto session = call<CreateDeviceSession>(*ipc, commands, device).device;
    call<GetOrCreateResource>(*ipc, commands, {session, DeviceScope{}, MaterialAllocationProbe::Descriptor::descriptor.type});
    if (observe_pipelines)
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
            *server, thread, *ipc, commands, session, config.channel_name, {scene, other}, materials, mesh,
            images.front(), **compiler, shader_target, *pipeline_isolation, workers, stop_server
        );
    }
    else if (instance_profile)
    {
        CHECK(!count && !varied);
        profileInstances(*server, *ipc, commands, session, scene, materials, instances, mesh, images.front(),
            **compiler, shader_target);
    }
    else if (count)
    {
        verifyScale(*server, *ipc, commands, session, scene, materials, instances, mesh, images, **compiler,
            shader_target, count, varied);
    }
    else
    {
        verifyPurposes(*server, *ipc, commands, session, scene, materials, instances, mesh,
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
