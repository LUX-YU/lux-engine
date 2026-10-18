#include "TextureFixture.hpp"
#include "ParameterFixture.hpp"
#include "extension/NativeBuffer.hpp"
#include "extension/Structured.hpp"
#include "extension/StructuredRecords.hpp"
#include "shaders/Texture.hpp"
#include <lux/engine/render/graph/Builder.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>

namespace
{
    ShaderBlock block()
    {
        static constexpr rdesc::DataGpuField fields[]{
            {"matrix", rdesc::EDataScalar::FLOAT, 0, 4, 4, 0, 16},
            {"colors[0]", rdesc::EDataScalar::FLOAT, 64, 4, 1, 16, 0},
            {"colors[1]", rdesc::EDataScalar::FLOAT, 80, 4, 1, 16, 0}
        };
        return ShaderBlock::from({rdesc::EDataLayout::STD430, 96, 16, fields, {}, {}, 0});
    }
}

int main()
{
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
        extension_test::bufferFactory(), extension_test::TestBufferStorage::Config{8, 96}
    ));
    const auto storage_id = take(session->globalStorages().getOrCreate(
        session->resources().typeId(extension_test::TestBufferStorage::description.type), "wide"));
    auto& storage = *static_cast<extension_test::NativeBuffer*>(session->resources().native(storage_id));
    std::array<float, 24> values{};
    for (unsigned i = 0; i < 4; ++i) values[i * 5] = 1;
    for (unsigned i = 20; i < 24; ++i) values[i] = 0.25f;
    const auto bytes = std::as_bytes(std::span(values));
    auto owner = std::make_shared<const std::vector<std::byte>>(bytes.begin(), bytes.end());
    auto uploaded = take(runUpload(lux::render::upload(storage,
        extension_test::SetBuffer::Request{{storage_id, {}}, payload(owner)}), cpu, *session));
    const auto buffer = uploaded.id;
    auto pixels = std::make_shared<const std::vector<std::byte>>(1024, std::byte{128});
    auto texture_upload = take(runUpload(lux::render::upload(*textures(*session), SetTexture::Request{
        {textures(*session)->id(), {}}, 0, information(16, 1, false), payload(pixels)
    }), cpu, *session));
    const auto texture = texture_upload.texture;

    const TargetDesc target{16, 16, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    RenderGraphBuilder builder;
    const auto image = take(builder.declareResource({"texture", GraphImageDesc{16, 16}, EGraphOrigin::IMPORTED,
        {GraphImageRange{1, 0, 1, 0, 1}}}));
    const auto data = take(builder.declareResource({"data", GraphBufferDesc{96}, EGraphOrigin::IMPORTED,
        {GraphBufferRange{0, 96}}}));
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
        {rdesc::EShaderType::VERTEX, "main", {texture_fixture::vert.begin(), texture_fixture::vert.end()}},
        {
            rdesc::EShaderType::FRAGMENT, "main",
            {extension_test::structured_fragment.begin(), extension_test::structured_fragment.end()}
        }
    };
    draw.shader.interface.bindings = {
        {4, 8, EDescriptorKind::SAMPLED_IMAGE_ARRAY, 0, {}, "textures"},
        {2, 9, EDescriptorKind::STORAGE_BUFFER, 1, block(), "data"}
    };
    draw.resources = {{"textures", "texture"}, {"data", "data"}};
    parameter_fixture::configure(draw, "control", "texture");
    std::array programs{draw, PassProgram{"read"}};
    const std::array arguments{
        StorageArgument{"texture", take(ConfigValue::make(TextureBinding{texture}))},
        StorageArgument{"data", take(ConfigValue::make(extension_test::BufferBinding{buffer}))}
    };
    auto code = std::make_shared<CompiledGraph>(take(compileGraph(
        *session, definition, analysis, bindings, programs, target
    )));
    auto* execution = take(session->prepare(code, target, nullptr, nullptr, arguments));
    CHECK(take(execution->execute()) == EExecutionOutcome::EXECUTED);
    CHECK(take(session->queue().wait(*execution->lastUse(), UINT64_MAX)));
    auto readback = take(execution->readback());
    for (auto byte : readback) CHECK(std::to_integer<unsigned>(byte) == 32);
    for (unsigned fault = 0; fault < 9; ++fault)
    {
        auto bad = programs;
        auto& layout = bad[0].shader.interface.bindings[1].block;
        switch (fault)
        {
        case 0: layout.fields[0].matrix_stride = 8; break;
        case 1: layout.fields[1].array_stride = 32; break;
        case 2: layout.fields[2].offset = 64; break;
        case 3: layout.fields[0].columns = 3; break;
        case 4: layout.fields[1].scalar = rdesc::EDataScalar::UINT; break;
        case 5: layout.fields[0].path = "unknown"; break;
        case 6: layout.fields.pop_back(); break;
        case 7: layout.alignment = 3; break;
        case 8: layout.runtime_records = true; break;
        }
        CHECK(!compileGraph(*session, definition, analysis, bindings, bad, target));
    }
    CHECK(session->release(execution));
    auto records = programs;
    records[0].shader.stages[1].words.assign(
        extension_test::structured_records_fragment.begin(), extension_test::structured_records_fragment.end()
    );
    records[0].shader.interface.bindings[1].block.runtime_records = true;
    auto record_code = std::make_shared<CompiledGraph>(take(compileGraph(
        *session, definition, analysis, bindings, records, target
    )));
    auto* record_execution = take(session->prepare(record_code, target, nullptr, nullptr, arguments));
    CHECK(take(record_execution->execute()) == EExecutionOutcome::EXECUTED);
    CHECK(take(session->queue().wait(*record_execution->lastUse(), UINT64_MAX)));
    for (auto byte : take(record_execution->readback())) CHECK(std::to_integer<unsigned>(byte) == 32);
    records[0].shader.interface.bindings[1].block.size = 112;
    CHECK(!compileGraph(*session, definition, analysis, bindings, records, target));
    CHECK(session->release(record_execution));
    record_code.reset();
    CHECK(storage.remove(buffer));
    CHECK(session->globalStorages().remove(storage_id));
    CHECK(session->drain());
    code.reset();
    session.reset();
    CHECK(validation.errors == 0);
    std::printf("Structured SSBO mat4/vec4[2], relocation, actual upload/draw/readback=32; "
        "runtime record stride=96, ABI negatives=10; validation=0\n");
}
