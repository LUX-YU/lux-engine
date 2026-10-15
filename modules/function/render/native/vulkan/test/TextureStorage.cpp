#include "TextureFixture.hpp"
#include <lux/engine/render/native/vulkan/texture/TextureStorage.hpp>

static std::string_view fail_call;
static std::array<int, 4> live{};
static constexpr std::array names{"image", "image_view", "texture_sampler", "buffer"};
namespace lux::render::vulkan::test
{
    VkResult before(const char* call) noexcept
    {
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

static void complete(TextureUpload& upload)
{
    const auto end = Clock::now() + std::chrono::seconds(10);
    CHECK(!upload.publish());
    while (!take(upload.advance())) { CHECK(Clock::now() < end); std::this_thread::yield(); }
    CHECK(upload.publish());
    CHECK(!upload.publish());
}

static std::vector<std::byte> read(DeviceSession& session, const TextureVersion& version)
{
    const auto& info = version.info();
    const auto last = info.mip_ranges[info.mip_count - 1];
    auto output = take(session.createBuffer(last.offset + last.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        EMemoryAccess::READBACK));
    auto batch = take(session.queue().begin());
    VkImageMemoryBarrier2 image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    image.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    image.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    image.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    image.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    image.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    image.srcQueueFamilyIndex = image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image.image = version.image().native();
    image.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, info.mip_count, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &image;
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    for (unsigned mip = 0; mip < info.mip_count; ++mip)
    {
        VkBufferImageCopy region{};
        region.bufferOffset = info.mip_ranges[mip].offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        region.imageExtent = {info.mip_ranges[mip].width, info.mip_ranges[mip].height, 1};
        vkCmdCopyImageToBuffer(batch.native(), image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            session.borrow(output)->native(), 1, &region);
    }
    image.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    image.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    image.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    image.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    std::swap(image.oldLayout, image.newLayout);
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = session.borrow(output)->native();
    host.size = VK_WHOLE_SIZE;
    dependency.imageMemoryBarrierCount = 0;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &host;
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    const auto ticket = take(std::move(batch).submit());
    CHECK(take(session.queue().wait(ticket, UINT64_MAX)));
    std::vector<std::byte> bytes(last.offset + last.size);
    CHECK(session.read(*session.borrow(output), 0, bytes));
    CHECK(session.releaseCompleted(output, ticket));
    return bytes;
}

int main()
{
    Validation validation;
    PrepareWorker host;
    {
        auto bootstrap = instance(validation);
        DeviceOptions options;
        options.multiple_queues = true;
        options.texture_descriptors = true;
        options.texture_compression_bc = true;
        auto session = take(DeviceSession::create(bootstrap, options));
        CHECK(session->configureUploads({1024 * 1024, 2, 32 * 1024 * 1024,
            &host, PrepareWorker::enqueue, PrepareWorker::join}));
        for (auto name : {"texture_layout", "texture_pool", "texture_set"})
        {
            fail_call = name;
            CHECK(!session->configureTextures(16, 32 * 1024 * 1024));
            CHECK(fail_call.empty() && !session->textures());
        }
        CHECK(session->configureTextures(16, 32 * 1024 * 1024));
        auto& storage = *session->textures();
        auto info = information(16, 4, false);
        const auto last = info.mip_ranges[3];
        std::vector<std::byte> bytes(last.offset + last.size, std::byte{27});
        auto upload = take(session->uploadTexture(info, bytes));
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
        complete(*upload); upload.reset();
        auto* old = take(storage.acquire(published.texture));
        CHECK(old->revision() == 1 && storage.residentVersions() == 1);
        const auto old_slot = old->descriptorIndex();
        upload = take(session->uploadTexture(info, bytes));
        CHECK(!storage.reserve(*upload, published.texture, 0));
        const auto replacement = take(storage.reserve(*upload, published.texture, 1));
        CHECK(replacement.texture == published.texture && replacement.revision == 2);
        CHECK(storage.find(published.texture) == old);
        CHECK(!storage.release(published.texture, 1)); // writer reservation remains unique
        upload.reset(); // unsubmitted candidate rolls back without changing last-good
        CHECK(storage.find(published.texture) == old && storage.residentVersions() == 1);
        upload = take(session->uploadTexture(info, bytes));
        published = take(storage.reserve(*upload, published.texture, 1));
        complete(*upload); upload.reset();
        CHECK(storage.find(published.texture) != old && old->revision() == 1);
        CHECK(storage.find(published.texture)->descriptorIndex() != old_slot);
        auto* current = take(storage.acquire(published.texture));
        CHECK(storage.release(published.texture, 2));
        CHECK(!storage.release(published.texture, 2) && !storage.acquire(published.texture));
        CHECK(storage.residentVersions() == 2); // both physical generations pinned independently
        storage.releaseLease(*old); storage.releaseLease(*current);
        CHECK(session->collect());
        CHECK(storage.residentVersions() == 0 && storage.residentBytes() == 0);
        upload = take(session->uploadTexture(info, bytes));
        const auto next = take(storage.reserve(*upload));
        CHECK(next.texture.index == published.texture.index && next.texture.gen != published.texture.gen);
        complete(*upload); upload.reset();
        CHECK(!storage.acquire(published.texture));
        CHECK(storage.release(next.texture, 1));
        for (bool bc : {false, true})
        {
            const auto mip_info = information(16, 4, bc);
            const auto end = mip_info.mip_ranges[3];
            std::vector<std::byte> original(end.offset + end.size, std::byte{27});
            upload = take(session->uploadTexture(mip_info, original));
            const auto id = take(storage.reserve(*upload)).texture;
            complete(*upload); upload.reset();
            auto* previous = take(storage.acquire(id));
            const TextureRegion region = bc ? TextureRegion{1, 4, 4, 4, 4, 8} : TextureRegion{1, 2, 1, 3, 2, 16};
            std::vector<std::byte> patch(bc ? 8 : 28, std::byte{73});
            auto invalid = region;
            invalid.row_pitch = UINT64_MAX;
            CHECK(!storage.uploadRegion(id, 1, invalid, patch));
            CHECK(storage.find(id)->revision() == 1);
            upload = take(storage.uploadRegion(id, 1, region, patch));
            CHECK(!storage.uploadRegion(id, 1, region, patch));
            complete(*upload); upload.reset();
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
            upload = take(session->uploadTexture(info, bytes));
            full.push_back(take(storage.reserve(*upload)));
            complete(*upload); upload.reset();
        }
        upload = take(session->uploadTexture(info, bytes));
        CHECK(!storage.reserve(*upload));
        upload.reset();
        CHECK(storage.residentVersions() == 16);
        for (auto value : full) CHECK(storage.release(value.texture, value.revision));
        CHECK(storage.residentVersions() == 0);
        std::printf("texture storage descriptors=16 resident_peak=%llu bytes; exact last-good/generation/leases\n",
            storage.residentPeak());
    }
    CHECK(std::ranges::all_of(live, [](int count) { return count == 0; }));
    CHECK(validation.errors == 0);
    std::printf("texture storage validation_errors=0 warnings=%u\n", validation.warnings.load());
}
