#include "Support.hpp"
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <array>
#include <cstring>

using namespace foundation_test;

static void copyImage(const InstanceContext& bootstrap, bool multiple, bool dedicated)
{
    DeviceOptions options;
    options.multiple_queues = multiple;
    options.prefer_dedicated_queues = dedicated;
    auto session = take(DeviceSession::create(bootstrap, options));
    auto foreign = take(DeviceSession::create(bootstrap));
    auto& transfer = session->queue(EQueueRole::TRANSFER);
    auto& graphics = session->queue();
    const auto source_family = transfer.nativeQueue().family;
    const auto target_family = graphics.nativeQueue().family;
    const bool cross_family = source_family != target_family;
    const bool distinct = &transfer != &graphics;
    std::printf("transfer multiple=%d dedicated=%d graphics=%u:%u transfer=%u:%u alias=%d\n",
        multiple, dedicated, target_family, graphics.nativeQueue().index, source_family,
        transfer.nativeQueue().index, !distinct);
    if (!multiple) CHECK(!distinct);
    if (!dedicated) CHECK(!cross_family);
    std::array<std::byte, 16 * 16 * 4> pixels, result;
    for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = std::byte(i % 251);
    auto upload = take(session->allocator().createBuffer(pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, EMemoryAccess::UPLOAD));
    auto readback = take(session->allocator().createBuffer(pixels.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK));
    auto image = take(session->allocator().createImage(ImageDescription{{16, 16}, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT}));
    CHECK(upload->write(0, pixels));
    auto batch = take(transfer.begin());
    batch.retain(upload);
    batch.retain(image);
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image->native();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {16, 16, 1};
    vkCmdCopyBufferToImage(batch.native(), upload->native(), barrier.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = distinct ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.dstAccessMask = distinct ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (cross_family) { barrier.srcQueueFamilyIndex = source_family; barrier.dstQueueFamilyIndex = target_family; }
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    const auto producer = take(std::move(batch).submit());
    CHECK(session->queueFor(producer) == &transfer && !foreign->queueFor(producer));
    if (distinct) CHECK(!graphics.owns(producer));
    std::weak_ptr<Buffer> retained_upload = upload;
    upload.reset();
    CHECK(!retained_upload.expired());
    auto consumer = take(graphics.begin());
    consumer.retain(image);
    consumer.retain(readback);
    if (distinct)
    {
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        if (!cross_family) barrier.oldLayout = barrier.newLayout;
        vkCmdPipelineBarrier2(consumer.native(), &dependency);
    }
    vkCmdCopyImageToBuffer(consumer.native(), barrier.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        readback->native(), 1, &region);
    VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = readback->native();
    host.size = VK_WHOLE_SIZE;
    dependency.imageMemoryBarrierCount = 0;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &host;
    vkCmdPipelineBarrier2(consumer.native(), &dependency);
    const SubmissionWait wait{producer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    const auto final = take(std::move(consumer).submit(distinct ? std::span(&wait, 1) : std::span<const SubmissionWait>{}));
    std::weak_ptr<Image> retained_image = image;
    image.reset();
    CHECK(!retained_image.expired());
    CHECK(take(graphics.wait(final, UINT64_MAX)));
    CHECK(take(transfer.wait(producer, UINT64_MAX)));
    CHECK(retained_upload.expired() && retained_image.expired());
    CHECK(readback->read(0, result));
    CHECK(result == pixels);

}

int main()
{
    Validation validation;
    {
        auto bootstrap = instance(validation);
        copyImage(bootstrap, false, false);
        copyImage(bootstrap, true, false);
        copyImage(bootstrap, true, true);
    }
    std::printf("transfer pixels=1024x3 validation_errors=%u warnings=%u\n",
        validation.errors.load(), validation.warnings.load());
    CHECK(validation.errors == 0);
}
