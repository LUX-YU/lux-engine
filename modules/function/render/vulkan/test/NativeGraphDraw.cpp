#include "Allocation.hpp"
#include "F4CArguments.pass.hpp"
#include "F4CIndexed.pass.hpp"
#include "F4CIndexedIndirect.pass.hpp"
#include "F4CIndirect.pass.hpp"
#include "F4CVertex.pass.hpp"
#include "F4CopyBuffer.pass.hpp"
#include "F4Readback.pass.hpp"
#include "NativeGraphSupport.hpp"

using namespace native_graph_test;

template <class Params>
void drawCase(
    const std::filesystem::path& shaders,
    Validation& validation,
    const char* name,
    VkIndexType index_type = VK_INDEX_TYPE_UINT16
)
{
    constexpr bool indexed = requires(Params p) { p.indices; };
    constexpr bool indirect = requires(Params p) { p.arguments; };
    auto host = instance(validation);
    DeviceOptions options;
    options.dynamic_rendering = true;
    options.multiple_queues = true;
    auto device = checked(VulkanDevice::create(host, options));
    auto allocator = checked(VulkanAllocator::create(device));
    auto queue = checked(SubmissionQueue::create(device, 32));
    auto compute = checked(SubmissionQueue::create(device, 32, EQueueRole::COMPUTE));
    auto transfer = checked(SubmissionQueue::create(device, 32, EQueueRole::TRANSFER));
    std::printf(
        "DEVICE %s driver=%u API=%u graphics=%u compute=%u transfer=%u\n",
        device.properties().deviceName,
        device.properties().driverVersion,
        device.properties().apiVersion,
        queue->nativeQueue().family,
        compute->nativeQueue().family,
        transfer->nativeQueue().family
    );
    auto vertices = checked(Buffer::create(allocator, 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, EMemoryAccess::UPLOAD));
    const unsigned index_bytes = index_type == VK_INDEX_TYPE_UINT32 ? 4u : 2u;
    auto indices =
        checked(Buffer::create(allocator, 8 * index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, EMemoryAccess::UPLOAD));
    auto initial = checked(Buffer::create(allocator, 32, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, EMemoryAccess::UPLOAD));
    const std::array<float, 12> geometry{0, 0, 20, 20, -1, -1, 3, -1, -1, 3, 0, 0};
    const std::array<std::uint16_t, 8> elements{0, 0, 1, 2, 3, 0, 0, 0};
    std::array<unsigned, 8> zeros{};
    CHECK(vertices.write(0, std::as_bytes(std::span{geometry})));
    if (index_type == VK_INDEX_TYPE_UINT32)
    {
        const std::array<std::uint32_t, 8> wide_elements{0, 0, 1, 2, 3, 0, 0, 0};
        CHECK(indices.write(0, std::as_bytes(std::span{wide_elements})));
    }
    else
    {
        CHECK(indices.write(0, std::as_bytes(std::span{elements})));
    }
    std::printf("INDEX element_bytes=%u offset=%u count=3 values=1,2,3\n", index_bytes, 2 * index_bytes);
    CHECK(initial.write(0, std::as_bytes(std::span{zeros})));
    RenderGraphBuilder builder;
    auto vertex = builder.importBuffer("draw.vertices", {48, 4}, EPersistentScope::VIEW);
    std::vector<NativeImportBinding> native;
    std::vector<GraphImportBinding> imports;
    const auto import = [&](GraphBuffer graph, const Buffer& buffer, unsigned identity)
    {
        native.push_back(
            {GraphResourceId{graph.value()},
             GraphBackingId{identity},
             std::cref(buffer),
             {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED},
             {},
             true}
        );
        imports.push_back({GraphResourceId{graph.value()}, GraphBackingId{identity}, 0, 1});
    };
    import(vertex, vertices, 1);
    Params params;
    params.vertices = {vertex, {8, 32}};
    if constexpr (indexed)
    {
        auto index = builder.importBuffer("draw.indices", {8 * index_bytes, index_bytes}, EPersistentScope::VIEW);
        params.indices = {index, {2 * index_bytes, 3 * index_bytes}};
        import(index, indices, 2);
    }
    std::vector<NativePassCommand> commands;
    GraphPassId argument_pass;
    if constexpr (indirect)
    {
        auto seed = builder.importBuffer("draw.seed", {32, 4}, EPersistentScope::VIEW);
        import(seed, initial, 3);
        auto arguments = builder.buffer({32, 4}, "draw.arguments");
        F4CopyBuffer copy;
        copy.source = {seed, {0, 32}};
        copy.destination = {arguments, {0, 32}};
        auto init = checked(builder.transfer("draw.init", EExecutionScope::VIEW, copy));
        commands.push_back({init, CopyCommand{}, EQueueRole::TRANSFER});
        F4CArguments producer;
        producer.output = {arguments, {0, 32}};
        producer.indexed = indexed;
        argument_pass =
            checked(builder.compute("draw.generate", {{"F4CArguments", "default"}}, EExecutionScope::VIEW, producer));
        CHECK(builder.source(passKey("draw.generate"), "output", PassProducer{passKey("draw.init")}));
        commands.push_back({argument_pass, DispatchCommand{}, EQueueRole::COMPUTE});
        params.arguments = {arguments, {0, 32}};
    }
    TextureDesc texture;
    texture.width = texture.height = 4;
    params.output.texture = builder.texture(texture, "draw.color");
    params.output.load = ELoadOp::CLEAR;
    auto pass = checked(builder.graphics("draw.geometry", {{name, "default"}}, EExecutionScope::VIEW, params));
    if constexpr (indirect)
    {
        CHECK(builder.source(passKey("draw.geometry"), "arguments", PassProducer{passKey("draw.generate")}));
    }
    F4Readback read;
    read.source.texture = params.output.texture;
    read.destination.buffer = builder.buffer({256, 4}, "draw.pixels");
    auto read_pass = checked(builder.transfer("draw.read", EExecutionScope::VIEW, read));
    CHECK(builder.source(passKey("draw.read"), "source", PassProducer{passKey("draw.geometry")}));
    CHECK(builder.exportBuffer(read.destination.buffer, PassProducer{passKey("draw.read")}, EGraphOutput::READBACK));
    auto definition = checked(std::move(builder).finish());
    auto logical = checked(compileLogicalGraph(definition));
    auto invocation = makeGraphInvocationData(definition);
    const auto& uses = logical.identity().passes[pass.value() - 1].uses;
    const auto use = [&](EGraphUsage role)
    {
        for (unsigned i = 0; i < uses.size(); ++i)
        {
            if (uses[i].usage == role)
            {
                return i;
            }
        }
        CHECK(false);
        return 0u;
    };
    VNativeCommand draw;
    if constexpr (indexed && indirect)
    {
        draw = DrawIndexedIndirectCommand{
            {use(EGraphUsage::INDEX), index_type, 0},
            {use(EGraphUsage::INDIRECT), 0, 20, 1},
            {{0, use(EGraphUsage::VERTEX), 0}}
        };
    }
    else if constexpr (indirect)
    {
        draw = DrawIndirectCommand{{use(EGraphUsage::INDIRECT), 0, 16, 1}, {{0, use(EGraphUsage::VERTEX), 0}}};
    }
    else if constexpr (indexed)
    {
        draw = DrawIndexedCommand{
            {use(EGraphUsage::INDEX), index_type, 0},
            3,
            1,
            0,
            0,
            0,
            {{0, use(EGraphUsage::VERTEX), 0}}
        };
    }
    else
    {
        draw = DrawCommand{3, 1, 1, 0, {{0, use(EGraphUsage::VERTEX), 0}}};
    }
    const auto draw_index = commands.size();
    commands.push_back({pass, draw, EQueueRole::GRAPHICS});
    commands.push_back({read_pass, CopyCommand{}, EQueueRole::GRAPHICS});
    GraphicsDescription graphics;
    graphics.color_formats = {VK_FORMAT_R32G32B32A32_SFLOAT};
    graphics.blends.resize(1);
    graphics.vertex_bindings = {{0, 8, VK_VERTEX_INPUT_RATE_VERTEX}};
    graphics.attributes = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}};
    // Device-budget rejection uses the real existing pipeline owner, before native creation.
    {
        auto vertex_module = checked(ShaderModule::create(device, words(shaders / (std::string(name) + ".vert.spv"))));
        auto fragment_module =
            checked(ShaderModule::create(device, words(shaders / (std::string(name) + ".frag.spv"))));
        auto layout = checked(PipelineLayout::create(device, {}));
        auto invalid = graphics;
        invalid.vertex_bindings[0].stride = device.properties().limits.maxVertexInputBindingStride + 1;
        const auto over_limit = GraphicsPipeline::create(device, vertex_module, fragment_module, layout, invalid);
        CHECK(!over_limit && over_limit.error().type == kInvalidArgument);
        std::puts("REJECT physical-device-vertex-stride-limit");
    }
    auto build = [&](unsigned capacity)
    {
        std::vector<NativeShaderProgram> programs;
        if constexpr (indirect)
        {
            programs.push_back(program(
                device,
                logical,
                argument_pass,
                "F4CArguments",
                PassSchema<F4CArguments>::contract(),
                shaders,
                true
            ));
        }
        programs.push_back(
            program(device, logical, pass, name, PassSchema<Params>::contract(), shaders, false, 0, &graphics)
        );
        return compileFixture(
            {logical,
             device,
             allocator,
             {queue.get(), compute.get(), transfer.get()},
             commands,
             native,
             invocation,
             {},
             capacity},
            std::move(programs)
        );
    };
    const auto reject = [&](const char* reason)
    {
        auto bad = build(2);
        CHECK(!bad && bad.error().type == kInvalidArgument);
        std::printf("REJECT %s\n", reason);
        commands[draw_index].command = draw;
    };
    const auto mutate = [&](auto fn)
    {
        std::visit(
            [&](auto& c)
            {
                if constexpr (requires { c.streams; })
                {
                    fn(c);
                }
            },
            commands[draw_index].command
        );
    };
    mutate([&](auto& c) { c.streams[0].use_index = use(EGraphUsage::COLOR_ATTACHMENT); });
    reject("wrong-kind-use");
    mutate([&](auto& c) { c.streams[0].use_index = 999; });
    reject("missing-use");
    mutate([&](auto& c) { c.streams[0].offset = 1; });
    reject("vertex-misaligned");
    mutate([&](auto& c) { c.streams[0].offset = 32; });
    reject("vertex-range");
    mutate([&](auto& c) { c.streams[0].binding = 1; });
    reject("binding-not-in-pipeline");
    if constexpr (indexed)
    {
        mutate(
            [](auto& c)
            {
                if constexpr (requires { c.indices; })
                {
                    c.indices.offset = 1;
                }
            }
        );
        reject("index-offset");
        mutate(
            [](auto& c)
            {
                if constexpr (requires { c.indices; })
                {
                    c.indices.type = VK_INDEX_TYPE_UINT8_EXT;
                }
            }
        );
        reject("index-type-not-enabled");
        mutate(
            [&](auto& c)
            {
                if constexpr (requires { c.indices; })
                {
                    c.indices.use_index = use(EGraphUsage::VERTEX);
                }
            }
        );
        reject("wrong-index-usage");
        if constexpr (!indirect)
        {
            std::get<DrawIndexedCommand>(commands[draw_index].command).indices_count = 4;
            reject("index-count");
        }
    }
    if constexpr (indirect)
    {
        mutate(
            [](auto& c)
            {
                if constexpr (requires { c.arguments; })
                {
                    c.arguments.offset = 2;
                }
            }
        );
        reject("indirect-offset");
        mutate(
            [](auto& c)
            {
                if constexpr (requires { c.arguments; })
                {
                    c.arguments.offset = 28;
                }
            }
        );
        reject("indirect-range");
        mutate(
            [](auto& c)
            {
                if constexpr (requires { c.arguments; })
                {
                    c.arguments.stride = 3;
                }
            }
        );
        reject("indirect-stride");
        mutate(
            [](auto& c)
            {
                if constexpr (requires { c.arguments; })
                {
                    c.arguments.count = 2;
                }
            }
        );
        reject("multidraw-not-enabled");
        mutate(
            [&](auto& c)
            {
                if constexpr (requires { c.arguments; })
                {
                    c.arguments.use_index = use(EGraphUsage::VERTEX);
                }
            }
        );
        reject("wrong-indirect-usage");
    }
    else if constexpr (!indexed)
    {
        std::get<DrawCommand>(commands[draw_index].command).vertices = 4;
        reject("vertex-count");
    }
    graphics.vertex_bindings[0].stride = 7;
    reject("vertex-stride-alignment");
    graphics.vertex_bindings[0].stride = 8;
    for (unsigned capacity : {2u, 3u})
    {
        const auto start = std::chrono::steady_clock::now();
        auto plan = checked(build(capacity));
        const auto cold =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        {
            imports[0].dynamic_offset = 2;
            auto invalid_frame = checked(FrameGraphBindings::create(plan.plan.logical(), {}, imports, invocation));
            auto bad = executeGraph(plan.invocations, invalid_frame.frame().frame_slot, invalid_frame, native);
            CHECK(!bad && bad.error().type == kInvalidArgument);
            imports[0].dynamic_offset = 0;
            auto frame = checked(FrameGraphBindings::create(plan.plan.logical(), {}, imports, invocation));
            auto wrong_usage =
                checked(Buffer::create(allocator, 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, EMemoryAccess::UPLOAD));
            native[0].backing = std::cref(wrong_usage);
            bad = executeGraph(plan.invocations, frame.frame().frame_slot, frame, native);
            CHECK(!bad && bad.error().type == kInvalidArgument);
            native[0].backing = std::cref(vertices);
            std::puts("REJECT invocation-offset native-vertex-usage");
        }
        std::vector<NativeTraceEvent> trace(plan.invocations.traceCapacity());
        std::array<std::uint64_t, 24> times{};
        std::array<float, 64> pixels{};
        bool saw_vertex = false, saw_index = !indexed, saw_indirect = !indirect;
        for (unsigned serial = 0; serial < 27; ++serial)
        {
            GraphFrameValues values;
            values.frame_serial = serial;
            auto frame = checked(FrameGraphBindings::create(plan.plan.logical(), values, imports, invocation));
            if (serial >= 3)
            {
                allocation_count = 0;
                allocation_bytes = 0;
                measuring = true;
            }
            const auto begin = std::chrono::steady_clock::now();
            auto submitted = executeGraph(plan.invocations, frame.frame().frame_slot, frame, native, trace);
            const auto ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
            measuring = false;
            auto receipt = checked(std::move(submitted));
            if (serial >= 3)
            {
                CHECK(allocation_count == 0 && allocation_bytes == 0);
                times[serial - 3] = ns;
            }
            CHECK(checked(queue->wait(receipt.completion, UINT64_MAX)));
            CHECK(plan.invocations.readback(
                receipt,
                GraphResourceId{read.destination.buffer.value()},
                0,
                std::as_writable_bytes(std::span{pixels})
            ));
            for (unsigned pixel = 0; pixel < 16; ++pixel)
            {
                CHECK(
                    pixels[pixel * 4] == 1 && pixels[pixel * 4 + 1] == 0 && pixels[pixel * 4 + 2] == 0 &&
                    pixels[pixel * 4 + 3] == 1
                );
            }
            for (const auto& event : std::span{trace}.first(receipt.trace_count))
            {
                saw_vertex |= (event.after.access & VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT) != 0;
                saw_index |= (event.after.access & VK_ACCESS_2_INDEX_READ_BIT) != 0;
                saw_indirect |= (event.after.access & VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT) != 0 &&
                                (event.before.access & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT) != 0;
            }
            if (serial == 0)
            {
                std::puts(nativeTraceJson(std::span{trace}.first(receipt.trace_count)).c_str());
            }
        }
        CHECK(saw_vertex && saw_index && saw_indirect);
        std::sort(times.begin(), times.end());
        std::printf(
            "DRAW %s slots=%u pixels=PASS vertex_index_indirect_sync=PASS allocations=0 bytes=0 cold_ns=%lld p50=%llu "
            "p95=%llu max=%llu\n",
            name,
            capacity,
            cold,
            times[12],
            times[22],
            times[23]
        );
    }
}

int main(int argc, char** argv)
{
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    CHECK(argc == 3);
    Validation validation;
    std::string mode = argv[1];
    if (mode == "vertex")
    {
        drawCase<F4CVertex>(argv[2], validation, "F4CVertex");
    }
    else if (mode == "indexed")
    {
        drawCase<F4CIndexed>(argv[2], validation, "F4CIndexed");
    }
    else if (mode == "indexed-u32")
    {
        drawCase<F4CIndexed>(argv[2], validation, "F4CIndexed", VK_INDEX_TYPE_UINT32);
    }
    else if (mode == "indirect")
    {
        drawCase<F4CIndirect>(argv[2], validation, "F4CIndirect");
    }
    else
    {
        CHECK(mode == "indexed-indirect");
        drawCase<F4CIndexedIndirect>(argv[2], validation, "F4CIndexedIndirect");
    }
    std::printf("VALIDATION errors=%u warnings=%u\n", validation.errors.load(), validation.warnings.load());
    CHECK(validation.errors == 0);
}
