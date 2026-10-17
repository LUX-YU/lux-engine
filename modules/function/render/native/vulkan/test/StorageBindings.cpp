#include "TextureFixture.hpp"
#include "TextureReadback.hpp"
#include "extension/NativeBuffer.hpp"
#include "extension/Feature.hpp"
#include "extension/WrongBlock.hpp"
#include "Allocation.hpp"
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>

static bool hold_completion = false;
static unsigned destroyed_buffers = 0;
namespace lux::render::vulkan::test
{
    VkResult before(const char* operation) noexcept
    {
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
    auto* context = take(session->prepare(code, extension_test::target, nullptr, nullptr, arguments));
    execute(*context, *session, 64);
    // Reflection mismatches are rejected, including type, layout, set and array extent.
    for (unsigned fault = 0; fault < 4; ++fault)
    {
        auto bad = programs;
        auto& binding = bad[0].shader.interface.bindings[1];
        if (fault == 0) binding.kind = EDescriptorKind::SAMPLED_IMAGE_ARRAY;
        if (fault == 1) binding.layout_bytes = 32;
        if (fault == 2) binding.set = 2;
        if (fault == 3) binding.count = 2;
        CHECK(!compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, bad, extension_test::target));
    }
    auto wrong_block = programs;
    wrong_block[0].shader.fragment.assign(extension_test::wrong_block.begin(), extension_test::wrong_block.end());
    CHECK(!compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, wrong_block, extension_test::target));
    const std::array<float, 4> quarter{0.25f, 0.25f, 0.25f, 0.25f};
    const auto quarter_bytes = std::as_bytes(std::span(quarter));
    const auto quarter_owner = std::make_shared<const std::vector<std::byte>>(quarter_bytes.begin(), quarter_bytes.end());
    upload = take(lux::render::upload(buffer, extension_test::SetBuffer::Request{id, payload(quarter_owner)}));
    complete(upload.second, worker, *session);
    upload.second.reset();
    CHECK(buffer.backingCount() == 2);
    auto* replacement = take(session->prepare(code, extension_test::target, nullptr, nullptr, arguments));
    CHECK(&replacement->code() == &context->code()); // same immutable code, distinct prepared asset bindings
    execute(*context, *session, 64);
    execute(*replacement, *session, 32);
    CHECK(buffer.remove(id));
    CHECK(!session->prepare(code, extension_test::target, nullptr, nullptr, arguments));
    execute(*context, *session, 64);
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
    CHECK(destroyed_buffers == destroyed_before + 4); // two professional versions + two Context readback buffers
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
