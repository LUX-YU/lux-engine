#include "TextureFixture.hpp"
#include <lux/engine/render/native/vulkan/features/MeshFeature.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/render/native/vulkan/instance/InstanceStorage.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshStorage.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialStorage.hpp>
#include <lux/engine/render/data/Camera.hpp>
#include <fstream>
#include <source_location>

template <class T>
static T checked(RenderResult<T> result, std::source_location location = std::source_location::current())
{
    if (!result) std::fprintf(stderr, "Mesh feature result at line %u\n", location.line());
    return take(std::move(result));
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
    for (unsigned channel = 0; channel < 4; ++channel)
        CHECK(std::abs(int(std::to_integer<unsigned>(bytes[(y * 64 + x) * 4 + channel])) -
            int(expected[channel])) <= 1);
}

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    Validation validation;
    CpuFixture host;
    for (unsigned topology = 0; topology < 3; ++topology)
    {
        auto bootstrap = instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        options.multiple_queues = topology != 0;
        options.prefer_dedicated_queues = topology == 2;
        auto session = checked(DeviceSession::create(bootstrap, options));
        CHECK(session->configureUploads({65536, 2, 2 * 1024 * 1024}, host.cpu()));
        auto& registry = session->resources();
        CHECK(registry.installFactory(meshStorageFactory(), MeshStorageConfig{}));
        CHECK(registry.installFactory(materialStorageFactory(), MaterialStorageConfig{}));
        CHECK(registry.installFactory(instanceStorageFactory(), InstanceStorageConfig{}));
        auto& global = session->globalStorages();
        StorageSet scene(registry);
        const auto mesh_storage = checked(global.prepare(registry.typeId(kMeshStorageType), "meshes"));
        const auto material_storage = checked(global.prepare(registry.typeId(kMaterialStorageType), "materials"));
        const auto instance_storage = checked(scene.prepare(registry.typeId(kInstanceStorageType), "instances"));
        auto& meshes = *take(registry.borrow<vulkan::MeshStorage>(mesh_storage));
        auto& materials = *take(registry.borrow<vulkan::MaterialStorage>(material_storage));
        auto& instances = *take(registry.borrow<vulkan::InstanceStorage>(instance_storage));
        // A tetrahedron: positions vary in all three axes; real indexed input is shared by both instances.
        const std::array<float, 20> vertices{
            -.22f, -.2f, .3f, 0, 0, .22f, -.2f, .3f, 1, 0,
            0, .23f, .3f, .5f, 1, 0, 0, .8f, .5f, .5f
        };
        const std::array<std::uint32_t, 12> indices{0, 1, 2, 1, 0, 3, 2, 1, 3, 0, 2, 3};
        auto source = std::make_shared<std::vector<std::byte>>(sizeof(vertices) + sizeof(indices));
        std::memcpy(source->data(), vertices.data(), sizeof(vertices));
        std::memcpy(source->data() + sizeof(vertices), indices.data(), sizeof(indices));
        MeshWrite mesh_input{{mesh_storage, {}}, 0,
            {20, 4, sizeof(vertices), 4, 12,
                {{0, 0, rdesc::ETextureFormat::RGB32_SFLOAT}, {1, 12, rdesc::ETextureFormat::RG32_SFLOAT}},
                {{0, 12, 0}}}, payload(source)};
        auto mesh_work = checked(lux::render::upload(meshes, mesh_input));
        complete(mesh_work.second, host, *session);
        const auto mesh = mesh_work.first.mesh;
        mesh_work.second.reset();
        MaterialProgram program{{rdesc::EShaderType::FRAGMENT, "main", spirv(argv[1])}, {}, 1};
        auto cooked = checked(ConfigValue::make(std::move(program)));
        MaterialWrite material_input{{material_storage, {}}, 0, cooked};
        material_input.parameters[0] = {1, 0, 0, 1};
        auto material_work = checked(lux::render::upload(materials, material_input));
        complete(material_work.second, host, *session);
        auto material = material_work.first;
        material_work.second.reset();
        InstanceDescription input{mesh, material.material};
        input.transform[12] = -.45f;
        const auto left = checked(instances.create(input));
        input.transform[12] = .45f;
        const auto right = checked(instances.create(input));
        MeshFeatureConfig config;
        config.target = {64, 64, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
        config.mesh = {mesh, mesh_input.description};
        config.material = {material.material, cooked};
        config.instances = {2, InstanceDrawGroup{mesh, material.material}};
        config.readback = true;
        const auto compile = [&]<class Feature>() {
            const auto metadata = vulkan::FeatureProvider<Feature>{}.definition();
            auto value = checked(ConfigValue::make(config));
            const FeatureInstanceView selected{metadata.descriptor, metadata.graph, &value, "main"};
            auto assembled = checked(assembleFeatures({&selected, 1}, 1024 * 1024));
            auto contribution = checked(metadata.programs(value.data(), "main"));
            auto code = std::make_shared<CompiledGraph>(checked(compileGraph(*session, assembled.definition,
                assembled.analysis, assembled.external_bindings, contribution.programs, config.target)));
            auto* context = checked(session->prepare(code, config.target, &scene, nullptr, contribution.arguments));
            CHECK(context->setContextParameters(cameraParameters(CameraState{})));
            return std::pair{context, std::move(code)};
        };
        auto [first, first_code] = compile.template operator()<BasicMeshFeature>();
        auto [depth, depth_code] = compile.template operator()<DepthOnlyFeature>();
        const auto render = [&](ExecutionContext& context) {
            CHECK(checked(context.execute()) == EExecutionOutcome::EXECUTED);
            CHECK(checked(session->queue().wait(*context.lastUse(), UINT64_MAX)));
            return checked(context.readback());
        };
        auto pixels = render(*first);
        pixel(pixels, 17, 30, {255, 0, 0, 255});
        pixel(pixels, 46, 30, {255, 0, 0, 255});
        pixel(pixels, 32, 30, {0, 0, 0, 255});
        pixels = render(*depth);
        pixel(pixels, 17, 30, {0, 0, 0, 255});
        const auto key = featureKey(DepthOnlyFeature::name, "main", "depth.pixels");
        const auto& resources = depth->code().resources();
        const auto found = std::ranges::find(resources, key, &NativeResourcePlan::key);
        CHECK(found != resources.end());
        const auto depths = checked(depth->readback(std::uint32_t(found - resources.begin())));
        float near = 0, background = 0;
        std::memcpy(&near, depths.data() + (30 * 64 + 17) * 4, 4);
        std::memcpy(&background, depths.data() + (30 * 64 + 32) * 4, 4);
        CHECK(std::abs(near - .3f) < .00001f && background == 1);
        // BasicMesh attachments share target extent. DepthOnly's independently declared map stays fixed.
        auto resized = config.target;
        resized.width = resized.height = 32;
        const auto prepare_resized = [&]<class Feature>(const std::shared_ptr<CompiledGraph>& code) {
            const auto metadata = vulkan::FeatureProvider<Feature>{}.definition();
            const auto contribution = checked(metadata.programs(&config, "main"));
            auto* context = checked(session->prepare(code, resized, &scene, nullptr, contribution.arguments));
            CHECK(context->setContextParameters(cameraParameters(CameraState{})));
            return context;
        };
        auto* resized_mesh = prepare_resized.template operator()<BasicMeshFeature>(first_code);
        auto smaller = render(*resized_mesh);
        CHECK(smaller.size() == 32 * 32 * 4);
        CHECK(session->release(resized_mesh));
        auto* resized_depth = prepare_resized.template operator()<DepthOnlyFeature>(depth_code);
        CHECK(render(*resized_depth).size() == 32 * 32 * 4);
        CHECK(checked(resized_depth->readback(std::uint32_t(found - resources.begin()))).size() == 64 * 64 * 4);
        CHECK(session->release(resized_depth));
        // Parameter update rebinds the existing physical table; the immutable Feature graph/code is retained.
        material_input.material = material.material;
        material_input.expected_revision = material.revision;
        material_input.parameters[0] = {0, 1, 0, 1};
        auto changed = checked(lux::render::upload(materials, material_input));
        complete(changed.second, host, *session);
        material = changed.first;
        changed.second.reset();
        pixels = render(*first);
        pixel(pixels, 17, 30, {0, 255, 0, 255});
        CHECK(session->release(depth));
        CHECK(instances.release(right));
        pixels = render(*first);
        pixel(pixels, 17, 30, {0, 255, 0, 255});
        pixel(pixels, 46, 30, {0, 0, 0, 255});
        CHECK(instances.release(left));
        CHECK(session->release(first));
        CHECK(scene.releasePrepared(instance_storage));
        CHECK(global.releasePrepared(material_storage));
        CHECK(global.releasePrepared(mesh_storage));
        CHECK(session->drain());
    }
    CHECK(validation.errors == 0);
    std::printf("BasicMesh/DepthOnly: cooked tetrahedra, indexed-indirect/color/depth; warnings=%u\n",
        validation.warnings.load());
}
