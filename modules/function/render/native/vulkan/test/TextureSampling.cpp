#include "TextureFixture.hpp"
#include "TextureSceneFixture.hpp"
#include "Allocation.hpp"
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <cmath>
#include <fstream>

namespace cxx = lux::cxx;

namespace
{
    unsigned pipelines = 0, descriptor_binds = 0, max_error = 0;
    bool hold_observation = false;
}

namespace lux::render::vulkan::test
{
    VkResult before(const char* call) noexcept
    {
        if (hold_observation && std::string_view(call) == "fence_status") return VK_NOT_READY;
        return VK_SUCCESS;
    }

    VkResult after(const char* call, VkResult result) noexcept
    {
        if (result == VK_SUCCESS && std::string_view(call) == "pipeline") ++pipelines;
        if (std::string_view(call) == "cmd_descriptor") ++descriptor_binds;
        return result;
    }

    void destroyed(const char*) noexcept {}
}

static void complete(TextureUpload& upload)
{
    const auto end = Clock::now() + std::chrono::seconds(10);
    while (!take(upload.advance())) { CHECK(Clock::now() < end); std::this_thread::yield(); }
    CHECK(upload.publish());
}

static std::array<unsigned, 4> oracle(std::array<unsigned, 4> rgba, rdesc::ETextureColorSpace space)
{
    if (space == rdesc::ETextureColorSpace::DATA) return rgba;
    std::array<double, 3> linear;
    for (unsigned i = 0; i < 3; ++i)
    {
        const double value = rgba[i] / 255.0;
        linear[i] = space == rdesc::ETextureColorSpace::SRGB ?
            (value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4)) : value;
    }
    constexpr double matrix[3][3]{
        {.6274038959, .3292830384, .0433130657},
        {.0690972894, .9195403951, .0113623156},
        {.0163914389, .0880133079, .8955952532}
    };
    auto result = rgba;
    for (unsigned row = 0; row < 3; ++row)
        result[row] = unsigned(std::lround(255 * (matrix[row][0] * linear[0] +
            matrix[row][1] * linear[1] + matrix[row][2] * linear[2])));
    return result;
}

static void pixels(std::span<const std::byte> actual, std::array<unsigned, 4> expected, unsigned tolerance)
{
    for (std::size_t i = 0; i < actual.size(); ++i)
    {
        const auto value = std::to_integer<int>(actual[i]);
        max_error = std::max(max_error, unsigned(std::abs(value - int(expected[i % 4]))));
        if (std::abs(value - int(expected[i % 4])) > int(tolerance))
        {
            std::fprintf(stderr, "pixel %zu actual=%d expected=%u tolerance=%u\n", i, value, expected[i % 4], tolerance);
            CHECK(false);
        }
    }
}

int main()
{
    Validation validation;
    PrepareWorker host;
    auto bootstrap = instance(validation);
    DeviceOptions options;
    options.dynamic_rendering = true;
    options.multiple_queues = true;
    options.texture_descriptors = true;
    options.texture_compression_bc = true;
    auto session = take(DeviceSession::create(bootstrap, options, 8));
    CHECK(session->configureUploads({1024 * 1024, 2, 32 * 1024 * 1024,
        &host, PrepareWorker::enqueue, PrepareWorker::join}));
    CHECK(configureTextures(*session, 32, 32 * 1024 * 1024));
    auto& storage = *textures(*session);
    std::ofstream raw("texture-sampling.csv");
    raw << "space,mip,views,pipeline_creations,sampled_pixels,tolerance\n";
    for (auto space : {rdesc::ETextureColorSpace::DATA, rdesc::ETextureColorSpace::SRGB,
        rdesc::ETextureColorSpace::LINEAR})
    {
        auto info = information(16, 4, false);
        info.color_space = space;
        if (space == rdesc::ETextureColorSpace::SRGB) info.pixel_format = rdesc::ETexturePixelFormat::RGBA8_SRGB;
        if (space == rdesc::ETextureColorSpace::LINEAR)
            info.flags = rdesc::toUnderlying(rdesc::ETextureAssetFlags::PREMULTIPLIED_ALPHA);
        const auto last = info.mip_ranges[3];
        std::vector<std::byte> input(last.offset + last.size);
        std::array<std::array<unsigned, 4>, 4> values{{{128, 64, 32, 192}, {255, 0, 0, 255},
            {0, 255, 0, 128}, {0, 0, 255, 64}}};
        if (space == rdesc::ETextureColorSpace::LINEAR)
        { values[2] = {0, 128, 0, 128}; values[3] = {0, 0, 64, 64}; }
        for (unsigned mip = 0; mip < 4; ++mip)
            for (std::size_t b = 0; b < info.mip_ranges[mip].size; ++b)
                input[info.mip_ranges[mip].offset + b] = std::byte(values[mip][b % 4]);
        if (space == rdesc::ETextureColorSpace::SRGB)
        {
            auto ambiguous = info;
            ambiguous.flags = rdesc::toUnderlying(rdesc::ETextureAssetFlags::PREMULTIPLIED_ALPHA);
            CHECK(!TextureUpload::create(*session, ambiguous, input));
        }
        auto upload = take(TextureUpload::create(*session, info, input));
        const auto published = take(storage.reserve(*upload));
        complete(*upload); upload.reset();
        for (unsigned mip = 0; mip < 4; ++mip)
        {
            texture_fixture::Config config{published.texture, 16, 16, 4,
                space == rdesc::ETextureColorSpace::SRGB ? rdesc::ETextureFormat::RGBA8_SRGB :
                    rdesc::ETextureFormat::RGBA8_UNORM, mip};
            auto assembly = take(texture_fixture::assemble(config));
            auto bad_programs = texture_fixture::programs(config);
            bad_programs[0].shader.interface.bindings.clear();
            CHECK(!compileGraph(*session, assembly, bad_programs, texture_fixture::target(config)));
            // A structurally valid SPIR-V module with the wrong Descriptor binding fails reflection before Vulkan.
            bad_programs = texture_fixture::programs(config);
            auto& words = bad_programs[0].shader.fragment;
            bool changed = false;
            for (std::size_t i = 5; i < words.size(); i += words[i] >> 16)
                if ((words[i] & 65535) == 71 && (words[i] >> 16) == 4 && words[i + 2] == 33)
                { words[i + 3] = 1; changed = true; }
            CHECK(changed && !compileGraph(*session, assembly, bad_programs, texture_fixture::target(config)));
            const auto old_pipelines = pipelines;
            auto code = std::make_shared<CompiledGraph>(take(compileGraph(*session, assembly,
                texture_fixture::programs(config), texture_fixture::target(config))));
            CHECK(pipelines == old_pipelines + 1);
            // Asset identity/LOD are Attach arguments, never immutable code identity.
            auto invalid = config;
            invalid.texture = {};
            CHECK(!session->prepare(code, texture_fixture::target(config), {}, texture_fixture::arguments(invalid)));
            invalid = config;
            invalid.lod = 4;
            CHECK(!session->prepare(code, texture_fixture::target(config), {}, texture_fixture::arguments(invalid)));
            std::array<ExecutionContext*, 4> views{};
            for (unsigned s = 0; s < 2; ++s)
            {
                const auto scene = take(session->resources().createDomain(session->resources().root(), EResourceScope::SCENE));
                for (unsigned v = 0; v < 2; ++v)
                {
                    const auto view = take(session->resources().createDomain(scene, EResourceScope::VIEW));
                    views[2 * s + v] = take(session->prepare(code, texture_fixture::target(config), view, texture_fixture::arguments(config)));
                    CHECK(&views[2 * s + v]->code() == code.get());
                }
            }
            for (auto* view : views) CHECK(take(view->execute()));
            CHECK(take(session->queue().wait(*views.back()->lastUse(), UINT64_MAX)));
            const auto tolerance = space == rdesc::ETextureColorSpace::DATA ? 0u : 2u;
            for (auto* view : views) pixels(take(view->readback()), oracle(values[mip], space), tolerance);
            render_allocation::participate = true;
            render_allocation::calls = 0;
            render_allocation::window = true;
            for (unsigned frame = 0; frame < 32; ++frame)
            {
                for (auto* view : views) CHECK(take(view->execute()));
                CHECK(take(session->queue().wait(*views.back()->lastUse(), UINT64_MAX)));
            }
            render_allocation::window = false;
            CHECK(render_allocation::calls == 0 && pipelines == old_pipelines + 1);
            raw << unsigned(space) << ',' << mip << ",4,1," << (16 >> mip) * (16 >> mip) * 4 << ',' << tolerance << '\n';
            if (mip == 3)
            {
                // Accepted draws pin the old physical slot, publication refreshes only prepared borrows.
                for (auto* view : views) CHECK(take(view->execute()));
                auto* old = take(storage.acquire(published.texture));
                std::vector<std::byte> replacement(input.size(), std::byte{64});
                upload = take(TextureUpload::create(*session, info, replacement));
                CHECK(storage.reserve(*upload, published.texture, 1));
                complete(*upload); upload.reset();
                CHECK(old->descriptorIndex() != storage.find(published.texture)->descriptorIndex());
                CHECK(storage.residentVersions() == 2);
                storage.releaseLease(*old);
                CHECK(session->collect());
                for (auto* view : views) CHECK(take(view->execute()));
                CHECK(take(session->queue().wait(*views.back()->lastUse(), UINT64_MAX)));
                for (auto* view : views) pixels(take(view->readback()), oracle({64, 64, 64, 64}, space), tolerance);
                CHECK(pipelines == old_pipelines + 1);

                std::array<std::byte, 20> patch;
                patch.fill(std::byte{96});
                upload = take(storage.uploadRegion(published.texture, 2, {3, 0, 0, 2, 2, 12}, patch));
                complete(*upload); upload.reset();
                for (auto* view : views) CHECK(take(view->execute()));
                CHECK(take(session->queue().wait(*views.back()->lastUse(), UINT64_MAX)));
                for (auto* view : views) pixels(take(view->readback()), oracle({96, 96, 96, 96}, space), tolerance);
                CHECK(pipelines == old_pipelines + 1);

                CHECK(storage.release(published.texture, 3));
                // Released business ID cannot be rebound; existing consumers keep valid backing.
                CHECK(!session->prepare(code, texture_fixture::target(config), {}, texture_fixture::arguments(config)));
                for (auto* view : views) CHECK(take(view->execute()));
                CHECK(take(session->queue().wait(*views.back()->lastUse(), UINT64_MAX)));
            }
            for (auto* view : views) CHECK(session->release(view));
            CHECK(session->collect());
        }
        CHECK(storage.residentVersions() == 0);
    }
    // BC1 hardware decompression + SRGB EOTF uses the same compiler/descriptor/consumer path.
    {
        auto info = information(16, 4, true);
        const auto end = info.mip_ranges[3];
        std::vector<std::byte> blocks(end.offset + end.size);
        for (std::size_t i = 0; i < blocks.size(); i += 8)
        { blocks[i] = std::byte{0}; blocks[i + 1] = std::byte{0xf8}; } // RGB565 red, index zero
        auto upload = take(TextureUpload::create(*session, info, blocks));
        const auto id = take(storage.reserve(*upload)).texture;
        complete(*upload); upload.reset();
        texture_fixture::Config config{id, 16, 16, 4, rdesc::ETextureFormat::BC1_RGBA_SRGB, 1};
        auto assembly = take(texture_fixture::assemble(config));
        auto code = std::make_shared<CompiledGraph>(take(compileGraph(*session, assembly,
            texture_fixture::programs(config), texture_fixture::target(config))));
        auto* context = take(session->prepare(code, texture_fixture::target(config), {}, texture_fixture::arguments(config)));
        CHECK(take(context->execute()));
        CHECK(take(session->queue().wait(*context->lastUse(), UINT64_MAX)));
        pixels(take(context->readback()), oracle({255, 0, 0, 255}, rdesc::ETextureColorSpace::SRGB), 2);
        CHECK(storage.release(id, 1));
        CHECK(session->release(context));
    }
    CHECK(session->drain());
    session.reset();
    CHECK(validation.errors == 0);
    std::printf("texture sample: BT2020 double oracle DATA exact SRGB/LINEAR tolerance=2/255; "
        "2 scenes/4 views/4 mips; new=0 stable; descriptor_binds=%u max_error=%u/255 validation_errors=0 warnings=%u\n",
        descriptor_binds, max_error, validation.warnings.load());
}
