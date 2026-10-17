#include "TextureFixture.hpp"
#include "TextureReadback.hpp"
#include "extension/NativeBuffer.hpp"
#include "extension/Feature.hpp"
#include "extension/WrongBlock.hpp"
#include "extension/Packed.hpp"
#include "Allocation.hpp"
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>

namespace lux::render::vulkan
{
    struct NativeMemoryAudit final
    {
        static void repeatedRebind(ExecutionContext& context) noexcept
        {
            const auto submitted = context.submitted_bank_;
            context.rebind();
            CHECK(context.descriptor_bank_ != submitted);
            context.rebind();
            CHECK(context.descriptor_bank_ != submitted); // repeated updates reuse the safe bank, never toggle into pending
        }
    };
}

static bool hold_completion = false;
static std::string_view fail_operation;
static unsigned destroyed_buffers = 0;
namespace lux::render::vulkan::test
{
    VkResult before(const char* operation) noexcept
    {
        if (fail_operation == operation) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (hold_completion && std::string_view(operation) == "fence_status") return VK_NOT_READY;
        return VK_SUCCESS;
    }
    VkResult after(const char*, VkResult result) noexcept { return result; }
    void destroyed(const char* operation) noexcept
    {
        if (std::string_view(operation) == "buffer") ++destroyed_buffers;
    }
}

static void execute(ExecutionContext& context, DeviceSession& session, unsigned expected)
{
    CHECK(take(context.execute()));
    CHECK(take(session.queue().wait(*context.lastUse(), UINT64_MAX)));
    const auto data = take(context.readback());
    CHECK(data.size() == 1024);
    for (auto byte : data) CHECK(std::to_integer<unsigned>(byte) == expected);
}

int main()
{
    using extension_test::NativeBuffer;
    using extension_test::TestBufferStorage;
    Validation validation;
    auto bootstrap = instance(validation);
    DeviceOptions options;
    options.dynamic_rendering = true;
    options.texture_descriptors = true;
    auto session = take(DeviceSession::create(bootstrap, options));
    CpuFixture worker;
    CHECK(session->configureUploads({65536, 2, 131072}, worker.cpu()));
    CHECK(configureTextures(*session, 16, 1024 * 1024));
    CHECK(session->resources().installFactory(extension_test::bufferFactory(), TestBufferStorage::Config{}));
    auto instance_id = take(session->globalStorages().prepare(
        session->resources().typeId(TestBufferStorage::description.type), "values"));
    auto& buffer = *static_cast<NativeBuffer*>(session->resources().native(instance_id));
    const std::array<float, 4> half{0.5f, 0.5f, 0.5f, 0.5f};
    const auto half_bytes = std::as_bytes(std::span(half));
    const auto half_owner = std::make_shared<const std::vector<std::byte>>(half_bytes.begin(), half_bytes.end());
    auto upload = take(lux::render::upload(buffer, extension_test::SetBuffer::Request{{instance_id, {}}, payload(half_owner)}));
    const auto id = upload.first.id;
    complete(upload.second, worker, *session);
    upload.second.reset();
    CHECK(take(session->resources().tryGet<TestBufferStorage>(instance_id)).exists(id));
    const auto white_owner = std::make_shared<const std::vector<std::byte>>(1024, std::byte{128});
    const auto& white = *white_owner;
    auto image = take(lux::render::upload(*textures(*session),
        SetTexture::Request{{textures(*session)->id(), {}}, 0, information(16, 1, false), payload(white_owner)}));
    const auto texture = image.first.texture;
    complete(image.second, worker, *session);
    image.second.reset();
    auto config = take(ConfigValue::make(extension_test::ViewConfig{texture, id}));
    const FeatureInstanceView selected{&extension_test::definition, &extension_test::definition_graph, &config, "combined"};
    auto assembly = take(assembleFeatures({&selected, 1}, extension_test::limits.max_encoded_bytes));
    auto contribution = take(extension_test::programs(config.data(), "combined"));
    auto& programs = contribution.programs;
    const auto count_before = session->resources().size();
    auto code = std::make_shared<CompiledGraph>(take(compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, programs, extension_test::target)));
    CHECK(session->resources().size() == count_before); // compiling cannot create physical scoped instances
    auto& arguments = contribution.arguments;
    const auto native_before = session->resources().size();
    auto wrong_arguments = arguments;
    wrong_arguments.front().value = take(ConfigValue::make(std::uint32_t{1}));
    auto wrong_type = session->prepare(code, extension_test::target, nullptr, nullptr, wrong_arguments);
    CHECK(!wrong_type && wrong_type.error().type == kStorageContract);
    CHECK(session->resources().size() == native_before);
    CHECK(!session->prepare(code, extension_test::target)); // Required typed arguments are not implicit defaults.
    wrong_arguments = arguments;
    wrong_arguments.front().resource_key = "unknown";
    CHECK(!session->prepare(code, extension_test::target, nullptr, nullptr, wrong_arguments));
    wrong_arguments = arguments;
    wrong_arguments.push_back({arguments.front().resource_key, take(ConfigValue::make(TextureBinding{}))});
    auto conflicting_argument = session->prepare(code, extension_test::target, nullptr, nullptr, wrong_arguments);
    CHECK(!conflicting_argument && conflicting_argument.error().type == kStorageMismatch);
    CHECK(session->resources().size() == native_before);
    for (const auto label : {"context_descriptor_pool", "context_descriptor_set"})
    {
        fail_operation = label;
        const auto rejected = session->prepare(code, extension_test::target, nullptr, nullptr, arguments);
        CHECK(!rejected && rejected.error().type == kNativeFailure);
        fail_operation = {};
        CHECK(buffer.backingCount() == 1 && session->resources().size() == native_before);
    }
    auto* context = take(session->prepare(code, extension_test::target, nullptr, nullptr, arguments));
    execute(*context, *session, 64);
    {
        auto packed = programs;
        packed[0].shader.stages[1].words.assign(extension_test::packed_fragment.begin(), extension_test::packed_fragment.end());
        packed[0].shader.interface.bindings = {
            {2, 5, EDescriptorKind::SAMPLED_IMAGE_ARRAY, 0, {}, "textures"},
            {3, 7, EDescriptorKind::STORAGE_BUFFER, 1, extension_test::colorBlock(), "data"},
            {6, 11, EDescriptorKind::STORAGE_BUFFER, 1, extension_test::colorBlock(), "other"}
        };
        packed[0].resources = {{"textures", "shared:texture"}, {"data", "shared:values"}, {"other", "shared:values"}};
        auto packed_code = std::make_shared<CompiledGraph>(take(compileGraph(*session, assembly.definition,
            assembly.analysis, assembly.external_bindings, packed, extension_test::target)));
        auto* packed_context = take(session->prepare(packed_code, extension_test::target, nullptr, nullptr, arguments));
        execute(*packed_context, *session, 32); // same Buffer, two independently packed bindings, real relocation
        CHECK(session->release(packed_context));
        {
            const std::array<float, 4> values{0.25f, 0.25f, 0.25f, 0.25f};
            const auto bytes = std::as_bytes(std::span(values));
            auto owner = std::make_shared<const std::vector<std::byte>>(bytes.begin(), bytes.end());
            auto other = take(lux::render::upload(buffer,
                extension_test::SetBuffer::Request{{instance_id, {}}, payload(owner)}));
            const auto other_id = other.first.id;
            complete(other.second, worker, *session);
            other.second.reset();
            RenderGraphBuilder builder;
            const auto image = take(builder.declareResource({"image", GraphImageDesc{16, 16}, EGraphOrigin::IMPORTED,
                {GraphImageRange{1, 0, 1, 0, 1}}}));
            const auto a = take(builder.declareResource({"a", GraphBufferDesc{16}, EGraphOrigin::IMPORTED,
                {GraphBufferRange{0, 16}}}));
            const auto b = take(builder.declareResource({"b", GraphBufferDesc{16}, EGraphOrigin::IMPORTED,
                {GraphBufferRange{0, 16}}}));
            const auto color = take(builder.declareResource({"color", GraphImageDesc{16, 16}}));
            const auto pixels = take(builder.declareResource({"pixels", GraphBufferDesc{1024}}));
            take(builder.addPass({"draw", EGraphPassKind::GRAPHICS,
                {{{image, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::SAMPLED},
                 {{a, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::STORAGE_READ},
                 {{b, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::STORAGE_READ},
                 {{color, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::COLOR_ATTACHMENT}}}));
            take(builder.addPass({"read", EGraphPassKind::TRANSFER,
                {{{color, 1}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
                 {{pixels, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
            CHECK(builder.exportOutput({{color, 1}, {}, extension_test::target}));
            CHECK(builder.exportOutput({{pixels, 1}}));
            auto graph = take(std::move(builder).finish());
            auto analysis = take(analyze(graph));
            GraphStorageBindings storage;
            storage.global = {
                {image, {StorageName(kTextureStorageType.name()), "default"}},
                {a, {StorageName(TestBufferStorage::description.type.name()), "values"}},
                {b, {StorageName(TestBufferStorage::description.type.name()), "values"}}
            };
            auto distinct = packed;
            distinct[0].pass_key = "draw";
            distinct[1].pass_key = "read";
            distinct[0].resources = {{"other", "b"}, {"textures", "image"}, {"data", "a"}};
            const std::array arguments{
                StorageArgument{"image", take(ConfigValue::make(TextureBinding{texture}))},
                StorageArgument{"a", take(ConfigValue::make(extension_test::BufferBinding{id}))},
                StorageArgument{"b", take(ConfigValue::make(extension_test::BufferBinding{other_id}))}
            };
            auto code = std::make_shared<CompiledGraph>(take(compileGraph(*session, graph, analysis, storage,
                distinct, extension_test::target)));
            auto* context = take(session->prepare(code, extension_test::target, nullptr, nullptr, arguments));
            execute(*context, *session, 16); // distinct buffers: 128 * 0.5 * 0.25, no private set per resource
            CHECK(session->release(context));
            CHECK(buffer.remove(other_id));
        }
        for (unsigned fault = 0; fault < 6; ++fault)
        {
            auto bad = packed;
            if (fault == 0) bad[0].resources[1].resource_key = "missing";
            if (fault == 1) bad[0].resources[1].shader_name = "missing";
            if (fault == 2) bad[0].shader.interface.bindings[1].name = "missing";
            if (fault == 3) bad[0].shader.interface.bindings[1].stages = 1;
            if (fault == 4) bad[0].shader.interface.bindings[1].kind = static_cast<EDescriptorKind>(255);
            if (fault == 5)
            {
                bad[0].shader.interface.bindings[2].binding = 7;
                bad[0].shader.interface.bindings[2].set = 3;
            }
            CHECK(!compileGraph(*session, assembly.definition, assembly.analysis, assembly.external_bindings,
                bad, extension_test::target));
        }
    }
    // Reflection mismatches are rejected, including type, layout, set and array extent.
    for (unsigned fault = 0; fault < 4; ++fault)
    {
        auto bad = programs;
        auto& binding = bad[0].shader.interface.bindings[1];
        if (fault == 0) binding.kind = EDescriptorKind::SAMPLED_IMAGE_ARRAY;
        if (fault == 1) binding.block.size = 32;
        if (fault == 2) binding.set = 2;
        if (fault == 3) binding.count = 2;
        CHECK(!compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, bad, extension_test::target));
    }
    auto wrong_block = programs;
    wrong_block[0].shader.stages[1].words.assign(extension_test::wrong_block.begin(), extension_test::wrong_block.end());
    CHECK(!compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, wrong_block, extension_test::target));
    const std::array<float, 4> quarter{0.25f, 0.25f, 0.25f, 0.25f};
    const auto quarter_bytes = std::as_bytes(std::span(quarter));
    const auto quarter_owner = std::make_shared<const std::vector<std::byte>>(quarter_bytes.begin(), quarter_bytes.end());
    upload = take(lux::render::upload(buffer, extension_test::SetBuffer::Request{id, payload(quarter_owner)}));
    complete(upload.second, worker, *session);
    upload.second.reset();
    CHECK(buffer.backingCount() == 1); // Prepared users rebind at publication; completed old backing retires.
    auto* replacement = take(session->prepare(code, extension_test::target, nullptr, nullptr, arguments));
    CHECK(&replacement->code() == &context->code()); // same immutable code, distinct prepared asset bindings
    execute(*context, *session, 32);
    execute(*replacement, *session, 32);
    CHECK(buffer.remove(id));
    CHECK(!session->prepare(code, extension_test::target, nullptr, nullptr, arguments));
    execute(*context, *session, 32);
    execute(*replacement, *session, 32);
    render_allocation::participate = true;
    render_allocation::calls = 0;
    render_allocation::window = true;
    for (unsigned i = 0; i < 32; ++i)
    {
        CHECK(take(context->execute()));
        CHECK(take(replacement->execute()));
        CHECK(take(session->queue().wait(*replacement->lastUse(), UINT64_MAX)));
    }
    render_allocation::window = false;
    CHECK(render_allocation::calls == 0);
    hold_completion = true;
    CHECK(take(context->execute()));
    CHECK(take(replacement->execute()));
    const auto ticket = *replacement->lastUse();
    NativeMemoryAudit::repeatedRebind(*context);
    NativeMemoryAudit::repeatedRebind(*replacement);
    const auto destroyed_before = destroyed_buffers;
    render_allocation::calls = 0;
    render_allocation::window = true;
    CHECK(session->release(context));
    CHECK(session->release(replacement));
    const auto context_release_calls = render_allocation::calls.load();
    CHECK(session->globalStorages().releasePrepared(instance_id));
    render_allocation::window = false;
    std::printf("Buffer owner release allocations: contexts=%zu storage=%zu\n", context_release_calls,
        render_allocation::calls.load() - context_release_calls);
    CHECK(render_allocation::calls == 0);
    CHECK(!session->resources().native(instance_id));
    CHECK(session->collect() && destroyed_buffers == destroyed_before);
    CHECK(session->queue().completed() < ticket.serial());
    hold_completion = false;
    CHECK(take(session->queue().wait(ticket, UINT64_MAX)));
    CHECK(session->collect());
    CHECK(destroyed_buffers == destroyed_before + 3); // current professional version + two Context readback buffers
    // Minimal indirect Material-like owner: a stored Texture reference survives both its creator
    // and its preload. It is the sole future user, and keeps the backing through a REAL copy ticket.
    auto& registry = session->resources();
    auto texture_instance = textures(*session)->id();
    auto* storage = textures(*session);
    auto* indirect = take(storage->acquire(texture));
    auto indirect_storage = take(registry.lease(texture_instance));
    CHECK(storage->release(texture, 1));
    CHECK(session->globalStorages().releasePrepared(texture_instance));
    CHECK(registry.native(texture_instance) == storage);
    const auto copied = read(*session, *indirect);
    CHECK(std::ranges::equal(copied, white));
    storage->releaseLease(*indirect); // read() waited the actual accepted copy ticket
    indirect_storage.reset();
    CHECK(!registry.native(texture_instance));
    CHECK(session->drain());
    code.reset();
    session.reset();
    CHECK(validation.errors == 0);
    std::printf("Texture+StorageBuffer reflected Shader: pixels=64/32; immutable code reused; stable new=0; validation=%u\n", validation.errors.load());
}
