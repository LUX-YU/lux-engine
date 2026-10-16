#include "TextureFixture.hpp"
#include "TextureReadback.hpp"
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>

using SampledBacking = std::remove_pointer_t<std::remove_cvref_t<decltype(std::declval<lux::render::vulkan::TextureStorage>().acquire({}).value())>>;
static_assert(!std::is_copy_constructible_v<SampledBacking> && !std::is_move_constructible_v<SampledBacking>);

static std::string_view fail_call;
static bool hold_completion = false;
static std::array<int, 4> live{};
static constexpr std::array names{"image", "image_view", "texture_sampler", "buffer"};
namespace lux::render::vulkan::test
{
    VkResult before(const char* call) noexcept
    {
        if (std::string_view(call) == "fence_status" && hold_completion) return VK_NOT_READY;
        if (call == fail_call) { fail_call = {}; return VK_ERROR_OUT_OF_HOST_MEMORY; }
        return VK_SUCCESS;
    }
    VkResult after(const char* call, VkResult result) noexcept
    {
        if (result == VK_SUCCESS)
            for (unsigned i = 0; i < names.size(); ++i) if (std::string_view(call) == names[i]) ++live[i];
        return result;
    }
    void destroyed(const char* call) noexcept
    {
        for (unsigned i = 0; i < names.size(); ++i) if (std::string_view(call) == names[i]) --live[i];
    }
}

static void complete(TextureUpload& upload, CpuFixture& host)
{
    const auto end = Clock::now() + std::chrono::seconds(10);
    CHECK(!upload.publish());
    while (!take(advanceAfterCpu(host, upload))) { CHECK(Clock::now() < end); std::this_thread::yield(); }
    CHECK(upload.publish());
    CHECK(!upload.publish());
}


int main()
{
    Validation validation;
    CpuFixture host;
    {
        auto bootstrap = instance(validation);
        DeviceOptions options;
        options.multiple_queues = true;
        options.texture_descriptors = true;
        options.texture_compression_bc = true;
        auto session = take(DeviceSession::create(bootstrap, options));
        CHECK(session->configureUploads({1024 * 1024, 2, 32 * 1024 * 1024}, host.cpu()));
        for (auto name : {"texture_layout", "texture_pool", "texture_set"})
        {
            fail_call = name;
            CHECK(!configureTextures(*session, 16, 32 * 1024 * 1024));
            CHECK(fail_call.empty() && !textures(*session));
        }
        CHECK(configureTextures(*session, 16, 32 * 1024 * 1024));
        auto& storage = *textures(*session);
        auto info = information(16, 4, false);
        const auto last = info.mip_ranges[3];
        std::vector<std::byte> bytes(last.offset + last.size, std::byte{27});
        auto upload = take(TextureUpload::create(*session, info, bytes));
        upload->setCompletionPost(host.post());
        for (auto name : {"image_view", "texture_sampler"})
        {
            const auto previous = live;
            fail_call = name;
            CHECK(!storage.reserve(*upload));
            CHECK(fail_call.empty() && live == previous && storage.residentVersions() == 0);
        }
        auto published = take(storage.reserve(*upload));
        CHECK(!storage.find(published.texture));
        CHECK(!storage.reserve(*upload));
        CHECK(!upload->takeImage());
        complete(*upload, host); upload.reset();
        auto* old = take(storage.acquire(published.texture));
        CHECK(old->revision() == 1 && storage.residentVersions() == 1);
        const auto old_slot = old->descriptorIndex();
        upload = take(TextureUpload::create(*session, info, bytes));
        upload->setCompletionPost(host.post());
        CHECK(!storage.reserve(*upload, published.texture, 0));
        const auto replacement = take(storage.reserve(*upload, published.texture, 1));
        CHECK(replacement.texture == published.texture && replacement.revision == 2);
        CHECK(storage.find(published.texture) == old);
        CHECK(!storage.release(published.texture, 1)); // writer reservation remains unique
        upload.reset(); // unsubmitted candidate rolls back without changing last-good
        CHECK(storage.find(published.texture) == old && storage.residentVersions() == 1);
        upload = take(TextureUpload::create(*session, info, bytes));
        upload->setCompletionPost(host.post());
        published = take(storage.reserve(*upload, published.texture, 1));
        complete(*upload, host); upload.reset();
        CHECK(storage.find(published.texture) != old && old->revision() == 1);
        CHECK(storage.find(published.texture)->descriptorIndex() != old_slot);
        auto* current = take(storage.acquire(published.texture));
        CHECK(storage.release(published.texture, 2));
        CHECK(!storage.release(published.texture, 2) && !storage.acquire(published.texture));
        CHECK(storage.residentVersions() == 2); // both physical generations pinned independently
        storage.releaseLease(*old); storage.releaseLease(*current);
        CHECK(session->collect());
        CHECK(storage.residentVersions() == 0 && storage.residentBytes() == 0);
        upload = take(TextureUpload::create(*session, info, bytes));
        upload->setCompletionPost(host.post());
        const auto next = take(storage.reserve(*upload));
        CHECK(next.texture.index == published.texture.index && next.texture.gen != published.texture.gen);
        complete(*upload, host); upload.reset();
        CHECK(!storage.acquire(published.texture));
        CHECK(storage.release(next.texture, 1));
        // A real accepted first chunk followed by a failed submit cannot publish over last-good.
        {
            auto large_info = information(1024, 1, false);
            std::vector<std::byte> large(4 * 1024 * 1024, std::byte{13});
            upload = take(TextureUpload::create(*session, large_info, large));
            upload->setCompletionPost(host.post());
            const auto id = take(storage.reserve(*upload)).texture;
            complete(*upload, host); upload.reset();
            const auto* last_good = storage.find(id);
            upload = take(TextureUpload::create(*session, large_info, large));
            upload->setCompletionPost(host.post());
            CHECK(storage.reserve(*upload, id, 1));
            const auto end = Clock::now() + std::chrono::seconds(10);
            while (!upload->lastUse()) { CHECK(!take(advanceAfterCpu(host, *upload))); CHECK(Clock::now() < end); }
            const auto first = *upload->lastUse();
            fail_call = "submit";
            for (;;)
            {
                auto result = advanceAfterCpu(host, *upload);
                if (!result) break;
                CHECK(!*result && Clock::now() < end);
            }
            CHECK(fail_call.empty() && upload->lastUse()->serial() == first.serial());
            CHECK(storage.find(id) == last_good && last_good->revision() == 1);
            upload->cancel();
            while (!take(advanceAfterCpu(host, *upload))) CHECK(Clock::now() < end);
            CHECK(!upload->publish()); upload.reset();
            CHECK(storage.find(id) == last_good && storage.residentVersions() == 1);
            CHECK(storage.release(id, 1));
        }
        for (bool bc : {false, true})
        {
            const auto mip_info = information(16, 4, bc);
            const auto end = mip_info.mip_ranges[3];
            std::vector<std::byte> original(end.offset + end.size, std::byte{27});
            upload = take(TextureUpload::create(*session, mip_info, original));
            upload->setCompletionPost(host.post());
            const auto id = take(storage.reserve(*upload)).texture;
            complete(*upload, host); upload.reset();
            auto* previous = take(storage.acquire(id));
            const TextureRegion region = bc ? TextureRegion{1, 4, 4, 4, 4, 8} : TextureRegion{1, 2, 1, 3, 2, 16};
            std::vector<std::byte> patch(bc ? 8 : 28, std::byte{73});
            auto invalid = region;
            invalid.row_pitch = UINT64_MAX;
            CHECK(!storage.uploadRegion(id, 1, invalid, patch));
            invalid = region;
            invalid.x = mip_info.mip_ranges[1].width;
            CHECK(!storage.uploadRegion(id, 1, invalid, patch));
            if (bc)
            {
                invalid = region;
                invalid.x = 1;
                CHECK(!storage.uploadRegion(id, 1, invalid, patch));
            }
            CHECK(storage.find(id)->revision() == 1);
            upload = take(storage.uploadRegion(id, 1, region, patch));
            upload->setCompletionPost(host.post());
            CHECK(!storage.uploadRegion(id, 1, region, patch));
            complete(*upload, host); upload.reset();
            auto expected = original;
            if (bc)
                std::fill_n(expected.begin() + mip_info.mip_ranges[1].offset + 24, 8, std::byte{73});
            else
                for (unsigned row = 0; row < 2; ++row)
                    std::fill_n(expected.begin() + mip_info.mip_ranges[1].offset + (row + 1) * 32 + 8,
                        12, std::byte{73});
            CHECK(read(*session, *previous) == original);
            CHECK(read(*session, *storage.find(id)) == expected);
            CHECK(storage.find(id)->revision() == 2);
            storage.releaseLease(*previous);
            CHECK(storage.release(id, 2));
            CHECK(storage.residentVersions() == 0);
        }
        std::vector<TexturePublished> full;
        for (unsigned i = 0; i < 16; ++i)
        {
            upload = take(TextureUpload::create(*session, info, bytes));
            upload->setCompletionPost(host.post());
            full.push_back(take(storage.reserve(*upload)));
            complete(*upload, host); upload.reset();
        }
        upload = take(TextureUpload::create(*session, info, bytes));
        upload->setCompletionPost(host.post());
        CHECK(!storage.reserve(*upload));
        upload.reset();
        CHECK(storage.residentVersions() == 16);
        for (auto value : full) CHECK(storage.release(value.texture, value.revision));
        CHECK(storage.residentVersions() == 0);
        std::printf("texture storage descriptors=16 resident_peak=%llu bytes; PrivateBacking=%zu TextureUpload=%zu; "
            "exact last-good/generation/leases\n", storage.residentPeak(), sizeof(std::remove_reference_t<decltype(*storage.find({}))>), sizeof(TextureUpload));
    }
    {
        auto bootstrap = instance(validation);
        DeviceOptions options;
        options.texture_descriptors = true;
        auto session = take(DeviceSession::create(bootstrap, options));
        CHECK(session->configureUploads({1024 * 1024, 1, 1024 * 1024}, host.cpu()));
        CHECK(configureTextures(*session, 16, 1));
        std::vector<std::byte> source(1024);
        auto candidate = take(TextureUpload::create(*session, information(16, 1, false), source));
        candidate->setCompletionPost(host.post());
        auto failed = textures(*session)->reserve(*candidate);
        CHECK(!failed && failed.error().type == kCapacity);
        CHECK(!candidate->lastUse() && textures(*session)->residentBytes() == 0);
    }
    {
        auto bootstrap = instance(validation);
        DeviceOptions options;
        options.texture_descriptors = true;
        auto session = take(DeviceSession::create(bootstrap, options));
        CHECK(session->configureUploads({1024 * 1024, 1, 1024 * 1024}, host.cpu()));
        CHECK(configureTextures(*session, 16, 1024 * 1024));
        std::vector<std::byte> source(1024, std::byte{71});
        auto upload = take(TextureUpload::create(*session, information(16, 1, false), source));
        upload->setCompletionPost(host.post());
        CHECK(textures(*session)->reserve(*upload));
        hold_completion = true;
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (!upload->lastUse()) { CHECK(!take(advanceAfterCpu(host, *upload))); CHECK(Clock::now() < deadline); }
        const auto held = live;
        CHECK(session->resources().closeDomain(session->resources().root()));
        CHECK(session->resources().size() == 1 && live == held);
        CHECK(!take(advanceAfterCpu(host, *upload)) && live == held);
        hold_completion = false;
        complete(*upload, host);
        upload.reset();
        CHECK(session->resources().size() == 0 && live[0] == 0 && live[1] == 0 && live[2] == 0);
    }
    CHECK(std::ranges::all_of(live, [](int count) { return count == 0; }));
    CHECK(validation.errors == 0);
    std::printf("texture storage validation_errors=0 warnings=%u\n", validation.warnings.load());
}
