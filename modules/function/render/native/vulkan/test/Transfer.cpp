#include "Support.hpp"
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/memory/Memory.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/core/execution/RunLoop.hpp>
#include <array>
#include <cstring>
#include <thread>

using namespace foundation_test;

struct UploadReceiver final
{
    using receiver_concept = stdexec::receiver_t;
    lux::execution::RunLoop& loop;
    bool& done;
    UploadContext::Result& result;

    auto get_env() const noexcept
    {
        return stdexec::env{stdexec::prop{stdexec::get_scheduler, loop.scheduler()}};
    }

    void set_value(UploadContext::Result value) && noexcept
    {
        result = std::move(value);
        done = true;
    }

    void set_error(std::exception_ptr) && noexcept { std::terminate(); }
    void set_stopped() && noexcept { std::terminate(); }
};

static void submittedUpload(const InstanceContext& bootstrap, bool multiple, bool dedicated)
{
    DeviceOptions options;
    options.multiple_queues = multiple;
    options.prefer_dedicated_queues = dedicated;
    options.host_query_reset = true;
    auto session = take(DeviceSession::create(bootstrap, options));
    auto created = lux::execution::ExecutionRuntime::create({2, 16, 128, {128}});
    CHECK(created);
    auto host = std::move(*created);
    lux::execution::RunLoop loop;
    CHECK(session->configureUploads({1024, 1, 4096, true}, host.cpu()));
    // Multi-queue devices enable Timeline: hold actual GPU execution before upload.
    // The single-queue configuration has no Timeline feature; it proves no CPU
    // completion observation is needed, without an invalid host-set pending event.
    VkSemaphore gate = VK_NULL_HANDLE;
    if (multiple)
    {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo create{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
        CHECK(vkCreateSemaphore(session->device().native(), &create, nullptr, &gate) == VK_SUCCESS);
        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        wait.semaphore = gate;
        wait.value = 1;
        wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 block{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        block.waitSemaphoreInfoCount = 1;
        block.pWaitSemaphoreInfos = &wait;
        CHECK(vkQueueSubmit2(session->queue(EQueueRole::TRANSFER).nativeQueue().handle, 1, &block, {}) == VK_SUCCESS);
    }
    auto source = std::make_shared<std::vector<std::byte>>(1024);
    for (std::size_t i = 0; i < source->size(); ++i) (*source)[i] = std::byte(i % 251);
    auto target = take(session->allocator().createBuffer(1024,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        EMemoryAccess::DEVICE));
    auto output = take(session->allocator().createBuffer(1024, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK));
    auto payload = take(RenderPayload::fromVector(source, 0, source->size()));
    bool done = false;
    UploadContext::Result result = lux::cxx::unexpected(RenderError{kBusy});
    auto operation = stdexec::connect(session->uploads().writeBuffer(target, std::move(payload)),
        UploadReceiver{loop, done, result});
    stdexec::start(operation);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done)
    {
        loop.executeReady(64);
        // Deliberately do not poll the GPU: one-chunk success must not require fence observation.
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    CHECK(result);
    auto& graphics = session->queue();
    CHECK(graphics.submitted() > graphics.completed());
    CHECK(session->uploadStats().pending_bytes == 0);
    CHECK(session->uploadStats().gpu_samples == 0);
    if (gate)
    {
        std::uint64_t value = UINT64_MAX;
        CHECK(vkGetSemaphoreCounterValue(session->device().native(), gate, &value) == VK_SUCCESS && value == 0);
        VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        signal.semaphore = gate;
        signal.value = 1;
        CHECK(vkSignalSemaphore(session->device().native(), &signal) == VK_SUCCESS);
    }
    std::weak_ptr<Buffer> retained = target;
    auto batch = take(graphics.begin());
    batch.retain(target);
    batch.retain(output);
    const VkBufferCopy copy{0, 0, 1024};
    vkCmdCopyBuffer(batch.native(), target->native(), output->native(), 1, &copy);
    VkBufferMemoryBarrier2 host_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    host_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    host_barrier.srcQueueFamilyIndex = host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host_barrier.buffer = output->native();
    host_barrier.size = VK_WHOLE_SIZE;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &host_barrier;
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    const auto ticket = take(std::move(batch).submit());
    target.reset();
    CHECK(!retained.expired());
    CHECK(take(graphics.wait(ticket, UINT64_MAX)));
    CHECK(session->drain());
    std::array<std::byte, 1024> actual;
    CHECK(output->read(0, actual));
    CHECK(std::equal(actual.begin(), actual.end(), source->begin()));
    CHECK(session->uploadStats().gpu_query_errors == 0);
    CHECK(session->uploadStats().gpu_samples == 1);
    if (gate) vkDestroySemaphore(session->device().native(), gate, nullptr);
    std::printf("submitted upload multiple=%d dedicated=%d early=1 bytes=1024 samples=1\n", multiple, dedicated);
}

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
        submittedUpload(bootstrap, false, false);
        submittedUpload(bootstrap, true, false);
        submittedUpload(bootstrap, true, true);
    }
    std::printf("transfer pixels=1024x3 validation_errors=%u warnings=%u\n",
        validation.errors.load(), validation.warnings.load());
    CHECK(validation.errors == 0);
}
