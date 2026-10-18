#include "TextureFixture.hpp"
#include "Allocation.hpp"
#include <lux/engine/render/native/vulkan/mesh/MeshStorage.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/render/native/vulkan/StorageBinding.hpp>
#include <lux/engine/render/graph/Builder.hpp>
#include <fstream>

static bool hold_completion = false;
static unsigned fail_submit = 0, accepted_submits = 0;
static int live_buffers = 0;
namespace lux::render::vulkan::test
{
    VkResult before(const char* call) noexcept
    {
        if (hold_completion && std::string_view(call) == "fence_status") return VK_NOT_READY;
        if (std::string_view(call) == "submit" && fail_submit && --fail_submit == 0)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        return VK_SUCCESS;
    }
    VkResult after(const char* call, VkResult result) noexcept
    {
        if (result == VK_SUCCESS && std::string_view(call) == "buffer") ++live_buffers;
        if (result == VK_SUCCESS && std::string_view(call) == "submit") ++accepted_submits;
        return result;
    }
    void destroyed(const char* call) noexcept { if (std::string_view(call) == "buffer") --live_buffers; }
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

static MeshWrite request(StorageId storage, unsigned index_stride, float color, unsigned vertices = 3)
{
    MeshDescription description{16, vertices, vertices * 16ull, index_stride, 3,
        {{0, 0, rdesc::ETextureFormat::RGBA32_SFLOAT}}, {{0, 3, 0}}};
    auto owner = std::make_shared<std::vector<std::byte>>((description.index_offset + index_stride * 3 + 3) & ~3ull);
    const std::array<float, 12> values{-1, -1, 0, color, 3, -1, 0, color, -1, 3, 0, color};
    std::memcpy(owner->data(), values.data(), sizeof(values));
    for (unsigned i = 0; i < 3; ++i)
        std::memcpy(owner->data() + description.index_offset + i * index_stride, &i, index_stride);
    return {{storage, {}}, 0, std::move(description), payload(owner)};
}

static void pixels(ExecutionContext& context, DeviceSession& session, unsigned value)
{
    render_allocation::participate = true;
    render_allocation::calls = 0;
    render_allocation::window = true;
    CHECK(take(context.execute()) == EExecutionOutcome::EXECUTED);
    CHECK(take(session.queue().wait(*context.lastUse(), UINT64_MAX)));
    render_allocation::window = false;
    CHECK(render_allocation::calls == 0);
    for (auto byte : take(context.readback())) CHECK(std::to_integer<unsigned>(byte) == value);
}

static void snapshotLifetime(DeviceSession& session, CpuFixture& host)
{
    const auto id = take(session.globalStorages().getOrCreate(session.resources().typeId(kMeshStorageType), "snapshot"));
    auto& storage = *take(session.resources().borrow<lux::render::vulkan::MeshStorage>(id));
    auto input = request(id, 4, .25f);
    auto upload = take(lux::render::upload(storage, input));
    complete(upload.second, host, session);
    const auto published = upload.first;
    upload.second.reset();
    const auto factory = meshStorageFactory();
    const auto& ops = *static_cast<const StorageBindingOps*>(factory.binding_ops);
    NativeResourcePlan plan{"mesh", GraphBufferDesc{input.source.bytes().view().size()}, EGraphOrigin::IMPORTED};
    const MeshBinding argument{published.mesh, input.description};
    PreparedStorageBinding first, second, third;
    CHECK(ops.prepare(&storage, plan, &argument, first));
    CHECK(ops.prepare(&storage, plan, &argument, second));
    CHECK(ops.prepare(&storage, plan, &argument, third));
    first = {}; // Swap removal must preserve the remaining subscription positions.
    second = {};
    std::weak_ptr old = third.resources;
    auto destination = take(session.allocator().createBuffer(
        third.buffer->size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK
    ));
    auto batch = take(session.queue().begin());
    batch.retain(third.resources);
    batch.retain(destination);
    const VkBufferCopy copy{0, 0, third.buffer->size()};
    vkCmdCopyBuffer(batch.native(), third.buffer->native(), destination->native(), 1, &copy);
    VkBufferMemoryBarrier2 host_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    host_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    host_barrier.srcQueueFamilyIndex = host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_barrier.buffer = destination->native();
    host_barrier.size = VK_WHOLE_SIZE;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &host_barrier;
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    const auto ticket = take(std::move(batch).submit());
    hold_completion = true;
    CHECK(storage.release(published.mesh, published.revision));
    CHECK(session.globalStorages().remove(id));
    CHECK(!session.resources().native(id));
    third = {}; // Late subscription destruction cannot touch the now-destroyed logical Storage.
    CHECK(!old.expired());
    {
        auto rejected = take(session.queue().begin());
        auto unrelated = take(session.allocator().createBuffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::DEVICE));
        std::weak_ptr rejected_owner = unrelated;
        rejected.retain(unrelated);
        unrelated.reset();
        fail_submit = 1;
        CHECK(!std::move(rejected).submit());
        CHECK(rejected_owner.expired() && !old.expired());
    }
    CHECK(session.collect());
    CHECK(!old.expired());
    hold_completion = false;
    CHECK(take(session.queue().wait(ticket, UINT64_MAX)));
    CHECK(old.expired());
    std::vector<std::byte> result(destination->size());
    CHECK(destination->read(0, result));
    CHECK(std::ranges::equal(result, input.source.bytes().view()));
}

int main(int argc, char** argv)
{
    CHECK(argc == 3);
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
        CHECK(session->resources().installFactory(meshStorageFactory(), MeshStorageConfig{}));
        const auto storage_id = take(session->globalStorages().getOrCreate(
            session->resources().typeId(kMeshStorageType), "mesh"));
        auto& storage = *take(session->resources().borrow<lux::render::vulkan::MeshStorage>(storage_id));
        const TargetDesc target{16, 16, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
        for (unsigned stride : {2u, 4u})
        {
            auto input = request(storage_id, stride, .25f);
            input.description.attributes.reserve(17);
            input.description.submeshes.reserve(19);
            for (unsigned fault = 0; fault < 7; ++fault)
            {
                auto bad = request(storage_id, stride, .25f);
                if (fault == 0) bad.description.vertex_stride = 0;
                if (fault == 1) bad.description.index_offset = UINT64_MAX;
                if (fault == 2) bad.description.attributes[0].offset = 16;
                if (fault == 3) bad.description.submeshes[0].index_count = 6;
                if (fault == 4) bad.description.submeshes[0].vertex_offset = -1;
                if (fault == 5) bad.description.attributes.push_back(bad.description.attributes[0]);
                if (fault == 6) bad.expected_revision = 1;
                const auto before = live_buffers;
                CHECK(!lux::render::upload(storage, bad));
                CHECK(live_buffers == before && storage.residentVersions() == 0);
            }
            auto candidate = take(lux::render::upload(storage, input));
            auto published = candidate.first;
            CHECK(!storage.query(published.mesh));
            candidate.second.reset();
            CHECK(storage.residentVersions() == 0 && !storage.query(published.mesh));
            candidate = take(lux::render::upload(storage, input));
            CHECK(candidate.first.mesh != published.mesh);
            published = candidate.first;
            complete(candidate.second, host, *session);
            candidate.second.reset();
            CHECK(take(storage.query(published.mesh)).description == input.description);

            RenderGraphBuilder builder;
            const auto bytes = input.source.bytes().view().size();
            const auto mesh = take(builder.declareResource({"mesh", GraphBufferDesc{bytes}, EGraphOrigin::IMPORTED,
                {GraphBufferRange{0, bytes}}}));
            const auto color = take(builder.declareResource({"color", GraphImageDesc{16, 16}}));
            const auto output = take(builder.declareResource({"pixels", GraphBufferDesc{1024}}));
            const auto copy = take(builder.declareResource({"mesh_copy", GraphBufferDesc{bytes}}));
            CHECK(builder.addPass({"draw", EGraphPassKind::GRAPHICS,
                {{{mesh, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::VERTEX},
                 {{mesh, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::INDEX},
                 {{color, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::COLOR_ATTACHMENT}}}));
            CHECK(builder.addPass({"read", EGraphPassKind::TRANSFER,
                {{{color, 1}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
                 {{output, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
            CHECK(builder.addPass({"mesh_read", EGraphPassKind::TRANSFER,
                {{{mesh, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
                 {{copy, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
            CHECK(builder.exportOutput({{color, 1}, {}, target}));
            CHECK(builder.exportOutput({{output, 1}}));
            CHECK(builder.exportOutput({{copy, 1}}));
            auto graph = take(std::move(builder).finish());
            auto analysis = take(analyze(graph));
            PassProgram draw{"draw"};
            draw.shader.stages = {{rdesc::EShaderType::VERTEX, "main", spirv(argv[1])},
                {rdesc::EShaderType::FRAGMENT, "main", spirv(argv[2])}};
            draw.graphics.vertex_bindings = {{0, 16}};
            draw.graphics.vertex_attributes = {{0, 0, 0, rdesc::ETextureFormat::RGBA32_SFLOAT}};
            draw.vertex_buffers = {{"mesh", 0, 0}};
            draw.draw = DrawIndexed{"mesh", input.description.index_offset,
                stride == 2 ? EIndexType::UINT16 : EIndexType::UINT32, 3};
            const std::array programs{draw, PassProgram{"read"}, PassProgram{"mesh_read"}};
            GraphStorageBindings bindings;
            bindings.global = {{mesh, {StorageName(kMeshStorageType.name()), "mesh"}}};
            auto code = std::make_shared<CompiledGraph>(take(compileGraph(
                *session, graph, analysis, bindings, programs, target)));
            const std::array arguments{StorageArgument{"mesh", take(ConfigValue::make(
                MeshBinding{published.mesh, input.description}))}};
            auto* first = take(session->prepare(code, target, nullptr, nullptr, arguments));
            auto* second = take(session->prepare(code, target, nullptr, nullptr, arguments));
            pixels(*first, *session, 64);
            const auto copied = take(first->readback(3));
            CHECK(std::ranges::equal(copied, input.source.bytes().view()));
            pixels(*second, *session, 64);
            auto wrong = input;
            wrong.mesh = published.mesh;
            wrong.expected_revision = 1;
            wrong.description.attributes[0].format = rdesc::ETextureFormat::RGBA32_UINT;
            CHECK(!lux::render::upload(storage, wrong)); // incompatible immutable Draw ABI
            auto replacement = request(storage_id, stride, .75f);
            replacement.mesh = published.mesh;
            replacement.expected_revision = published.revision;
            auto replace = take(lux::render::upload(storage, replacement));
            CHECK(!storage.release(published.mesh, published.revision));
            complete(replace.second, host, *session);
            published = replace.first;
            replace.second.reset();
            CHECK(published.revision == 2);
            pixels(*first, *session, 191);
            CHECK(session->release(first));
            pixels(*second, *session, 191);
            CHECK(!storage.release(published.mesh, 1));
            CHECK(take(second->execute()) == EExecutionOutcome::EXECUTED);
            hold_completion = true;
            CHECK(storage.release(published.mesh, 2));
            CHECK(storage.residentVersions() == 1);
            CHECK(!storage.query(published.mesh));
            CHECK(session->release(second));
            // Actual accepted Draw retains Buffer after both logical release/detach.
            CHECK(storage.residentVersions() == 1);
            hold_completion = false;
            CHECK(session->drain());
            CHECK(storage.residentVersions() == 0);
        }
        // A successful earlier chunk followed by failed submission preserves the published last-good Mesh.
        auto large = request(storage_id, 4, .25f, 10000);
        auto original = take(lux::render::upload(storage, large));
        complete(original.second, host, *session);
        const auto last_good = original.first;
        original.second.reset();
        large.mesh = last_good.mesh;
        large.expected_revision = 1;
        auto upload = take(lux::render::upload(storage, large));
        const auto before = accepted_submits;
        fail_submit = 2;
        WorkCompletion completion;
        completion.start(upload.second, host);
        finish(completion, host, *session);
        CHECK(!completion.result && accepted_submits == before + 1);
        CHECK(!upload.second.publish());
        CHECK(take(storage.query(last_good.mesh)).published.revision == 1);
        upload.second.reset();
        CHECK(storage.residentVersions() == 1);
        CHECK(storage.release(last_good.mesh, 1));
        CHECK(storage.residentVersions() == 0);
        if (topology == 0)
        {
            // More than the old test fixture's64/128 records, with no production capacity policy.
            std::vector<MeshPublished> records;
            auto small = request(storage_id, 4, .25f);
            for (unsigned i = 0; i < 130; ++i)
            {
                auto created = take(lux::render::upload(storage, small));
                complete(created.second, host, *session);
                records.push_back(created.first);
            }
            CHECK(storage.residentVersions() == records.size());
            for (auto value : records) CHECK(storage.release(value.mesh, value.revision));
            CHECK(storage.residentVersions() == 0);
            const auto limited_id = take(session->globalStorages().getOrCreate(
                session->resources().typeId(kMeshStorageType), "budget", take(ConfigValue::make(MeshStorageConfig{60}))
            ));
            auto& limited = *take(session->resources().borrow<lux::render::vulkan::MeshStorage>(limited_id));
            auto limited_input = request(limited_id, 4, .25f);
            auto created = take(lux::render::upload(limited, limited_input));
            complete(created.second, host, *session);
            const auto live = created.first;
            created.second.reset();
            const auto before_submit = accepted_submits;
            const auto rejected = lux::render::upload(limited, limited_input);
            CHECK(!rejected && rejected.error().type == kStorageCapacity);
            CHECK(accepted_submits == before_submit && limited.residentBytes() == 60);
            CHECK(limited.release(live.mesh, 1));
            CHECK(limited.residentBytes() == 0);
            auto retry = take(lux::render::upload(limited, limited_input));
            complete(retry.second, host, *session);
            CHECK(limited.release(retry.first.mesh, 1));
        }
        snapshotLifetime(*session, host);
        CHECK(session->drain());
        session.reset();
        CHECK(live_buffers == 0);
    }
    CHECK(validation.errors == 0);
    std::printf("Mesh: three queue modes, UInt16/32 indexed output64/191, exact backing readback, "
        "revision/rollback/rebind/shared-use/in-flight release; hot new=0; errors=%u\n", validation.errors.load());
}
