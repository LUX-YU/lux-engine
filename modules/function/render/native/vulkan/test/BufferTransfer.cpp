#include "StagingArena.hpp"
#include "TextureFixture.hpp"
#include <cstring>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>
#include <semaphore>

static unsigned fail_submit = 0;
static unsigned submits = 0;
static bool hold_completion = false;
static int live_buffers = 0;
static unsigned publications = 0;

namespace lux::render::vulkan::test
{
    VkResult before(const char* call) noexcept
    {
        if (std::string_view(call) == "fence_status" && hold_completion)
        {
            return VK_NOT_READY;
        }
        if (std::string_view(call) == "submit" && fail_submit && --fail_submit == 0)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        return VK_SUCCESS;
    }

    VkResult after(const char* call, VkResult result) noexcept
    {
        if (std::string_view(call) == "cpuPrepare")
        {
            lux::render::test::CpuFixture::runBeforeWork();
        }
        if (result == VK_SUCCESS && std::string_view(call) == "buffer")
        {
            ++live_buffers;
        }
        if (result == VK_SUCCESS && std::string_view(call) == "submit")
        {
            ++submits;
        }
        return result;
    }

    void destroyed(const char* call) noexcept
    {
        if (std::string_view(call) == "buffer")
        {
            --live_buffers;
        }
    }
} // namespace lux::render::vulkan::test

static exec::task<RenderResult<std::shared_ptr<Buffer>>> upload(DeviceSession& session, RenderPayload source)
{
    auto buffer = session.allocator().createBuffer(
        source.bytes().view().size(),
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        EMemoryAccess::DEVICE
    );
    if (!buffer)
    {
        co_return lux::cxx::unexpected(buffer.error());
    }
    auto accepted = co_await session.uploads().writeBuffer(*buffer, std::move(source));
    if (!accepted)
    {
        co_return lux::cxx::unexpected(accepted.error());
    }
    ++publications;
    co_return std::move(*buffer);
}

static std::vector<std::byte> read(DeviceSession& session, std::shared_ptr<Buffer>& buffer)
{
    const auto* source = buffer.get();
    auto destination =
        take(session.allocator().createBuffer(source->size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK)
        );
    auto batch = take(session.queue().begin());
    batch.retain(buffer);
    batch.retain(destination);
    const VkBufferCopy copy{0, 0, source->size()};
    vkCmdCopyBuffer(batch.native(), source->native(), destination->native(), 1, &copy);
    VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = destination->native();
    host.size = VK_WHOLE_SIZE;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &host;
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    const auto ticket = take(std::move(batch).submit());
    CHECK(take(session.queue().wait(ticket, UINT64_MAX)));
    std::vector<std::byte> bytes(source->size());
    CHECK(destination->read(0, bytes));
    destination.reset();
    buffer.reset();
    return bytes;
}

// A page's mapped contents cannot be recycled merely because its CPU producer has returned.
static void stagingLifetime(DeviceSession& session)
{
    const auto before = live_buffers;
    auto& queue = session.queue(EQueueRole::TRANSFER);
    auto arena = take(StagingArena::create(session.allocator(), 256, 2, {}));
    CHECK(arena->available() == 2 && live_buffers == before + 2);
    {
        auto page = take(arena->acquire());
        const auto first_index = page->index();
        auto other = take(arena->acquire());
        CHECK(page->index() != other->index() && !arena->acquire());
        auto output =
            take(session.allocator().createBuffer(256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK));
        std::array<std::byte, 256> source{};
        for (std::size_t i = 0; i < source.size(); ++i)
        {
            source[i] = std::byte((i * 13 + 7) % 251);
        }
        const auto mapped = page->buffer().mappedBytes();
        CHECK(mapped.size() == source.size());
        std::memcpy(mapped.data(), source.data(), source.size());
        CHECK(page->buffer().flush(0, source.size()));
        CHECK(!page->buffer().flush(source.size(), 1));
        auto reservation = session.retirement().reserve();
        const auto pending_before = session.retirement().pending();
        auto batch = take(queue.begin());
        batch.retain(output);
        const VkBufferCopy copy{0, 0, source.size()};
        vkCmdCopyBuffer(batch.native(), page->buffer().native(), output->native(), 1, &copy);
        VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = output->native();
        barrier.size = VK_WHOLE_SIZE;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(batch.native(), &dependency);
        hold_completion = true;
        const auto accepted = take(std::move(batch).submit());
        SubmissionRequirements use;
        use.merge(accepted);
        CHECK(session.retirement().retire(reservation, use, page));
        CHECK(!page && session.collect() && arena->available() == 0);
        CHECK(session.retirement().pending() == pending_before + 1);

        // A rejected later submission returns only its own page, never the earlier accepted page.
        const auto other_index = other->index();
        {
            auto rejected = take(queue.begin());
            fail_submit = 1;
            const auto failure = std::move(rejected).submit();
            CHECK(!failure && failure.error().type == kNativeFailure && fail_submit == 0);
            other.reset(); // Rejected work has no GPU use and releases directly.
        }
        CHECK(arena->available() == 1 && session.retirement().pending() == pending_before + 1);
        auto reused = take(arena->acquire());
        CHECK(reused->index() == other_index && !arena->acquire());
        hold_completion = false;
        CHECK(take(queue.wait(accepted, UINT64_MAX)));
        CHECK(session.collect());
        CHECK(session.retirement().pending() == pending_before && arena->available() == 1);
        auto completed = take(arena->acquire());
        CHECK(completed->index() == first_index);
        std::array<std::byte, 256> readback{};
        CHECK(output->read(0, readback) && readback == source);
    }
    CHECK(arena->available() == 2 && live_buffers == before + 2);
    arena.reset();
    CHECK(live_buffers == before);
    std::printf(
        "staging page retained until actual completion; failed Submit returns only rejected page; "
        "bytes=256 Page=%zu Arena=%zu\n",
        sizeof(StagingPage),
        sizeof(StagingArena)
    );
}

int main()
{
    Validation validation;
    CpuFixture host;
    std::vector<std::byte> source(1024 * 1024);
    for (std::size_t i = 0; i < source.size(); ++i)
    {
        source[i] = std::byte((i * 73 + 17) % 251);
    }
    for (unsigned topology = 0; topology < 3; ++topology)
    {
        auto bootstrap = instance(validation);
        DeviceOptions options;
        options.multiple_queues = topology != 0;
        options.prefer_dedicated_queues = topology == 2;
        auto session = take(DeviceSession::create(bootstrap, options));
        stagingLifetime(*session);
        CHECK(session->configureUploads({65536, 1, source.size()}, host.cpu()));
        auto source_owner = std::make_shared<const std::vector<std::byte>>(source);
        const auto source_payload = payload(source_owner);
        ImageDescription invalid_image{
            {4, 4},
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
        };
        invalid_image.mip_levels = 33;
        CHECK(!session->allocator().createImage(invalid_image));
        invalid_image.mip_levels = 1;
        auto image = take(session->allocator().createImage(invalid_image));
        std::vector<ImageTransferRegion> regions{{0, 16, 1, 4, {VK_IMAGE_ASPECT_COLOR_BIT, 32, 0, 1}, {}, {4, 4, 1}}};
        const auto no_submit = submits;
        CHECK(!runUpload(session->uploads().writeImage(image, source_payload, regions), host, *session));
        regions[0].subresource.mipLevel = 0;
        regions[0].subresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        CHECK(!runUpload(session->uploads().writeImage(image, source_payload, regions), host, *session));
        regions[0].subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[0].source_offset = UINT64_MAX;
        CHECK(!runUpload(session->uploads().writeImage(image, source_payload, regions), host, *session));
        CHECK(submits == no_submit && session->uploadStats().pending_bytes == 0 && live_buffers == 1);
        image.reset();
        auto buffer = take(runUpload(upload(*session, source_payload), host, *session));
        CHECK(session->uploadStats().pending_bytes == 0 && session->uploadStats().chunks == 16);
        CHECK(read(*session, buffer) == source);
        CHECK(session->drain());

        // Cancellation cannot destroy the source or staging while a real CPU worker owns its borrow.
        // Saturating retained bytes/page admission rejects a second task without publishing anything.
        struct HoldCpu final
        {
            std::binary_semaphore entered{0}, release{0};
        } held_cpu;

        const auto blockCpu = [](void* value) noexcept
        {
            auto& held = *static_cast<HoldCpu*>(value);
            held.entered.release();
            held.release.acquire();
        };
        host.setBeforeWork(&held_cpu, blockCpu);
        const auto before_stop_submit = submits;
        const auto before_publication = publications;
        UploadCompletion<std::shared_ptr<Buffer>> stopped_before_submit;
        stopped_before_submit.start(upload(*session, source_payload), host);
        CHECK(held_cpu.entered.try_acquire_for(std::chrono::seconds(10)));
        auto rejected = runUpload(upload(*session, source_payload), host, *session);
        CHECK(!rejected && rejected.error().type == kCapacity);
        stopped_before_submit.cancel();
        pump(host, *session);
        CHECK(!stopped_before_submit.done && submits == before_stop_submit);
        CHECK(live_buffers == 2 && session->uploadStats().pending_bytes == source.size());
        CHECK(source_payload.bytes().view().data() == source_owner->data());
        held_cpu.release.release();
        finish(stopped_before_submit, host, *session);
        CHECK(!stopped_before_submit.result && stopped_before_submit.result.error().type == kWorkStopped);
        CHECK(submits == before_stop_submit && publications == before_publication);
        CHECK(live_buffers == 1 && session->uploadStats().pending_bytes == 0);
        host.setBeforeWork(nullptr, nullptr);

        // After real admission, cancellation means unknown side effects, never a guaranteed no-op.
        // The first accepted Ticket/backing survives while its completion observation is held.
        auto& copy_queue = session->queue(EQueueRole::TRANSFER);
        const auto before_cancel_serial = copy_queue.submitted();
        UploadCompletion<std::shared_ptr<Buffer>> stopped_after_submit;
        hold_completion = true;
        stopped_after_submit.start(upload(*session, source_payload), host);
        const auto stop_deadline = Clock::now() + std::chrono::seconds(10);
        while (copy_queue.submitted() == before_cancel_serial)
        {
            pump(host, *session);
            CHECK(!stopped_after_submit.done && Clock::now() < stop_deadline);
            std::this_thread::yield();
        }
        const auto stop_ticket = *copy_queue.lastSubmitted();
        stopped_after_submit.cancel();
        pump(host, *session);
        CHECK(!stopped_after_submit.done && copy_queue.submitted() == stop_ticket.serial());
        CHECK(live_buffers == 2 && session->uploadStats().pending_bytes == source.size());
        hold_completion = false;
        finish(stopped_after_submit, host, *session);
        CHECK(!stopped_after_submit.result && stopped_after_submit.result.error().type == kOutcomeUnknown);
        CHECK(copy_queue.completed() >= stop_ticket.serial() && publications == before_publication);
        CHECK(session->collect() && live_buffers == 1 && session->uploadStats().pending_bytes == 0);

        // A rejected second chunk does not overwrite the first accepted Ticket or publish the candidate.
        const auto before_failure_serial = copy_queue.submitted();
        UploadCompletion<std::shared_ptr<Buffer>> failure;
        fail_submit = 2;
        hold_completion = true;
        failure.start(upload(*session, source_payload), host);
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (copy_queue.submitted() == before_failure_serial)
        {
            pump(host, *session);
            CHECK(!failure.done && Clock::now() < deadline);
            std::this_thread::yield();
        }
        const auto accepted = *copy_queue.lastSubmitted();
        pump(host, *session);
        CHECK(!failure.done && live_buffers == 2 && fail_submit == 1);
        hold_completion = false;
        finish(failure, host, *session);
        CHECK(!failure.result && fail_submit == 0 && copy_queue.submitted() == accepted.serial());
        CHECK(failure.result.error().type == kNativeFailure);
        CHECK(failure.result.error().args[0] == static_cast<std::uint32_t>(VK_ERROR_OUT_OF_DEVICE_MEMORY));
        failure.cancel(); // Cannot rewrite a terminal failure into cancellation.
        CHECK(failure.result.error().type == kNativeFailure && publications == before_publication);
        CHECK(copy_queue.completed() >= accepted.serial());
        CHECK(session->collect() && live_buffers == 1 && session->uploadStats().pending_bytes == 0);

        std::printf(
            "buffer upload topology=%u graphics=%u:%u transfer=%u:%u bytes=%zu chunks=16 "
            "partial-failure retained ticket=%llu\n",
            topology,
            session->queue().nativeQueue().family,
            session->queue().nativeQueue().index,
            copy_queue.nativeQueue().family,
            copy_queue.nativeQueue().index,
            source.size(),
            accepted.serial()
        );
    }
    {
        auto bootstrap = instance(validation);
        DeviceOptions options;
        options.multiple_queues = true;
        options.prefer_dedicated_queues = false;
        auto session = take(DeviceSession::create(bootstrap, options));
        auto& graphics = session->queue();
        auto& transfer = session->queue(EQueueRole::TRANSFER);
        CHECK(&graphics != &transfer && graphics.nativeQueue().family == transfer.nativeQueue().family);
        const std::array<std::byte, 16> input{std::byte{73}};
        auto source = take(
            session->allocator().createBuffer(input.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, EMemoryAccess::UPLOAD)
        );
        CHECK(source->write(0, input));
        auto output_a = take(
            session->allocator().createBuffer(input.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK)
        );
        auto output_b = take(
            session->allocator().createBuffer(input.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK)
        );
        const auto copy = [&](SubmissionQueue& queue, const std::shared_ptr<Buffer>& output)
        {
            auto batch = take(queue.begin());
            batch.retain(source);
            batch.retain(output);
            const VkBufferCopy region{0, 0, input.size()};
            vkCmdCopyBuffer(batch.native(), source->native(), output->native(), 1, &region);
            VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.buffer = output->native();
            host.size = VK_WHOLE_SIZE;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &host;
            vkCmdPipelineBarrier2(batch.native(), &dependency);
            return take(std::move(batch).submit());
        };
        const auto a = copy(graphics, output_a), b = copy(transfer, output_b);
        CHECK(take(graphics.wait(a, UINT64_MAX)));
        hold_completion = true;
        std::weak_ptr<Buffer> retained_source = source;
        source.reset();
        CHECK(session->collect());
        CHECK(!retained_source.expired() && live_buffers == 3);
        hold_completion = false;
        CHECK(take(transfer.wait(b, UINT64_MAX)));
        CHECK(retained_source.expired());
        std::array<std::byte, 16> a_bytes{}, b_bytes{};
        CHECK(output_a->read(0, a_bytes));
        CHECK(output_b->read(0, b_bytes));
        CHECK(a_bytes == input && b_bytes == input);
        output_a.reset();
        output_b.reset();
        std::printf(
            "independent queue uses retained until BOTH observed tickets; same serial=%d\n",
            a.serial() == b.serial()
        );
    }
    CHECK(live_buffers == 0 && validation.errors == 0);
    std::printf(
        "Buffer/Image UploadContext; buffer bytes oracle; validation=0 warnings=%u accepted=%u\n",
        validation.warnings.load(),
        submits
    );
}
