#include "TextureFixture.hpp"
#include <lux/engine/render/features/MeshFeature.hpp>
#include <lux/engine/render/native/vulkan/instance/InstanceStorage.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshStorage.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialStorage.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/render/graph/Builder.hpp>
#include <fstream>

static unsigned pipelines = 0;

namespace lux::render::vulkan::test
{
    VkResult before(const char*) noexcept { return VK_SUCCESS; }
    VkResult after(const char* call, VkResult result) noexcept
    {
        if (result == VK_SUCCESS && std::string_view(call) == "pipeline") ++pipelines;
        return result;
    }
    void destroyed(const char*) noexcept {}
}

static std::vector<std::uint32_t> spirv(const char* path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    CHECK(input);
    const auto bytes = input.tellg();
    CHECK(bytes > 0 && bytes % 4 == 0);
    std::vector<std::uint32_t> result(std::size_t(bytes) / 4);
    input.seekg(0);
    CHECK(input.read(reinterpret_cast<char*>(result.data()), bytes));
    return result;
}

static void pixel(std::span<const std::byte> bytes, unsigned x, unsigned y, std::array<unsigned, 4> expected)
{
    for (unsigned c = 0; c < 4; ++c)
        CHECK(std::to_integer<unsigned>(bytes[(y * 64 + x) * 4 + c]) == expected[c]);
}

int main(int argc, char** argv)
{
    CHECK(argc == 4);
    Validation validation;
    CpuFixture host;
    for (unsigned topology = 0; topology < 3; ++topology)
    {
        auto bootstrap = instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        options.multiple_queues = topology != 0;
        options.prefer_dedicated_queues = topology == 2;
        auto session = take(DeviceSession::create(bootstrap, options));
        CHECK(session->configureUploads({65536, 2, 2 * 1024 * 1024}, host.cpu()));
        auto& registry = session->resources();
        CHECK(registry.installFactory(meshStorageFactory(), MeshStorageConfig{}));
        CHECK(registry.installFactory(materialStorageFactory(), MaterialStorageConfig{}));
        CHECK(registry.installFactory(instanceStorageFactory(), InstanceStorageConfig{}));
        auto& globals = session->globalStorages();
        const auto mesh_id = take(globals.getOrCreate(registry.typeId(kMeshStorageType), "mesh"));
        const auto material_id = take(globals.getOrCreate(registry.typeId(kMaterialStorageType), "material"));
        const auto instance_id = take(globals.getOrCreate(registry.typeId(kInstanceStorageType), "instances"));
        auto& meshes = *take(registry.borrow<lux::render::vulkan::MeshStorage>(mesh_id));
        auto& materials = *take(registry.borrow<lux::render::vulkan::MaterialStorage>(material_id));
        auto& instances = *take(registry.borrow<lux::render::vulkan::InstanceStorage>(instance_id));
        auto source = std::make_shared<std::vector<std::byte>>(60);
        const std::array<float, 12> vertices{-.2f, -.2f, .5f, 1, .2f, -.2f, .5f, 1, 0, .2f, .5f, 1};
        const std::array<std::uint32_t, 3> indices{0, 1, 2};
        std::memcpy(source->data(), vertices.data(), sizeof(vertices));
        std::memcpy(source->data() + 48, indices.data(), sizeof(indices));
        MeshWrite mesh_request{{mesh_id, {}}, 0,
            {16, 3, 48, 4, 3, {{0, 0, rdesc::ETextureFormat::RGBA32_SFLOAT}}, {{0, 3, 0}}}, payload(source)};
        auto mesh_work = take(lux::render::upload(meshes, mesh_request));
        complete(mesh_work.second, host, *session);
        const auto mesh = mesh_work.first.mesh;
        mesh_work.second.reset();
        MaterialProgram material_program{{rdesc::EShaderType::FRAGMENT, "main", spirv(argv[3])}, {}, 1};
        auto material_config = take(ConfigValue::make(std::move(material_program)));
        MaterialWrite material_request{{material_id, {}}, 0, material_config};
        auto material_work = take(lux::render::upload(materials, material_request));
        complete(material_work.second, host, *session);
        const auto material = material_work.first.material;
        material_work.second.reset();
        InstanceDescription input{mesh, material};
        input.transform[12] = -.45f;
        const auto left = take(instances.create(input));
        input.transform[12] = .45f;
        auto right = take(instances.create(input));
        const TargetDesc target{64, 64, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
        RenderGraphBuilder builder;
        const auto geometry = take(builder.declareResource({"mesh", GraphBufferDesc{60}, EGraphOrigin::IMPORTED,
            {GraphBufferRange{0, 60}}}));
        const auto matrices = take(builder.declareResource({"matrices", GraphBufferDesc{128}, EGraphOrigin::IMPORTED,
            {GraphBufferRange{0, 128}}}));
        const auto members = take(builder.declareResource({"members", GraphBufferDesc{28}, EGraphOrigin::IMPORTED,
            {GraphBufferRange{0, 28}}}));
        const auto color = take(builder.declareResource({"color", GraphImageDesc{64, 64}}));
        const auto depth = take(builder.declareResource({
            "depth", GraphImageDesc{64, 64, 1, 1, rdesc::ETextureFormat::D32_SFLOAT}
        }));
        const auto output = take(builder.declareResource({"pixels", GraphBufferDesc{64 * 64 * 4}}));
        CHECK(builder.addPass({"draw", EGraphPassKind::GRAPHICS,
            {{{geometry, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::VERTEX},
             {{geometry, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::INDEX},
             {{matrices, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::STORAGE_READ},
             {{members, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::STORAGE_READ},
             {{members, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::INDIRECT},
             {{color, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::COLOR_ATTACHMENT},
             {{depth, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::DEPTH_ATTACHMENT}}}));
        CHECK(builder.addPass({"read", EGraphPassKind::TRANSFER,
            {{{color, 1}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
             {{output, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
        CHECK(builder.exportOutput({{color, 1}, {}, target}));
        CHECK(builder.exportOutput({{output, 1}}));
        auto graph = take(std::move(builder).finish());
        auto analysis = take(analyze(graph));
        PassProgram draw{"draw"};
        draw.shader.stages = {{rdesc::EShaderType::VERTEX, "main", spirv(argv[1])},
            {rdesc::EShaderType::FRAGMENT, "main", spirv(argv[2])}};
        ShaderBlock transform{64, 16}, word{4, 4};
        transform.runtime_records = word.runtime_records = true;
        transform.fields = {{"model", rdesc::EDataScalar::FLOAT, 0, 4, 4, 0, 16}};
        word.fields = {{"value", rdesc::EDataScalar::UINT, 0, 1, 1, 0, 0}};
        draw.shader.interface.bindings = {
            {3, 7, EDescriptorKind::STORAGE_BUFFER, 1, transform, "transforms", 1},
            {2, 6, EDescriptorKind::STORAGE_BUFFER, 1, word, "members", 1}
        };
        draw.shader.interface.parameters = draw.context_parameters = meshViewParameterBlock();
        draw.shader.interface.parameter_stages = 1;
        draw.parameter_values.resize(128);
        draw.parameters = {{"view", "", "view", EParameterSource::CONTEXT},
            {"projection", "", "projection", EParameterSource::CONTEXT}};
        draw.resources = {{"transforms", "matrices"}, {"members", "members"}};
        draw.graphics.vertex_bindings = {{0, 16}};
        draw.graphics.vertex_attributes = {{0, 0, 0, rdesc::ETextureFormat::RGBA32_SFLOAT}};
        draw.graphics.depth_test = draw.graphics.depth_write = true;
        draw.vertex_buffers = {{"mesh", 0, 0}};
        draw.draw = DrawIndexedIndirect{"mesh", 48, EIndexType::UINT32, "members"};
        draw.clear_color = {0, 0, 0, 1};
        const std::array programs{draw, PassProgram{"read"}};
        GraphStorageBindings bindings;
        bindings.global = {{geometry, {StorageName(kMeshStorageType.name()), "mesh"}},
            {matrices, {StorageName(kInstanceStorageType.name()), "instances"}},
            {members, {StorageName(kInstanceStorageType.name()), "instances"}}};
        const auto before_pipeline = pipelines;
        auto code = std::make_shared<CompiledGraph>(take(compileGraph(
            *session, graph, analysis, bindings, programs, target)));
        CHECK(pipelines == before_pipeline + 1);
        const InstanceDrawGroup group{mesh, material};
        const std::array arguments{
            StorageArgument{"mesh", take(ConfigValue::make(MeshBinding{mesh, mesh_request.description}))},
            StorageArgument{"matrices", take(ConfigValue::make(InstanceBinding{2, group}))},
            StorageArgument{"members", take(ConfigValue::make(InstanceBinding{2, group, EInstanceBinding::DRAW}))}
        };
        auto* first = take(session->prepare(code, target, nullptr, nullptr, arguments));
        auto* second = take(session->prepare(code, target, nullptr, nullptr, arguments));
        const auto resident = instances.residentBytes();
        auto bad_arguments = arguments;
        bad_arguments[2].value = take(ConfigValue::make(InstanceBinding{2, {}, EInstanceBinding::DRAW}));
        CHECK(!session->prepare(code, target, nullptr, nullptr, bad_arguments));
        CHECK(instances.residentBytes() == resident);
        MeshViewParameters camera;
        CHECK(first->setContextParameters(meshViewParameterBytes(camera)));
        camera.view[13] = .4f;
        CHECK(second->setContextParameters(meshViewParameterBytes(camera)));
        const auto render = [&](ExecutionContext& context) {
            CHECK(take(context.execute()) == EExecutionOutcome::EXECUTED);
            CHECK(take(session->queue().wait(*context.lastUse(), UINT64_MAX)));
            return take(context.readback());
        };
        auto image = render(*first);
        pixel(image, 17, 30, {255, 0, 0, 255});
        pixel(image, 46, 30, {0, 255, 0, 255});
        image = render(*second);
        pixel(image, 17, 43, {255, 0, 0, 255});
        pixel(image, 46, 43, {0, 255, 0, 255});
        pixel(image, 17, 30, {0, 0, 0, 255});
        // The explicitly declared two-slot Graph has no room for a third logical instance.
        const auto full = instances.create(input);
        CHECK(!full && full.error().type == kStorageCapacity);
        CHECK(take(instances.query(right.instance)).published.revision == right.revision);
        CHECK(instances.release(right));
        image = render(*first);
        pixel(image, 17, 30, {255, 0, 0, 255});
        pixel(image, 46, 30, {0, 0, 0, 255});
        right = take(instances.create(input));
        image = render(*first);
        pixel(image, 46, 30, {0, 255, 0, 255});
        TransformBatch batch{instance_id, 7, {{right.instance, right.revision, input.transform}}};
        batch.updates[0].transform[12] = .15f;
        CHECK(instances.update(batch));
        ++right.revision;
        image = render(*first);
        pixel(image, 37, 30, {0, 255, 0, 255});
        pixel(image, 46, 30, {0, 0, 0, 255});
        CHECK(session->release(first));
        image = render(*second);
        pixel(image, 37, 43, {0, 255, 0, 255});
        CHECK(instances.release(left));
        CHECK(instances.release(right));
        CHECK(session->release(second));
        CHECK(globals.remove(instance_id));
        CHECK(globals.remove(mesh_id));
        CHECK(globals.remove(material_id));
        CHECK(session->drain());
        CHECK(pipelines == before_pipeline + 1);
    }
    CHECK(validation.errors == 0);
    std::printf("Real indexed-indirect shared instances, two Cameras, add/remove/update with one Graph; warnings=%u\n",
        validation.warnings.load());
}
