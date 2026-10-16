#include "TextureFixture.hpp"
#include "TextureReadback.hpp"
#include "extension/NativeBuffer.hpp"
#include "extension/Feature.hpp"
#include "extension/WrongBlock.hpp"
#include "Allocation.hpp"
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>

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
    CHECK(session->resources().installFactory(extension_test::bufferFactory()));
    CHECK(session->resources().configureType(TestBufferStorage::description, std::make_shared<const TestBufferStorage::Config>()));
    auto instance_id = take(session->resources().getOrCreate(session->resources().root(), "values", TestBufferStorage::description, nullptr));
    auto& buffer = *static_cast<NativeBuffer*>(session->resources().native(instance_id));
    const std::array<float, 4> half{0.5f, 0.5f, 0.5f, 0.5f};
    auto upload = take(buffer.upload(std::as_bytes(std::span(half))));
    upload->setCompletionPost(worker.post());
    const auto id = upload->id;
    while (!take(advanceAfterCpu(worker, *upload))) std::this_thread::yield();
    CHECK(upload->publish());
    upload.reset();
    CHECK(take(session->resources().tryGet<TestBufferStorage>(session->resources().root(), "values")).exists(id));
    std::vector<std::byte> white(1024, std::byte{128});
    auto image = take(TextureUpload::create(*session, information(16, 1, false), white));
    image->setCompletionPost(worker.post());
    const auto texture = take(textures(*session)->reserve(*image)).texture;
    while (!take(advanceAfterCpu(worker, *image))) std::this_thread::yield();
    CHECK(image->publish());
    image.reset();
    auto config = take(encodeWire(extension_test::ViewConfig{texture, id}, {}, extension_test::limits));
    const WireView wire{DataSchema<extension_test::ViewConfig>::schema, config};
    const SelectedFeature selected{&extension_test::definition, &wire, "combined"};
    auto assembly = take(assembleFeatures({&selected, 1}, extension_test::limits));
    auto programs = take(extension_test::programs(&wire, "combined"));
    const auto count_before = session->resources().size();
    auto code = std::make_shared<CompiledGraph>(take(compileGraph(*session, assembly, programs, extension_test::target)));
    CHECK(session->resources().size() == count_before); // compiling cannot create physical scoped instances
    auto arguments = take(programArguments(programs));
    auto* context = take(session->prepare(code, extension_test::target, {}, arguments));
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
        CHECK(!compileGraph(*session, assembly, bad, extension_test::target));
    }
    auto wrong_block = programs;
    wrong_block[0].shader.fragment.assign(extension_test::wrong_block.begin(), extension_test::wrong_block.end());
    CHECK(!compileGraph(*session, assembly, wrong_block, extension_test::target));
    const std::array<float, 4> quarter{0.25f, 0.25f, 0.25f, 0.25f};
    upload = take(buffer.upload(std::as_bytes(std::span(quarter)), id));
    upload->setCompletionPost(worker.post());
    while (!take(advanceAfterCpu(worker, *upload))) std::this_thread::yield();
    CHECK(upload->publish());
    upload.reset();
    CHECK(buffer.backingCount() == 2);
    auto* replacement = take(session->prepare(code, extension_test::target, {}, arguments));
    CHECK(&replacement->code() == &context->code()); // same immutable code, distinct prepared asset bindings
    execute(*context, *session, 64);
    execute(*replacement, *session, 32);
    CHECK(buffer.remove(id));
    CHECK(!session->prepare(code, extension_test::target, {}, arguments));
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
    CHECK(session->release(context));
    CHECK(session->release(replacement));
    CHECK(session->collect());
    CHECK(buffer.backingCount() == 0);
    // Minimal indirect Material-like owner: a stored Texture reference survives both its creator
    // and the Root domain. It is the sole future user, and keeps the backing through a REAL copy ticket.
    auto& registry = session->resources();
    auto texture_instance = take(registry.find(registry.root(), "default", lux::render::TextureStorage::description));
    auto* storage = textures(*session);
    auto* indirect = take(storage->acquire(texture));
    registry.retain(texture_instance);
    CHECK(storage->release(texture, 1));
    CHECK(registry.closeDomain(registry.root()));
    CHECK(registry.native(texture_instance) == storage);
    const auto copied = read(*session, *indirect);
    CHECK(std::ranges::equal(copied, white));
    storage->releaseLease(*indirect); // read() waited the actual accepted copy ticket
    registry.release(texture_instance);
    CHECK(!registry.native(texture_instance));
    CHECK(session->drain());
    code.reset();
    session.reset();
    CHECK(validation.errors == 0);
    std::printf("Texture+StorageBuffer reflected Shader: pixels=64/32; immutable code reused; stable new=0; validation=%u\n", validation.errors.load());
}
