#include "TextureFixture.hpp"
#include <lux/engine/render/native/vulkan/transfer/Transfer.hpp>
#include <lux/engine/render/native/vulkan/transfer/GpuWork.hpp>
#include <semaphore>

static unsigned fail_submit = 0;
static unsigned submits = 0;
static bool hold_completion = false;
static int live_buffers = 0;
static unsigned stopped_publications = 0;
namespace lux::render::vulkan::test
{
    VkResult before(const char* call) noexcept
    {
        if (std::string_view(call) == "fence_status" && hold_completion) return VK_NOT_READY;
        if (std::string_view(call) == "submit" && fail_submit && --fail_submit == 0)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        return VK_SUCCESS;
    }
    VkResult after(const char* call, VkResult result) noexcept
    {
        if (result == VK_SUCCESS && std::string_view(call) == "buffer") ++live_buffers;
        if (result == VK_SUCCESS && std::string_view(call) == "submit") ++submits;
        return result;
    }
    void destroyed(const char* call) noexcept { if (std::string_view(call) == "buffer") --live_buffers; }
}

template<class Work> static void complete(Work& transfer, CpuFixture& host)
{
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (!take(advanceAfterCpu(host, transfer))) { CHECK(Clock::now() < deadline); std::this_thread::yield(); }
}

static std::vector<std::byte> read(DeviceSession& session, BufferPermit& buffer)
{
    const auto* source = session.borrow(buffer);
    auto destination = take(session.createBuffer(source->size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK));
    auto batch = take(session.queue().begin());
    const VkBufferCopy copy{0, 0, source->size()};
    vkCmdCopyBuffer(batch.native(), source->native(), session.borrow(destination)->native(), 1, &copy);
    VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    host.buffer = session.borrow(destination)->native();
    host.size = VK_WHOLE_SIZE;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &host;
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    const auto ticket = take(std::move(batch).submit());
    CHECK(take(session.queue().wait(ticket, UINT64_MAX)));
    std::vector<std::byte> bytes(source->size());
    CHECK(session.read(*session.borrow(destination), 0, bytes));
    CHECK(session.releaseCompleted(destination, ticket));
    CHECK(session.releaseCompleted(buffer, ticket));
    return bytes;
}

int main()
{
    Validation validation;
    CpuFixture host;
    std::vector<std::byte> source(1024 * 1024);
    for (std::size_t i = 0; i < source.size(); ++i) source[i] = std::byte((i * 73 + 17) % 251);
    for (unsigned topology = 0; topology < 3; ++topology)
    {
        auto bootstrap = instance(validation);
        DeviceOptions options;
        options.multiple_queues = topology != 0;
        options.prefer_dedicated_queues = topology == 2;
        auto session = take(DeviceSession::create(bootstrap, options));
        CHECK(session->configureUploads({65536, 1, source.size()}, host.cpu()));
        ImageDescription invalid_image{{4, 4}, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        ImageTransferRegion region{0, 16, 1, 4, {VK_IMAGE_ASPECT_COLOR_BIT, 32, 0, 1}, {}, {4, 4, 1}};
        invalid_image.mip_levels = 33;
        CHECK(!session->uploadImage(invalid_image, source, std::span(&region, 1)));
        invalid_image.mip_levels = 1;
        region.subresource.mipLevel = 0;
        region.subresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        CHECK(!session->uploadImage(invalid_image, source, std::span(&region, 1)));
        region.subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        invalid_image.samples = VK_SAMPLE_COUNT_2_BIT;
        CHECK(!session->uploadImage(invalid_image, source, std::span(&region, 1)));
        CHECK(session->uploadStats().pending_bytes == 0 && live_buffers == 1);
        auto transfer = take(session->uploadBuffer(source, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
        transfer->setCompletionPost(host.post());
        CHECK(!session->uploadBuffer(source, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
        CHECK(!transfer->takeBuffer());
        complete(*transfer, host);
        auto buffer = take(transfer->takeBuffer());
        CHECK(!transfer->takeBuffer());
        transfer.reset();
        CHECK(session->uploadStats().pending_bytes == 0 && session->uploadStats().chunks == 16);
        CHECK(read(*session, buffer) == source);

        // Cancellation cannot destroy the staging/source borrows while the actual CPU worker still owns them.
        struct HoldCpu final
        {
            std::binary_semaphore entered{0}, release{0};
        } held_cpu;
        host.setBeforeWork(&held_cpu, [](void* value) noexcept {
            auto& held = *static_cast<HoldCpu*>(value);
            held.entered.release();
            held.release.acquire();
        });
        transfer = take(session->uploadBuffer(source, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
        transfer->setCompletionPost(host.post());
        CHECK(!take(advanceAfterCpu(host, *transfer)));
        CHECK(held_cpu.entered.try_acquire_for(std::chrono::seconds(10)));
        transfer->cancel();
        CHECK(!take(advanceAfterCpu(host, *transfer)) && !transfer->lastUse());
        CHECK(live_buffers == 2 && session->uploadStats().pending_bytes == source.size());
        held_cpu.release.release();
        complete(*transfer, host);
        CHECK(!transfer->lastUse());
        transfer.reset();
        CHECK(live_buffers == 1 && session->uploadStats().pending_bytes == 0);
        host.setBeforeWork(nullptr, nullptr);

        // The erased Native owner must distinguish stopped preparation from a missing GPU proof.
        host.setBeforeWork(&held_cpu, [](void* value) noexcept {
            auto& held = *static_cast<HoldCpu*>(value);
            held.entered.release();
            held.release.acquire();
        });
        transfer = take(session->uploadBuffer(source, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
        auto stopped = take(GpuWork::pending<[](Transfer&, DeviceSession&) noexcept -> RenderResult<void> {
            ++stopped_publications;
            return {};
        }>(*session, transfer));
        stopped.setCompletionPost(host.post(), {}, {});
        const auto before_stop_submit = submits;
        CHECK(!take(advanceAfterCpu(host, stopped)));
        CHECK(held_cpu.entered.try_acquire_for(std::chrono::seconds(10)));
        stopped.cancel();
        CHECK(!take(advanceAfterCpu(host, stopped)) && !stopped.lastUse());
        CHECK(live_buffers == 2 && session->uploadStats().pending_bytes == source.size());
        CHECK(!stopped.publish());
        held_cpu.release.release();
        complete(stopped, host);
        CHECK(!stopped.lastUse() && submits == before_stop_submit);
        const auto stopped_result = stopped.publish();
        CHECK(!stopped_result && stopped_result.error().type == kWorkStopped && stopped_publications == 0);
        CHECK(!stopped.publish());
        stopped.reset();
        CHECK(live_buffers == 1 && session->uploadStats().pending_bytes == 0);
        host.setBeforeWork(nullptr, nullptr);

        // Stopping publication after Submit preserves the real Ticket and its in-flight backing.
        transfer = take(session->uploadBuffer(source, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
        stopped = take(GpuWork::pending<[](Transfer&, DeviceSession&) noexcept -> RenderResult<void> {
            ++stopped_publications;
            return {};
        }>(*session, transfer));
        stopped.setCompletionPost(host.post(), {}, {});
        hold_completion = true;
        const auto stop_deadline = Clock::now() + std::chrono::seconds(10);
        while (!stopped.lastUse())
        {
            CHECK(!take(advanceAfterCpu(host, stopped)) && Clock::now() < stop_deadline);
            std::this_thread::yield();
        }
        const auto stop_ticket = *stopped.lastUse();
        stopped.cancel();
        CHECK(!take(advanceAfterCpu(host, stopped)) && !stopped.publish());
        CHECK(stopped.lastUse()->serial() == stop_ticket.serial());
        CHECK(&stopped.lastUse()->owner() == &stop_ticket.owner());
        CHECK(live_buffers == 2 && session->uploadStats().pending_bytes == source.size());
        hold_completion = false;
        complete(stopped, host);
        CHECK(stop_ticket.owner().completed() >= stop_ticket.serial());
        const auto submitted_stop_result = stopped.publish();
        CHECK(!submitted_stop_result && submitted_stop_result.error().type == kWorkStopped);
        CHECK(stopped_publications == 0);
        stopped.reset();
        CHECK(live_buffers == 1 && session->uploadStats().pending_bytes == 0);

        transfer = take(session->uploadBuffer(source, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
        transfer->setCompletionPost(host.post());
        fail_submit = 2;
        bool observed_hold = false;
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        for (;;)
        {
            const auto progress = advanceAfterCpu(host, *transfer);
            if (!progress) { CHECK(fail_submit == 0 && transfer->lastUse()); break; }
            if (transfer->lastUse() && !observed_hold)
            {
                hold_completion = true;
                CHECK(!take(advanceAfterCpu(host, *transfer)) && live_buffers == 2);
                hold_completion = false;
                observed_hold = true;
            }
            CHECK(!*progress && Clock::now() < deadline);
            std::this_thread::yield();
        }
        const auto accepted = *transfer->lastUse();
        transfer->cancel();
        CHECK(observed_hold && transfer->lastUse()->serial() == accepted.serial());
        CHECK(live_buffers == 2); // one fixed staging page + the partially submitted candidate
        complete(*transfer, host);
        transfer.reset();
        CHECK(live_buffers == 1 && session->uploadStats().pending_bytes == 0);

        // A later shutdown request must not replace the actual failure after partial GPU admission.
        transfer = take(session->uploadBuffer(source, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
        stopped = take(GpuWork::pending<[](Transfer&, DeviceSession&) noexcept -> RenderResult<void> {
            ++stopped_publications;
            return {};
        }>(*session, transfer));
        stopped.setCompletionPost(host.post(), {}, {});
        fail_submit = 2;
        const auto failure_deadline = Clock::now() + std::chrono::seconds(10);
        while (fail_submit)
        {
            CHECK(!take(advanceAfterCpu(host, stopped)) && Clock::now() < failure_deadline);
            std::this_thread::yield();
        }
        CHECK(stopped.lastUse());
        stopped.cancel();
        complete(stopped, host);
        const auto failed_result = stopped.publish();
        CHECK(!failed_result && failed_result.error().type == kNativeFailure);
        CHECK(failed_result.error().args[0] == static_cast<std::uint32_t>(VK_ERROR_OUT_OF_DEVICE_MEMORY));
        CHECK(stopped_publications == 0);
        stopped.reset();
        CHECK(live_buffers == 1 && session->uploadStats().pending_bytes == 0);

        struct InvalidProof final
        {
            DeviceSession* session;
            std::optional<SubmissionTicket> old;
            bool unstartedIn(const DeviceSession& value) const noexcept { return session == &value; }
            RenderResult<bool> advance() noexcept { return true; }
            void cancel() noexcept {}
            std::optional<SubmissionTicket> lastUse() const noexcept { return old; }
        };
        for (const auto old : {std::optional<SubmissionTicket>{}, std::optional{accepted}})
        {
            auto invalid = std::make_unique<InvalidProof>(session.get(), old);
            auto work = take(GpuWork::pending<[](InvalidProof&, DeviceSession&) noexcept -> RenderResult<void> {
                std::abort(); // a missing/old queue proof must never invoke publication
            }>(*session, invalid));
            CHECK(!work.advance() && !work.publish());
        }
        std::printf("buffer transfer topology=%u graphics=%u:%u transfer=%u:%u bytes=%zu chunks=16 partial-failure retained ticket=%llu\n",
            topology, session->queue().nativeQueue().family, session->queue().nativeQueue().index,
            session->queue(EQueueRole::TRANSFER).nativeQueue().family, session->queue(EQueueRole::TRANSFER).nativeQueue().index,
            source.size(), accepted.serial());
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
        auto source = take(session->createBuffer(input.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, EMemoryAccess::UPLOAD));
        CHECK(session->write(*session->borrow(source), 0, input));
        auto output_a = take(session->createBuffer(input.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK));
        auto output_b = take(session->createBuffer(input.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK));
        const auto copy = [&](SubmissionQueue& queue, BufferPermit& output) {
            auto batch = take(queue.begin());
            const VkBufferCopy region{0, 0, input.size()};
            vkCmdCopyBuffer(batch.native(), session->borrow(source)->native(), session->borrow(output)->native(), 1, &region);
            VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host.buffer = session->borrow(output)->native();
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
        const std::array tickets{a, b};
        CHECK(session->retirement().retire(tickets, source) && !source);
        CHECK(take(session->retirement().collect()) == 0 && live_buffers == 3);
        hold_completion = false;
        CHECK(take(transfer.wait(b, UINT64_MAX)) && take(session->retirement().collect()) == 1);
        std::array<std::byte, 16> a_bytes{}, b_bytes{};
        CHECK(session->read(*session->borrow(output_a), 0, a_bytes));
        CHECK(session->read(*session->borrow(output_b), 0, b_bytes));
        CHECK(a_bytes == input && b_bytes == input);
        CHECK(session->releaseCompleted(output_a, a) && session->releaseCompleted(output_b, b));
        std::printf("independent queue uses retained until BOTH observed tickets; same serial=%d\n", a.serial() == b.serial());
    }
    CHECK(live_buffers == 0 && validation.errors == 0);
    std::printf("Buffer/Image shared Transfer; buffer bytes oracle; validation=0 warnings=%u accepted=%u\n",
        validation.warnings.load(), submits);
}
