#include "TextureFixture.hpp"
#include <lux/engine/render/native/vulkan/instance/InstanceStorage.hpp>
#include <lux/engine/render/native/vulkan/mesh/MeshStorage.hpp>
#include <lux/engine/render/native/vulkan/material/MaterialStorage.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/render/graph/Builder.hpp>
#include <fstream>
#include <limits>

static bool hold_completion = false;
static int buffers = 0;
static unsigned fail_flush = 0;
namespace lux::render::vulkan::test
{
    VkResult before(const char* call) noexcept
    {
        if (std::string_view(call) == "flush" && fail_flush && --fail_flush == 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        return hold_completion && std::string_view(call) == "fence_status" ? VK_NOT_READY : VK_SUCCESS;
    }
    VkResult after(const char* call, VkResult result) noexcept
    {
        if (result == VK_SUCCESS && std::string_view(call) == "buffer") ++buffers;
        return result;
    }
    void destroyed(const char* call) noexcept { if (std::string_view(call) == "buffer") --buffers; }
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

static void groups(DeviceSession& session, lux::render::vulkan::InstanceStorage& instances,
    MeshId mesh, MaterialId material, InstancePublished removed)
{
    const auto capacity = instances.recordCount();
    const auto matrix_bytes = std::uint64_t(capacity) * sizeof(TransformMatrix);
    const auto group_bytes = (std::uint64_t(capacity) + 5) * sizeof(std::uint32_t);
    const TargetDesc target{16, 16, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
    RenderGraphBuilder builder;
    const auto matrices = take(builder.declareResource({"matrices", GraphBufferDesc{matrix_bytes},
        EGraphOrigin::IMPORTED, {GraphBufferRange{0, matrix_bytes}}}));
    const auto members = take(builder.declareResource({"members", GraphBufferDesc{group_bytes},
        EGraphOrigin::IMPORTED, {GraphBufferRange{0, group_bytes}}}));
    const auto color = take(builder.declareResource({"color", GraphImageDesc{16, 16}}));
    const auto matrix_copy = take(builder.declareResource({"matrix_copy", GraphBufferDesc{matrix_bytes}}));
    const auto group_copy = take(builder.declareResource({"group_copy", GraphBufferDesc{group_bytes}}));
    CHECK(builder.addPass({"clear", EGraphPassKind::TRANSFER,
        {{{color, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
    for (const auto& pair : {std::pair{matrices, matrix_copy}, std::pair{members, group_copy}})
        CHECK(builder.addPass({pair.first == matrices ? "matrices" : "members", EGraphPassKind::TRANSFER,
            {{{pair.first, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
             {{pair.second, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
    CHECK(builder.exportOutput({{color, 1}, {}, target}));
    CHECK(builder.exportOutput({{matrix_copy, 1}}));
    CHECK(builder.exportOutput({{group_copy, 1}}));
    auto definition = take(std::move(builder).finish());
    auto analysis = take(analyze(definition));
    GraphStorageBindings bindings;
    bindings.global = {{matrices, {StorageName(kInstanceStorageType.name()), "instances"}},
        {members, {StorageName(kInstanceStorageType.name()), "instances"}}};
    const std::array programs{PassProgram{"clear"}, PassProgram{"matrices"}, PassProgram{"members"}};
    auto code = std::make_shared<CompiledGraph>(take(compileGraph(
        session, definition, analysis, bindings, programs, target)));
    const InstanceDrawGroup selection{mesh, material};
    const std::array arguments{
        StorageArgument{"matrices", take(ConfigValue::make(InstanceBinding{capacity, selection}))},
        StorageArgument{"members", take(ConfigValue::make(InstanceBinding{capacity, selection, EInstanceBinding::DRAW}))}
    };
    const auto before = instances.residentBytes();
    auto* first = take(session.prepare(code, target, nullptr, nullptr, arguments));
    CHECK(instances.residentBytes() == before + group_bytes * 3);
    auto* second = take(session.prepare(code, target, nullptr, nullptr, arguments));
    CHECK(instances.residentBytes() == before + group_bytes * 3); // Shared selection, no per-View GPU group copy.
    hold_completion = true;
    CHECK(take(first->execute()));
    fail_flush = 1;
    CHECK(!instances.release(removed));
    CHECK(instances.query(removed.instance));
    CHECK(instances.release(removed));
    CHECK(!instances.query(removed.instance));
    InstanceDescription input{mesh, material};
    input.transform[12] = 42;
    fail_flush = 2; // Membership staging succeeds; matrix staging then fails before either publication.
    CHECK(!instances.create(input));
    auto replacement = take(instances.create(input));
    CHECK(replacement.instance.local.index == removed.instance.local.index);
    CHECK(replacement.instance.local.gen != removed.instance.local.gen);
    CHECK(take(second->execute()));
    CHECK(instances.release(replacement));
    auto busy = instances.create(input); // Both non-current group banks are held by accepted GPU work.
    CHECK(!busy && busy.error().type == kBusy);
    CHECK(!instances.query(replacement.instance));
    hold_completion = false;
    CHECK(session.drain());
    const auto old_matrix = take(first->readback(matrix_copy.index));
    const auto new_matrix = take(second->readback(matrix_copy.index));
    TransformMatrix old_value, new_value;
    const auto offset = std::size_t(removed.instance.local.index) * sizeof(TransformMatrix);
    std::memcpy(old_value.data(), old_matrix.data() + offset, sizeof(old_value));
    std::memcpy(new_value.data(), new_matrix.data() + offset, sizeof(new_value));
    CHECK(old_value[12] == 0 && new_value[12] == 42);
    const auto old_group = take(first->readback(group_copy.index));
    const auto new_group = take(second->readback(group_copy.index));
    std::array<std::uint32_t, 5> old_args, new_args;
    std::memcpy(old_args.data(), old_group.data(), sizeof(old_args));
    std::memcpy(new_args.data(), new_group.data(), sizeof(new_args));
    CHECK(old_args[0] == 3 && old_args[1] == 9999 && old_args[4] == 0);
    CHECK(new_args == old_args); // Replacement restores the count without recompiling the Graph.
    replacement = take(instances.create(input));
    CHECK(instances.release(replacement));
    CHECK(session.release(first));
    CHECK(instances.residentBytes() == before + group_bytes * 3);
    CHECK(session.release(second));
    CHECK(session.drain());
    CHECK(instances.residentBytes() == before);
    std::puts("Instance group: shared snapshots, held delete/reuse, flush rollback, Busy and zero residual group bytes");
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
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
        auto session = take(DeviceSession::create(bootstrap, options));
        CHECK(session->configureUploads({65536, 2, 2 * 1024 * 1024}, host.cpu()));
        auto& registry = session->resources();
        CHECK(registry.installFactory(meshStorageFactory(), MeshStorageConfig{}));
        CHECK(registry.installFactory(materialStorageFactory(), MaterialStorageConfig{}));
        CHECK(registry.installFactory(instanceStorageFactory(), InstanceStorageConfig{}));
        auto& globals = session->globalStorages();
        const auto mesh_storage = take(globals.prepare(registry.typeId(kMeshStorageType), "mesh"));
        const auto material_storage = take(globals.prepare(registry.typeId(kMaterialStorageType), "material"));
        const auto instance_storage = take(globals.prepare(registry.typeId(kInstanceStorageType), "instances"));
        auto& instances = *take(registry.borrow<lux::render::vulkan::InstanceStorage>(instance_storage));
        auto& meshes = *take(registry.borrow<lux::render::vulkan::MeshStorage>(mesh_storage));
        auto& materials = *take(registry.borrow<lux::render::vulkan::MaterialStorage>(material_storage));
        auto owner = std::make_shared<std::vector<std::byte>>(60);
        const std::array<unsigned, 3> indices{0, 1, 2};
        std::memcpy(owner->data() + 48, indices.data(), 12);
        MeshWrite mesh_request{{mesh_storage, {}}, 0,
            {16, 3, 48, 4, 3, {{0, 0, rdesc::ETextureFormat::RGBA32_SFLOAT}}, {{0, 3, 0}}}, payload(owner)};
        auto mesh_upload = take(lux::render::upload(meshes, mesh_request));
        complete(mesh_upload.second, host, *session);
        const auto mesh = mesh_upload.first.mesh;
        mesh_upload.second.reset();
        MaterialProgram program{{rdesc::EShaderType::FRAGMENT, "main", spirv(argv[1])}, {}, 1};
        program.defaults[0] = {1, 1, 1, 1};
        auto config = take(ConfigValue::make(std::move(program)));
        MaterialWrite material_request{{material_storage, {}}, 0, config, config.borrow<MaterialProgram>()->defaults};
        auto material_upload = take(lux::render::upload(materials, material_request));
        complete(material_upload.second, host, *session);
        const auto material = material_upload.first.material;
        material_upload.second.reset();
        std::vector<InstancePublished> ids;
        for (unsigned count : {1u, 100u, 1000u, 10000u})
        {
            while (ids.size() < count) ids.push_back(take(instances.create({mesh, material})));
            TransformBatch all{instance_storage, count};
            for (const auto& id : ids) all.updates.push_back({id.instance, id.revision, kIdentityTransform});
            CHECK(take(instances.update(all)).changed == count);
            for (auto& id : ids) ++id.revision;
            // Settle the previous full-batch dirt on the other bank before measuring a sparse-only workload.
            TransformBatch settle{instance_storage, 0, {{ids.front().instance, ids.front().revision, kIdentityTransform}}};
            CHECK(instances.update(settle));
            ++ids.front().revision;
            TransformBatch batch{instance_storage, count, {{ids.front().instance, ids.front().revision, kIdentityTransform}}};
            batch.updates[0].transform[12] = float(count);
            const auto written = instances.writtenRecords(), checked = instances.checkedUpdates();
            const auto result = take(instances.update(batch));
            CHECK(result.sequence == count && result.changed == 1);
            ++ids.front().revision;
            CHECK(instances.checkedUpdates() - checked == 1);
            CHECK(instances.writtenRecords() - written <= 3);
            std::printf("Instance sparse live=%u checked=1 written=%llu resident=%llu\n", count,
                static_cast<unsigned long long>(instances.writtenRecords() - written),
                static_cast<unsigned long long>(instances.residentBytes()));
        }
        TransformBatch batch{instance_storage, 73,
            {{ids[0].instance, ids[0].revision, kIdentityTransform}, {ids[1].instance, ids[1].revision, kIdentityTransform}}};
        batch.updates.reserve(31);
        CHECK(take(CommandValueTraits<TransformBatch>::retainedBytes(batch)) ==
            sizeof(batch) + batch.updates.capacity() * sizeof(TransformUpdate));
        const auto before = take(instances.query(ids[0].instance));
        batch.updates[1].expected_revision = 0;
        CHECK(!instances.update(batch));
        CHECK(take(instances.query(ids[0].instance)).published.revision == before.published.revision);
        batch.updates[1].expected_revision = ids[1].revision;
        batch.updates.push_back(batch.updates.front());
        batch.updates.back().transform[12] = .75f;
        CHECK(take(instances.update(batch)).changed == 2);
        ++ids[0].revision; ++ids[1].revision;
        CHECK(take(instances.query(ids[0].instance)).description.transform[12] == .75f);
        CHECK(!instances.update(batch));
        // A failed candidate flush cannot alter the published matrix or revision. Retry repairs dirty candidate bytes.
        batch.updates = {{ids[0].instance, ids[0].revision, kIdentityTransform}};
        batch.updates[0].transform[12] = .5f;
        fail_flush = 1;
        CHECK(!instances.update(batch));
        CHECK(take(instances.query(ids[0].instance)).published.revision == ids[0].revision);
        CHECK(take(instances.query(ids[0].instance)).description.transform[12] == .75f);
        CHECK(instances.update(batch));
        ++ids[0].revision;
        batch.updates[0].expected_revision = ids[0].revision;
        batch.updates[0].transform[12] = .75f;
        CHECK(instances.update(batch));
        ++ids[0].revision;
        auto bad = ids[1].instance; ++bad.local.gen;
        CHECK(!instances.query(bad));
        bad = ids[1].instance; bad.storage = mesh_storage;
        CHECK(!instances.query(bad));
        batch.updates = {{ids[0].instance, ids[0].revision, kIdentityTransform}};
        batch.updates[0].transform[0] = std::numeric_limits<float>::quiet_NaN();
        CHECK(!instances.update(batch));

        const auto released = ids.back();
        CHECK(instances.release(released));
        CHECK(!instances.query(released.instance));
        const auto rebuilt = take(instances.create({mesh, material}));
        CHECK(rebuilt.instance != released.instance);
        CHECK(!instances.query(released.instance));
        CHECK(instances.release(rebuilt));

        groups(*session, instances, mesh, material, ids[1]);

        const TargetDesc target{16, 16, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
        RenderGraphBuilder builder;
        const auto source = take(builder.declareResource({"instances", GraphBufferDesc{128}, EGraphOrigin::IMPORTED,
            {GraphBufferRange{0, 128}}}));
        const auto color = take(builder.declareResource({"color", GraphImageDesc{16, 16}}));
        const auto output = take(builder.declareResource({"copy", GraphBufferDesc{128}}));
        CHECK(builder.addPass({"clear", EGraphPassKind::TRANSFER,
            {{{color, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
        CHECK(builder.addPass({"copy", EGraphPassKind::TRANSFER,
            {{{source, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
             {{output, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
        CHECK(builder.exportOutput({{color, 1}, {}, target}));
        CHECK(builder.exportOutput({{output, 1}}));
        auto definition = take(std::move(builder).finish());
        auto analysis = take(analyze(definition));
        GraphStorageBindings bindings;
        bindings.global = {{source, {StorageName(kInstanceStorageType.name()), "instances"}}};
        const std::array programs{PassProgram{"clear"}, PassProgram{"copy"}};
        auto code = std::make_shared<CompiledGraph>(take(compileGraph(*session, definition, analysis, bindings, programs, target)));
        const std::array arguments{StorageArgument{"instances", take(ConfigValue::make(InstanceBinding{2}))}};
        std::array<ExecutionContext*, 3> contexts;
        for (auto& context : contexts) context = take(session->prepare(code, target, nullptr, nullptr, arguments));
        CHECK(take(contexts[0]->execute()));
        CHECK(take(session->queue().wait(*contexts[0]->lastUse(), UINT64_MAX)));
        const auto bytes = take(contexts[0]->readback(2));
        TransformMatrix actual;
        std::memcpy(actual.data(), bytes.data(), sizeof(actual));
        CHECK(actual[12] == .75f);
        hold_completion = true;
        for (unsigned i = 0; i < 3; ++i)
        {
            if (i)
            {
                batch.updates = {{ids[0].instance, ids[0].revision, kIdentityTransform}};
                batch.updates[0].transform[12] = float(i);
                CHECK(instances.update(batch));
                ++ids[0].revision;
            }
            CHECK(take(contexts[i]->execute()));
        }
        batch.updates = {{ids[0].instance, ids[0].revision, kIdentityTransform}};
        auto busy = instances.update(batch);
        CHECK(!busy && busy.error().type == kBusy);
        CHECK(take(instances.query(ids[0].instance)).published.revision == ids[0].revision);
        CHECK(!instances.release(ids[0]));
        for (auto* context : contexts) CHECK(session->release(context));
        const auto retained = buffers;
        CHECK(globals.releasePrepared(instance_storage));
        CHECK(!registry.native(instance_storage));
        CHECK(buffers == retained);
        hold_completion = false;
        CHECK(session->drain());
        CHECK(buffers < retained);
        CHECK(globals.releasePrepared(mesh_storage));
        CHECK(globals.releasePrepared(material_storage));
        CHECK(session->drain());
        session.reset();
        CHECK(buffers == 0);
    }
    CHECK(validation.errors == 0);
    std::printf("Instance three topologies: Validation errors=0 warnings=%u\n", validation.warnings.load());
}
