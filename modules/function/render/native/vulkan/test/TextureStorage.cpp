#include "TextureFixture.hpp"
#include "TextureReadback.hpp"
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>

using TextureReference = std::remove_cvref_t<decltype(
    std::declval<lux::render::vulkan::TextureStorage>().acquire({}).value()
)>;
using SampledBacking = TextureReference::element_type;
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
        for (auto name : {"descriptor_array_layout", "descriptor_array_pool", "descriptor_array_set"})
        {
            fail_call = name;
            CHECK(!configureTextures(*session, 16, 32 * 1024 * 1024));
            CHECK(fail_call.empty() && !textures(*session));
        }
        CHECK(configureTextures(*session, 16, 32 * 1024 * 1024));
        auto& storage = *textures(*session);
        auto info = information(16, 4, false);
        const auto last = info.mip_ranges[3];
        const auto bytes = std::make_shared<const std::vector<std::byte>>(last.offset + last.size, std::byte{27});
        const SetTexture::Request create{{storage.id(), {}}, 0, info, payload(bytes)};
        for (auto name : {"image_view", "texture_sampler"})
        {
            const auto previous = live;
            fail_call = name;
            CHECK(!lux::render::upload(storage, create));
            CHECK(fail_call.empty() && live == previous && storage.residentVersions() == 0);
        }
        auto upload = take(lux::render::upload(storage, create));
        auto published = upload.first;
        CHECK(!storage.find(published.texture));
        CHECK(!lux::render::upload(storage, SetTexture::Request{published.texture, 1, info, payload(bytes)}));
        CHECK(!upload.second.publish());
        complete(upload.second, host, *session);
        upload.second.reset();
        auto old = take(storage.acquire(published.texture));
        CHECK(old->revision() == 1 && storage.residentVersions() == 1);
        const auto old_slot = old->descriptorIndex();
        CHECK(!lux::render::upload(storage, SetTexture::Request{published.texture, 0, info, payload(bytes)}));
        upload = take(lux::render::upload(storage, SetTexture::Request{published.texture, 1, info, payload(bytes)}));
        const auto replacement = upload.first;
        CHECK(replacement.texture == published.texture && replacement.revision == 2);
        CHECK(storage.find(published.texture) == old.get());
        CHECK(!storage.release(published.texture, 1)); // writer reservation remains unique
        upload.second.reset(); // unsubmitted candidate rolls back without changing last-good
        CHECK(storage.find(published.texture) == old.get() && storage.residentVersions() == 1);
        upload = take(lux::render::upload(storage, SetTexture::Request{published.texture, 1, info, payload(bytes)}));
        published = upload.first;
        complete(upload.second, host, *session);
        upload.second.reset();
        CHECK(storage.find(published.texture) != old.get() && old->revision() == 1);
        CHECK(storage.find(published.texture)->descriptorIndex() != old_slot);
        auto current = take(storage.acquire(published.texture));
        CHECK(storage.release(published.texture, 2));
        CHECK(!storage.release(published.texture, 2) && !storage.acquire(published.texture));
        CHECK(storage.residentVersions() == 2); // both physical generations pinned independently
        old.reset();
        current.reset();
        CHECK(session->collect());
        CHECK(storage.residentVersions() == 0 && storage.residentBytes() == 0);
        upload = take(lux::render::upload(storage, create));
        const auto next = upload.first;
        CHECK(next.texture.local.index == published.texture.local.index && next.texture.local.gen != published.texture.local.gen);
        complete(upload.second, host, *session); upload.second.reset();
        CHECK(!storage.acquire(published.texture));
        CHECK(storage.release(next.texture, 1));
        // A real accepted first chunk followed by a failed submit cannot publish over last-good.
        {
            auto large_info = information(1024, 1, false);
            const auto large = std::make_shared<const std::vector<std::byte>>(4 * 1024 * 1024, std::byte{13});
            upload = take(lux::render::upload(storage, SetTexture::Request{{storage.id(), {}}, 0, large_info, payload(large)}));
            const auto id = upload.first.texture;
            complete(upload.second, host, *session);
            upload.second.reset();
            const auto* last_good = storage.find(id);
            upload = take(lux::render::upload(storage, SetTexture::Request{id, 1, large_info, payload(large)}));
            auto& queue = session->queue(EQueueRole::TRANSFER);
            const auto before_submit = queue.submitted();
            WorkCompletion completion;
            hold_completion = true;
            completion.start(upload.second, host);
            const auto end = Clock::now() + std::chrono::seconds(10);
            while (queue.submitted() == before_submit)
            {
                pump(host, *session);
                CHECK(Clock::now() < end && !completion.done);
            }
            const auto first = *queue.lastSubmitted();
            fail_call = "submit";
            hold_completion = false;
            finish(completion, host, *session);
            CHECK(!completion.result && fail_call.empty() && queue.submitted() == first.serial());
            CHECK(storage.find(id) == last_good && last_good->revision() == 1);
            upload.second.cancel();
            CHECK(!upload.second.publish());
            upload.second.reset();
            CHECK(storage.find(id) == last_good && storage.residentVersions() == 1);
            CHECK(storage.release(id, 1));
        }
        for (bool bc : {false, true})
        {
            const auto mip_info = information(16, 4, bc);
            const auto end = mip_info.mip_ranges[3];
            const auto original_owner = std::make_shared<const std::vector<std::byte>>(end.offset + end.size, std::byte{27});
            const auto& original = *original_owner;
            upload = take(lux::render::upload(storage, SetTexture::Request{{storage.id(), {}}, 0, mip_info, payload(original_owner)}));
            const auto id = upload.first.texture;
            complete(upload.second, host, *session); upload.second.reset();
            auto previous = take(storage.acquire(id));
            const TextureRegion region = bc ? TextureRegion{1, 4, 4, 4, 4, 8} : TextureRegion{1, 2, 1, 3, 2, 16};
            const auto patch = std::make_shared<const std::vector<std::byte>>(bc ? 8 : 28, std::byte{73});
            auto invalid = region;
            invalid.row_pitch = UINT64_MAX;
            CHECK(!lux::render::upload(storage, SetTextureRegion::Request{id, 1, invalid, payload(patch)}));
            invalid = region;
            invalid.x = mip_info.mip_ranges[1].width;
            CHECK(!lux::render::upload(storage, SetTextureRegion::Request{id, 1, invalid, payload(patch)}));
            if (bc)
            {
                invalid = region;
                invalid.x = 1;
                CHECK(!lux::render::upload(storage, SetTextureRegion::Request{id, 1, invalid, payload(patch)}));
            }
            CHECK(storage.find(id)->revision() == 1);
            upload = take(lux::render::upload(storage, SetTextureRegion::Request{id, 1, region, payload(patch)}));
            CHECK(!lux::render::upload(storage, SetTextureRegion::Request{id, 1, region, payload(patch)}));
            complete(upload.second, host, *session); upload.second.reset();
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
            previous.reset();
            CHECK(storage.release(id, 2));
            CHECK(storage.residentVersions() == 0);
        }
        std::vector<TexturePublished> full;
        for (unsigned i = 0; i < 16; ++i)
        {
            upload = take(lux::render::upload(storage, create));
            full.push_back(upload.first);
            complete(upload.second, host, *session); upload.second.reset();
        }
        CHECK(!lux::render::upload(storage, create));
        CHECK(storage.residentVersions() == 16);
        for (auto value : full) CHECK(storage.release(value.texture, value.revision));
        CHECK(storage.residentVersions() == 0);
        std::printf(
            "texture storage descriptors=16 resident_peak=%llu bytes; TextureData=%zu GpuWork=%zu; "
            "exact last-good/generation/shared content\n",
            storage.residentPeak(), sizeof(SampledBacking), sizeof(GpuWork)
        );
    }
    {
        auto bootstrap = instance(validation);
        DeviceOptions options;
        options.texture_descriptors = true;
        auto session = take(DeviceSession::create(bootstrap, options));
        CHECK(session->configureUploads({1024 * 1024, 1, 1024 * 1024}, host.cpu()));
        CHECK(configureTextures(*session, 16, 1));
        const auto source = std::make_shared<const std::vector<std::byte>>(1024);
        const auto submitted = session->queue().submitted();
        auto failed = lux::render::upload(*textures(*session),
            SetTexture::Request{{textures(*session)->id(), {}}, 0, information(16, 1, false), payload(source)});
        CHECK(!failed && failed.error().type == kCapacity);
        CHECK(session->queue().submitted() == submitted && textures(*session)->residentBytes() == 0);
    }
    {
        auto bootstrap = instance(validation);
        DeviceOptions options;
        options.texture_descriptors = true;
        auto session = take(DeviceSession::create(bootstrap, options));
        CHECK(session->configureUploads({1024 * 1024, 1, 1024 * 1024}, host.cpu()));
        CHECK(configureTextures(*session, 16, 1024 * 1024));
        CHECK(session->globalStorages().remove(textures(*session)->id()));
        auto scene = std::make_unique<StorageSet>(session->resources());
        const auto storage_id = take(scene->getOrCreate(session->resources().typeId(kTextureStorageType), "scene"));
        auto* storage = take(session->resources().borrow<lux::render::vulkan::TextureStorage>(storage_id));
        std::weak_ptr<void> storage_lifetime = take(session->resources().share(storage_id));
        const auto source = std::make_shared<const std::vector<std::byte>>(1024, std::byte{71});
        auto upload = take(lux::render::upload(*storage,
            SetTexture::Request{{storage_id, {}}, 0, information(16, 1, false), payload(source)}));
        hold_completion = true;
        const auto before_submit = session->queue().submitted();
        WorkCompletion completion;
        completion.start(upload.second, host);
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (session->queue().submitted() == before_submit)
        {
            pump(host, *session);
            CHECK(!completion.done && Clock::now() < deadline);
        }
        const auto held = live;
        // Revoke the identity while the accepted operation and GPU batch retain their actual objects.
        scene.reset();
        CHECK(session->resources().size() == 0 && live == held && !storage_lifetime.expired());
        CHECK(!session->resources().share(storage_id));
        pump(host, *session);
        CHECK(!completion.done && live == held);
        hold_completion = false;
        finish(completion, host, *session);
        CHECK(completion.result);
        const auto publication = upload.second.publish();
        CHECK(!publication && publication.error().type == kStorageIdentity);
        upload.second.reset();
        CHECK(storage_lifetime.expired());
        CHECK(session->resources().size() == 0 && live[0] == 0 && live[1] == 0 && live[2] == 0);
    }
    {
        auto bootstrap = instance(validation);
        DeviceOptions options;
        options.texture_descriptors = true;
        auto session = take(DeviceSession::create(bootstrap, options));
        CHECK(session->configureUploads({1024 * 1024, 1, 1024 * 1024}, host.cpu()));
        CHECK(configureTextures(*session, 0, 0)); // no implicit 128-version or byte business budget
        auto& storage = *textures(*session);
        const auto source = std::make_shared<const std::vector<std::byte>>(4, std::byte{71});
        std::vector<TexturePublished> values;
        for (unsigned i = 0; i < 129; ++i)
        {
            auto work = take(lux::render::upload(storage,
                SetTexture::Request{{storage.id(), {}}, 0, information(1, 1, false), payload(source)}));
            values.push_back(work.first);
            complete(work.second, host, *session);
            work.second.reset();
        }
        CHECK(storage.residentVersions() == 129);
        CHECK(read(*session, *storage.find(values.front().texture)) == *source);
        CHECK(read(*session, *storage.find(values.back().texture)) == *source);
        for (const auto& value : values) CHECK(storage.release(value.texture, value.revision));
        CHECK(storage.residentVersions() == 0 && storage.residentBytes() == 0);
        std::printf("dynamic Texture growth: 129 real uploads, first/last readback, device descriptor limit=%u\n",
            take(session->sampledImageCapacity()));
    }
    CHECK(std::ranges::all_of(live, [](int count) { return count == 0; }));
    CHECK(validation.errors == 0);
    std::printf("texture storage validation_errors=0 warnings=%u\n", validation.warnings.load());
}
