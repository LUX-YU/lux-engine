#include "TextureFixture.hpp"
#include "ParameterFixture.hpp"
#include "extension/NativeBuffer.hpp"
#include "extension/Parameters.hpp"
#include "extension/Feature.hpp"
#include "Allocation.hpp"
#include "shaders/Texture.hpp"
#include <lux/engine/render/graph/Builder.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <fstream>

static std::vector<std::uint32_t> readSpirv(const char* path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    CHECK(input);
    const auto bytes = input.tellg();
    CHECK(bytes > 0 && bytes % 4 == 0);
    std::vector<std::uint32_t> result(static_cast<std::size_t>(bytes) / 4);
    input.seekg(0);
    CHECK(input.read(reinterpret_cast<char*>(result.data()), bytes));
    return result;
}

static ShaderBlock parameterBlock() noexcept
{
    static constexpr rdesc::DataGpuField fields[]{
        {"control", rdesc::EDataScalar::FLOAT, 0, 4, 1, 0, 0},
        {"view", rdesc::EDataScalar::FLOAT, 16, 4, 1, 0, 0},
        {"gain", rdesc::EDataScalar::FLOAT, 32, 4, 1, 0, 0},
        {"bias", rdesc::EDataScalar::FLOAT, 48, 4, 1, 0, 0}
    };
    return ShaderBlock::from({rdesc::EDataLayout::STD430, 64, 16, fields, {}, {}, 0});
}

int main(int argc, char** argv)
{
    CHECK(argc == 1 || argc == 3);
    Validation validation;
    auto bootstrap = instance(validation);
    DeviceOptions options;
    options.dynamic_rendering = true;
    options.texture_descriptors = true;
    auto session = take(DeviceSession::create(bootstrap, options));
    CpuFixture cpu;
    CHECK(session->configureUploads({65536, 2, 131072}, cpu.cpu()));
    CHECK(configureTextures(*session, 8, 1024 * 1024));
    CHECK(session->resources().installFactory(
        extension_test::bufferFactory(), extension_test::TestBufferStorage::Config{8, 16}
    ));
    const auto storage_id = take(session->globalStorages().getOrCreate(
        session->resources().typeId(extension_test::TestBufferStorage::description.type), "wide"));
    auto& storage = *static_cast<extension_test::NativeBuffer*>(session->resources().native(storage_id));
    std::array<float, 4> values{0.5f, 0.5f, 0.5f, 0.5f};
    const auto bytes = std::as_bytes(std::span(values));
    auto owner = std::make_shared<const std::vector<std::byte>>(bytes.begin(), bytes.end());
    auto uploaded = take(lux::render::upload(storage,
        extension_test::SetBuffer::Request{{storage_id, {}}, payload(owner)}));
    const auto buffer = uploaded.first.id;
    complete(uploaded.second, cpu, *session);
    uploaded.second.reset();
    auto pixels = std::make_shared<const std::vector<std::byte>>(1024, std::byte{128});
    auto texture_upload = take(lux::render::upload(*textures(*session), SetTexture::Request{
        {textures(*session)->id(), {}}, 0, information(16, 1, false), payload(pixels)
    }));
    const auto texture = texture_upload.first.texture;
    complete(texture_upload.second, cpu, *session);
    texture_upload.second.reset();

    const TargetDesc target{16, 16, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    RenderGraphBuilder builder;
    const auto image = take(builder.declareResource({"texture", GraphImageDesc{16, 16}, EGraphOrigin::IMPORTED,
        {GraphImageRange{1, 0, 1, 0, 1}}}));
    const auto data = take(builder.declareResource({"data", GraphBufferDesc{16}, EGraphOrigin::IMPORTED,
        {GraphBufferRange{0, 16}}}));
    const auto color = take(builder.declareResource({"color", GraphImageDesc{16, 16}}));
    const auto result = take(builder.declareResource({"pixels", GraphBufferDesc{1024}}));
    take(builder.addPass({"draw", EGraphPassKind::GRAPHICS,
        {{{image, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::SAMPLED},
         {{data, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::STORAGE_READ},
         {{color, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::COLOR_ATTACHMENT}}}));
    take(builder.addPass({"read", EGraphPassKind::TRANSFER,
        {{{color, 1}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
         {{result, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
    CHECK(builder.exportOutput({{color, 1}, {}, target}));
    CHECK(builder.exportOutput({{result, 1}}));
    auto definition = take(std::move(builder).finish());
    auto analysis = take(analyze(definition));
    GraphStorageBindings bindings;
    bindings.global = {
        {image, {StorageName(kTextureStorageType.name()), "default"}},
        {data, {StorageName(extension_test::TestBufferStorage::description.type.name()), "wide"}}
    };
    PassProgram draw{"draw"};
    draw.shader.stages = {
        {
            rdesc::EShaderType::VERTEX, "main",
            {extension_test::parameters_vertex.begin(), extension_test::parameters_vertex.end()}
        },
        {
            rdesc::EShaderType::FRAGMENT, "main",
            {extension_test::parameters_fragment.begin(), extension_test::parameters_fragment.end()}
        }
    };
    draw.shader.interface.bindings = {
        {4, 8, EDescriptorKind::SAMPLED_IMAGE_ARRAY, 0, {}, "textures"},
        {2, 9, EDescriptorKind::STORAGE_BUFFER, 1, extension_test::colorBlock(), "data", 3}
    };
    draw.resources = {{"textures", "texture"}, {"data", "data"}};
    if (argc == 3)
    {
        draw.shader.stages[0].words = readSpirv(argv[1]);
        draw.shader.stages[1].words = readSpirv(argv[2]);
        draw.shader.interface.bindings[0].set = 0;
        draw.shader.interface.bindings[0].binding = 0;
        draw.shader.interface.bindings[1].set = 0;
        draw.shader.interface.bindings[1].binding = 1;
    }
    draw.shader.interface.parameters = parameterBlock();
    draw.shader.interface.parameter_stages = 3;
    draw.context_parameters = parameter_fixture::float4("tint");
    draw.parameter_values.resize(64);
    draw.parameters = {
        {"control", "texture", "control"},
        {"gain", "data", "gain"},
        {"view", "", "tint", EParameterSource::CONTEXT}
    };
    std::array programs{draw, PassProgram{"read"}};
    const std::array arguments{
        StorageArgument{"texture", take(ConfigValue::make(TextureBinding{texture}))},
        StorageArgument{"data", take(ConfigValue::make(
            extension_test::BufferBinding{buffer, {0.5f, 0.5f, 0.5f, 0.5f}}
        ))}
    };
    auto code = std::make_shared<CompiledGraph>(take(compileGraph(
        *session, definition, analysis, bindings, programs, target
    )));
    auto* a = take(session->prepare(code, target, nullptr, nullptr, arguments));
    auto* b = take(session->prepare(code, target, nullptr, nullptr, arguments));
    const std::array<float, 4> half{0.5f, 0.5f, 0.5f, 0.5f}, one{1, 1, 1, 1};
    CHECK(a->setContextParameters(std::as_bytes(std::span(half))));
    CHECK(b->setContextParameters(std::as_bytes(std::span(one))));
    const auto execute = [&](ExecutionContext& context, unsigned expected) {
        CHECK(take(context.execute()) == EExecutionOutcome::EXECUTED);
        CHECK(take(session->queue().wait(*context.lastUse(), UINT64_MAX)));
        for (auto byte : take(context.readback())) CHECK(std::to_integer<unsigned>(byte) == expected);
    };
    execute(*a, 16);
    execute(*b, 32);
    std::array<float, 16> pass_values{};
    std::fill(pass_values.begin() + 12, pass_values.end(), 0.0625f);
    CHECK(a->setParameters({0}, std::as_bytes(std::span(pass_values))));
    execute(*a, 32); // Constant Pass bias changes, resource/context fields are restored from their sources.
    execute(*b, 32); // Independent Context unchanged.
    pass_values.fill(0);
    CHECK(a->setParameters({0}, std::as_bytes(std::span(pass_values))));
    CHECK(!a->setContextParameters({}));
    CHECK(!a->setParameters({0}, {}));
    CHECK(!a->setParameters({999}, {}));
    execute(*a, 16); // Failed updates preserve last-good values.
    for (unsigned fault = 0; fault < 9; ++fault)
    {
        auto bad = programs;
        if (fault == 0) bad[0].parameters.push_back(bad[0].parameters[0]);
        if (fault == 1) bad[0].parameters[0].shader_field = "absent";
        if (fault == 2) bad[0].parameters[0].resource_key = "absent";
        if (fault == 3) bad[0].parameter_values.pop_back();
        if (fault == 4) bad[0].shader.interface.parameter_stages = 0;
        if (fault == 5) bad[0].shader.interface.parameters.fields[2].offset = 16;
        if (fault == 6) bad[0].parameters[0].source_field = "absent";
        if (fault == 7) bad[0].context_parameters.fields[0].scalar = rdesc::EDataScalar::UINT;
        if (fault == 8) bad[0].shader.interface.parameter_stages = 2;
        auto compiled = compileGraph(*session, definition, analysis, bindings, bad, target);
        if (fault < 6 || fault == 8) CHECK(!compiled);
        else
        {
            CHECK(compiled);
            auto candidate = std::make_shared<CompiledGraph>(std::move(*compiled));
            CHECK(!session->prepare(candidate, target, nullptr, nullptr, arguments));
        }
    }
    render_allocation::participate = true;
    render_allocation::calls = 0;
    render_allocation::window = true;
    for (unsigned i = 0; i < 20; ++i)
    {
        CHECK(a->setContextParameters(std::as_bytes(std::span(i % 2 ? half : one))));
        CHECK(take(a->execute()) == EExecutionOutcome::EXECUTED);
        CHECK(take(session->queue().wait(*a->lastUse(), UINT64_MAX)));
    }
    render_allocation::window = false;
    CHECK(render_allocation::calls == 0);
    CHECK(session->release(a));
    CHECK(session->release(b));
    {
        RenderGraphBuilder builder;
        const auto color = take(builder.declareResource({"color", GraphImageDesc{16, 16}}));
        const auto pixels = take(builder.declareResource({"pixels", GraphBufferDesc{1024}}));
        CHECK(builder.addPass({"draw", EGraphPassKind::GRAPHICS,
            {{{color, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::COLOR_ATTACHMENT}}}));
        CHECK(builder.addPass({"read", EGraphPassKind::TRANSFER,
            {{{color, 1}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
             {{pixels, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
        CHECK(builder.exportOutput({{color, 1}, {}, target}));
        CHECK(builder.exportOutput({{pixels, 1}}));
        const auto graph = take(std::move(builder).finish());
        const auto analysis = take(analyze(graph));
        PassProgram draw{"draw"};
        draw.shader.stages = {
            {rdesc::EShaderType::VERTEX, "main", {texture_fixture::vert.begin(), texture_fixture::vert.end()}},
            {rdesc::EShaderType::FRAGMENT, "main",
                {extension_test::parameters_none.begin(), extension_test::parameters_none.end()}}
        };
        const std::array programs{draw, PassProgram{"read"}};
        auto compiled = std::make_shared<CompiledGraph>(take(compileGraph(
            *session, graph, analysis, {}, programs, target
        )));
        auto* none = take(session->prepare(compiled, target));
        CHECK(none->contextParameters().empty() && none->parameters({0}).empty());
        execute(*none, 64);
        CHECK(session->release(none));
    }
    CHECK(storage.remove(buffer));
    CHECK(session->globalStorages().remove(storage_id));
    CHECK(session->drain());
    code.reset();
    session.reset();
    CHECK(validation.errors == 0);
    std::printf("64B parameters: Texture + Buffer + Context, pixels16/32; rejection/last-good; hot new=0\n");
}
