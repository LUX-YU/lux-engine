#include "TextureFixture.hpp"
#include "Allocation.hpp"
#include <lux/engine/render/native/vulkan/material/MaterialStorage.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/render/graph/Builder.hpp>
#include <fstream>

static bool hold_completion = false;
static unsigned pipelines = 0;
static bool fail_submit = false;
static int live_buffers = 0;
namespace lux::render::vulkan::test
{
    VkResult before(const char* call) noexcept
    {
        if (fail_submit && std::string_view(call) == "submit")
        {
            fail_submit = false;
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        return hold_completion && std::string_view(call) == "fence_status" ? VK_NOT_READY : VK_SUCCESS;
    }
    VkResult after(const char* call, VkResult result) noexcept
    {
        if (result == VK_SUCCESS && std::string_view(call) == "buffer") ++live_buffers;
        if (result == VK_SUCCESS && std::string_view(call) == "pipeline") ++pipelines;
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

static void pixels(ExecutionContext& context, DeviceSession& session, unsigned value)
{
    render_allocation::participate = true;
    render_allocation::calls = 0;
    render_allocation::window = true;
    CHECK(take(context.execute()) == EExecutionOutcome::EXECUTED);
    CHECK(take(session.queue().wait(*context.lastUse(), UINT64_MAX)));
    render_allocation::window = false;
    CHECK(render_allocation::calls == 0);
    const auto bytes = take(context.readback());
    for (std::size_t i = 0; i < bytes.size(); ++i)
        CHECK(std::to_integer<unsigned>(bytes[i]) == (i % 4 == 3 ? 255u : value));
}

int main(int argc, char** argv)
{
    CHECK(argc == 3 || argc == 4);
    const bool textured = argc == 4;
    Validation validation;
    CpuFixture host;
    MaterialProgram program{{rdesc::EShaderType::FRAGMENT, "main", spirv(argv[2])}, {}, 1};
    program.texture_count = textured ? 1 : 0;
    program.defaults[0] = {.25f, .25f, .25f, 1};
    auto cooked = take(ConfigValue::make(std::move(program)));
    for (unsigned topology = 0; topology < 3; ++topology)
    {
        auto bootstrap = instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        options.texture_descriptors = textured;
        options.multiple_queues = topology != 0;
        options.prefer_dedicated_queues = topology == 2;
        auto session = take(DeviceSession::create(bootstrap, options));
        CHECK(session->configureUploads({65536, 2, 2 * 1024 * 1024}, host.cpu()));
        CHECK(session->resources().installFactory(materialStorageFactory(), MaterialStorageConfig{}));
        const auto storage_id = take(session->globalStorages().getOrCreate(
            session->resources().typeId(kMaterialStorageType), "material"));
        auto& storage = *take(session->resources().borrow<lux::render::vulkan::MaterialStorage>(storage_id));
        TexturePublished texture{};
        StorageId texture_storage;
        if (textured)
        {
            CHECK(configureTextures(*session, 0, 0));
            texture_storage = textures(*session)->id();
            auto bytes = std::make_shared<const std::vector<std::byte>>(1024, std::byte{255});
            auto upload = take(lux::render::upload(*textures(*session), TextureWrite{
                {textures(*session)->id(), {}}, 0, information(16, 1, false), payload(bytes)
            }));
            complete(upload.second, host, *session);
            texture = upload.first;
        }
        MaterialWrite input{{storage_id, {}}, 0, cooked, cooked.borrow<MaterialProgram>()->defaults};
        input.textures[0] = texture;
        auto bad = input;
        bad.program = take(ConfigValue::make(42));
        CHECK(!lux::render::upload(storage, bad));
        bad = input;
        bad.parameters[0][0] = std::numeric_limits<float>::infinity();
        CHECK(!lux::render::upload(storage, bad));
        if (textured)
        {
            auto wrong_texture = input;
            ++wrong_texture.textures[0].revision;
            CHECK(!lux::render::upload(storage, wrong_texture));
            CHECK(storage.residentVersions() == 0 && textures(*session)->residentVersions() == 1);
        }
        auto cancelled = take(lux::render::upload(storage, input));
        const auto stale = cancelled.first.material;
        cancelled.second.reset();
        CHECK(!storage.query(stale) && storage.residentVersions() == 0);
        auto candidate = take(lux::render::upload(storage, input));
        auto published = candidate.first;
        CHECK(published.material != stale);
        complete(candidate.second, host, *session);
        candidate.second.reset();
        CHECK(take(storage.query(published.material)).parameters == input.parameters);

        const TargetDesc target{16, 16, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
        RenderGraphBuilder builder;
        const auto data = take(builder.declareResource({"material", GraphBufferDesc{416}, EGraphOrigin::IMPORTED,
            {GraphBufferRange{0, 416}}}));
        const auto color = take(builder.declareResource({"color", GraphImageDesc{16, 16}}));
        const auto output = take(builder.declareResource({"pixels", GraphBufferDesc{1024}}));
        std::optional<GraphResourceId> image;
        GraphPass graphics{"draw", EGraphPassKind::GRAPHICS,
            {{{data, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::STORAGE_READ},
             {{color, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::COLOR_ATTACHMENT}}};
        if (textured)
        {
            image = take(builder.declareResource({"albedo", GraphImageDesc{16, 16}, EGraphOrigin::IMPORTED,
                {GraphImageRange{1, 0, 1, 0, 1}}}));
            graphics.uses.push_back({{*image, 0}, {}, EGraphAccess::READ, 0, {}, EGraphUse::SAMPLED});
        }
        CHECK(builder.addPass(std::move(graphics)));
        CHECK(builder.addPass({"read", EGraphPassKind::TRANSFER,
            {{{color, 1}, {}, EGraphAccess::READ, 0, {}, EGraphUse::TRANSFER_SOURCE},
             {{output, 1}, {}, EGraphAccess::WRITE, 0, {}, EGraphUse::TRANSFER_DESTINATION}}}));
        CHECK(builder.exportOutput({{color, 1}, {}, target}));
        CHECK(builder.exportOutput({{output, 1}}));
        auto graph = take(std::move(builder).finish());
        auto analysis = take(analyze(graph));
        PassProgram draw{"draw"};
        draw.shader.stages = {{rdesc::EShaderType::VERTEX, "main", spirv(argv[1])},
            cooked.borrow<MaterialProgram>()->fragment};
        draw.shader.interface.bindings = {{4, 4, EDescriptorKind::STORAGE_BUFFER, 1,
            materialParameterBlock(), "uMats", 2}};
        draw.resources = {{"uMats", "material"}};
        if (textured)
        {
            draw.shader.interface.bindings.push_back({2, 0, EDescriptorKind::SAMPLED_IMAGE_ARRAY, 0, {}, "uTex", 2});
            draw.resources.push_back({"uTex", "albedo"});
        }
        const std::array programs{draw, PassProgram{"read"}};
        GraphStorageBindings bindings;
        bindings.global = {{data, {StorageName(kMaterialStorageType.name()), "material"}}};
        if (textured) bindings.global.push_back({*image, {StorageName(kMaterialStorageType.name()), "material"}});
        const auto pipeline_before = pipelines;
        auto code = std::make_shared<CompiledGraph>(take(compileGraph(
            *session, graph, analysis, bindings, programs, target)));
        const auto pipeline_count = pipelines;
        CHECK(pipeline_count == pipeline_before + 1);
        std::vector arguments{StorageArgument{"material", take(ConfigValue::make(
            MaterialBinding{published.material, cooked}))}};
        if (textured) arguments.push_back({"albedo", take(ConfigValue::make(
            MaterialBinding{published.material, cooked, 0}))});
        auto* first = take(session->prepare(code, target, nullptr, nullptr, arguments));
        auto* second = take(session->prepare(code, target, nullptr, nullptr, arguments));
        pixels(*first, *session, 64);
        pixels(*second, *session, 64);
        auto changed = *cooked.borrow<MaterialProgram>();
        changed.fragment.entry = "other_entry";
        auto wrong = input;
        wrong.material = published.material;
        wrong.expected_revision = 1;
        wrong.program = take(ConfigValue::make(std::move(changed)));
        CHECK(!lux::render::upload(storage, wrong));
        CHECK(take(storage.query(published.material)).published.revision == 1);
        input.material = published.material;
        input.expected_revision = 1;
        input.parameters[0] = {.75f, .75f, .75f, 1};
        auto rejected = take(lux::render::upload(storage, input));
        fail_submit = true;
        WorkCompletion failure;
        failure.start(rejected.second, host);
        finish(failure, host, *session);
        CHECK(!failure.result && !rejected.second.publish());
        rejected.second.reset();
        CHECK(take(storage.query(published.material)).published.revision == 1);
        pixels(*second, *session, 64);
        auto replacement = take(lux::render::upload(storage, input));
        CHECK(!storage.release(published.material, 1));
        complete(replacement.second, host, *session);
        published = replacement.first;
        replacement.second.reset();
        CHECK(pipelines == pipeline_count);
        pixels(*first, *session, 191);
        CHECK(session->release(first));
        pixels(*second, *session, 191);
        CHECK(!storage.release(published.material, 1));
        if (textured)
        {
            auto bytes = std::make_shared<const std::vector<std::byte>>(1024, std::byte{128});
            auto changed_texture = take(lux::render::upload(*textures(*session), TextureWrite{
                texture.texture, texture.revision, information(16, 1, false), payload(bytes)
            }));
            complete(changed_texture.second, host, *session);
            const auto next = changed_texture.first;
            changed_texture.second.reset();
            pixels(*second, *session, 191); // Original material snapshot pins the earlier published version.
            CHECK(textures(*session)->residentVersions() == 2);
            input.expected_revision = published.revision;
            input.textures[0] = next;
            auto material = take(lux::render::upload(storage, input));
            complete(material.second, host, *session);
            published = material.first;
            material.second.reset();
            pixels(*second, *session, 96);
            CHECK(textures(*session)->residentVersions() == 1);
            CHECK(pipelines == pipeline_count);
            CHECK(textures(*session)->release(next.texture, next.revision));
            CHECK(session->globalStorages().remove(texture_storage));
            CHECK(!session->resources().native(texture_storage));
            pixels(*second, *session, 96); // Material pins physical content after the Texture Storage is gone.
        }
        CHECK(take(second->execute()) == EExecutionOutcome::EXECUTED);
        hold_completion = true;
        CHECK(storage.release(published.material, published.revision));
        CHECK(session->release(second));
        CHECK(storage.residentVersions() == 1);
        if (textured)
        {
            CHECK(session->globalStorages().remove(storage_id));
            CHECK(!session->resources().native(storage_id) && !session->resources().native(texture_storage));
            CHECK(live_buffers > 0); // Both logical owners ended while actual Draw completion is unobserved.
        }
        hold_completion = false;
        CHECK(session->drain());
        if (!textured) CHECK(storage.residentVersions() == 0);
        code.reset();

        session.reset();
        CHECK(live_buffers == 0);
    }
    CHECK(validation.errors == 0);
    std::puts("Material Cooked Unlit params pixels64/191, same Pipeline, shared Views, actual retirement PASS");
}


