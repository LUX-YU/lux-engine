#include "BufferStorageFixture.hpp"
#include "Support.hpp"
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <cstring>
#include <string_view>
#ifdef LUX_VULKAN_TEST_SEAM
#include "SceneFixture.hpp"
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#endif

using namespace foundation_test;

#ifdef LUX_VULKAN_TEST_SEAM
namespace
{
    constexpr std::array names{"instance", "messenger", "device", "allocator", "buffer", "image",
        "command_pool", "fence", "timeline", "shader", "pipeline_layout", "pipeline", "image_view", "image_memory"};
    std::array<int, names.size()> live{};
    std::array<unsigned, names.size()> created{};
    std::string_view fail_operation;
    int fail_countdown = 0;
    bool hold_fences = false;

    template <class F> void fails(std::string_view name, F&& operation, int occurrence = 1)
    {
        const auto previous = live;
        fail_operation = name;
        fail_countdown = occurrence;
        auto result = operation();
        CHECK(!result);
        CHECK(result.error().type == kNativeFailure);
        CHECK(fail_countdown == 0);
        fail_operation = {};
        CHECK(live == previous);
    }
}

namespace lux::render::vulkan::test
{
    VkResult before(const char* operation) noexcept
    {
        if (hold_fences && std::string_view(operation) == "fence_status") return VK_NOT_READY;
        if (std::string_view(operation) == fail_operation && --fail_countdown == 0)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        return VK_SUCCESS;
    }

    VkResult after(const char* operation, VkResult result) noexcept
    {
        if (result == VK_SUCCESS)
            for (std::size_t i = 0; i < names.size(); ++i)
                if (std::string_view(operation) == names[i]) { ++live[i]; ++created[i]; }
        return result;
    }

    void destroyed(const char* operation) noexcept
    {
        for (std::size_t i = 0; i < names.size(); ++i)
            if (std::string_view(operation) == names[i]) --live[i];
    }
}
#endif

namespace lux::render::vulkan
{
    struct NativeMemoryAudit final
    {
        static std::size_t contexts(const DeviceSession& session) noexcept { return session.contexts_.size(); }
    };
}

// Uses actual allocator-owned objects outside Session identity tables. The queue, not a Permit,
// keeps their Native backing alive once the application drops its references.
static void submittedBufferOwnership(DeviceSession& session)
{
    auto& queue = session.queue();
    auto& allocator = session.allocator();
    std::array<std::uint32_t, 64> input{};
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = 0x12340000u + static_cast<std::uint32_t>(i);
    auto source = take(allocator.createBuffer(sizeof(input), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, EMemoryAccess::UPLOAD));
    auto output = take(allocator.createBuffer(sizeof(input), VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK));
    CHECK(source->write(0, std::as_bytes(std::span(input))));
    std::weak_ptr<Buffer> weak_source = source;
    auto batch = take(queue.begin());
    batch.retain(source);
    batch.retain(output);
    const VkBufferCopy copy{0, 0, sizeof(input)};
    vkCmdCopyBuffer(batch.native(), source->native(), output->native(), 1, &copy);
    VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &host;
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    source.reset();
    CHECK(!weak_source.expired()); // Recording already owns the source.
    auto moved = std::move(batch);
    const auto ticket = take(std::move(moved).submit());
    CHECK(!weak_source.expired()); // No completion observation yet, even on a fast GPU.
#ifdef LUX_VULKAN_TEST_SEAM
    hold_fences = true;
    CHECK(take(queue.poll()) < ticket.serial());
    CHECK(!weak_source.expired());
    fails("submit", [&] {
        auto failed_source = take(allocator.createBuffer(64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::DEVICE));
        std::weak_ptr<Buffer> weak_failed = failed_source;
        auto failed_batch = take(queue.begin());
        failed_batch.retain(failed_source);
        vkCmdFillBuffer(failed_batch.native(), failed_source->native(), 0, 64, 7);
        failed_source.reset();
        auto result = std::move(failed_batch).submit();
        CHECK(weak_failed.expired()); // Only the rejected batch is released.
        CHECK(!weak_source.expired());
        CHECK(queue.lastSubmitted()->serial() == ticket.serial());
        return result;
    });
    hold_fences = false;
#endif
    CHECK(take(queue.wait(ticket, UINT64_MAX)));
    CHECK(weak_source.expired());
    std::array<std::uint32_t, 64> actual{};
    CHECK(output->read(0, std::as_writable_bytes(std::span(actual))));
    CHECK(actual == input);
    std::weak_ptr<Buffer> abandoned;
    {
        auto candidate = take(allocator.createBuffer(64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::DEVICE));
        abandoned = candidate;
        auto recording = take(queue.begin());
        recording.retain(candidate);
        candidate.reset();
        CHECK(!abandoned.expired());
    }
    CHECK(abandoned.expired());
    std::puts("batch ownership: real Copy/readback; recording move/cancel; completion releases source exactly once");
}

static void sharedPhysicalQueues(DeviceSession& session)
{
    auto& graphics = session.queue();
    auto& compute = session.queue(EQueueRole::COMPUTE);
    const std::array families{graphics.nativeQueue().family, compute.nativeQueue().family};
    const auto sharing = families[0] == families[1] ? std::span<const std::uint32_t>{} : std::span(families);
    std::array<std::uint32_t, 16> expected{};
    expected.fill(0xa1b2c3d4u);
    auto source = take(session.allocator().createBuffer(
        sizeof(expected), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, EMemoryAccess::UPLOAD, sharing
    ));
    CHECK(source->write(0, std::as_bytes(std::span(expected))));
    std::weak_ptr<Buffer> weak_source = source;
    auto first_output = take(session.allocator().createBuffer(
        sizeof(expected), VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK
    ));
    auto second_output = take(session.allocator().createBuffer(
        sizeof(expected), VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK
    ));
    const auto copy = [&](SubmissionQueue& queue, const std::shared_ptr<Buffer>& output) {
        auto batch = take(queue.begin());
        batch.retain(source);
        batch.retain(output);
        const VkBufferCopy region{0, 0, sizeof(expected)};
        vkCmdCopyBuffer(batch.native(), source->native(), output->native(), 1, &region);
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(batch.native(), &dependency);
        return take(std::move(batch).submit());
    };
    const auto first = copy(graphics, first_output);
    const auto second = copy(compute, second_output);
    source.reset();
    CHECK(!weak_source.expired());
    CHECK(take(graphics.wait(first, UINT64_MAX)));
    if (&graphics != &compute) CHECK(!weak_source.expired());
    CHECK(take(compute.wait(second, UINT64_MAX)));
    CHECK(weak_source.expired());
    std::array<std::uint32_t, 16> actual{};
    CHECK(first_output->read(0, std::as_writable_bytes(std::span(actual))) && actual == expected);
    CHECK(second_output->read(0, std::as_writable_bytes(std::span(actual))) && actual == expected);
    std::printf("batch ownership: shared Buffer across graphics/compute; distinct_physical_queues=%u\n",
        unsigned(&graphics != &compute));
}

static void batchCapacityAndAlias(const InstanceContext& bootstrap)
{
    auto a = take(DeviceSession::create(bootstrap, {}, 1));
    auto b = take(DeviceSession::create(bootstrap));
    CHECK(a->resources().factory(StorageNameView("lux.render.TextureStorage")) == nullptr);
    CHECK(a->resources().size() == 0);
    auto first = take(a->allocator().createBuffer(64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::DEVICE));
    auto second = take(a->allocator().createBuffer(64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::DEVICE));
    std::weak_ptr<Buffer> retained = first;
    auto batch = take(a->queue().begin());
    batch.retain(first);
    vkCmdFillBuffer(batch.native(), first->native(), 0, 64, 17);
    const auto ticket = take(std::move(batch).submit());
    CHECK(a->queueFor(ticket) == &a->queue() && b->queueFor(ticket) == nullptr);
    CHECK(!b->queue().wait(ticket, 0));
    first.reset();
    CHECK(!retained.expired());
#ifdef LUX_VULKAN_TEST_SEAM
    hold_fences = true;
    const auto owners = live;
    auto full = a->queue().begin();
    CHECK(!full && full.error().type == kCapacity && second);
    CHECK(a->collect() && live == owners && !retained.expired());
    hold_fences = false;
#endif
    CHECK(take(a->queue().wait(ticket, UINT64_MAX)) && retained.expired());
    auto retry = take(a->queue().begin());
    retry.retain(second);
    vkCmdFillBuffer(retry.native(), second->native(), 0, 64, 19);
    const auto retried = take(std::move(retry).submit());
    std::weak_ptr<Buffer> second_retained = second;
    second.reset();
    CHECK(!second_retained.expired());
    CHECK(take(a->queue().wait(retried, UINT64_MAX)) && second_retained.expired());

    ImageDescription description{{16, 16}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    description.aliasable = true;
    auto original = take(a->allocator().createImage(description));
    auto alias = take(a->allocator().aliasImage(*original));
    auto nested = take(a->allocator().aliasImage(*alias));
    const auto memory = original->memoryBinding();
    CHECK(alias->memoryBinding().memory == memory.memory && nested->memoryBinding().memory == memory.memory);
    std::weak_ptr<Image> alias_lifetime = alias, nested_lifetime = nested;
    alias.reset();
    CHECK(alias_lifetime.expired() && original->memoryBinding().memory == memory.memory);
    nested.reset();
    CHECK(nested_lifetime.expired() && original->memoryBinding().memory == memory.memory);
    CHECK(!b->allocator().aliasImage(*original));
    description.aliasable = false;
    auto plain = take(a->allocator().createImage(description));
    CHECK(!a->allocator().aliasImage(*plain));
    std::puts("batch capacity retry, foreign Ticket rejection, nested/alias-first memory sharing PASS");
}

static void compositeBatchLifetime(const InstanceContext& bootstrap)
{
    auto session = take(DeviceSession::create(bootstrap));
    struct Composite final
    {
        std::shared_ptr<Buffer> child;
        unsigned& destroyed;
        ~Composite() noexcept { ++destroyed; }
    };
    unsigned destroyed = 0;
    auto buffer = take(session->allocator().createBuffer(64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::DEVICE));
    std::weak_ptr<Buffer> child = buffer;
    auto composite = std::make_shared<Composite>(std::move(buffer), destroyed);
    auto batch = take(session->queue().begin());
    batch.retain(composite);
    vkCmdFillBuffer(batch.native(), composite->child->native(), 0, 64, 29);
    const auto ticket = take(std::move(batch).submit());
    composite.reset();
    CHECK(destroyed == 0 && !child.expired());
#ifdef LUX_VULKAN_TEST_SEAM
    hold_fences = true;
    CHECK(session->collect() && destroyed == 0 && !child.expired());
    hold_fences = false;
#endif
    CHECK(take(session->queue().wait(ticket, UINT64_MAX)));
    CHECK(destroyed == 1 && child.expired());
    CHECK(session->collect() && destroyed == 1);
    std::puts("submitted composite releases its actual child exactly once, without retirement admission");
}

static void allocationRecords(DeviceSession& session)
{
    auto upload = take(session.allocator().createBuffer(64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, EMemoryAccess::UPLOAD));
    std::array<std::byte, 64> bytes{};
    CHECK(upload->write(0, bytes));
    CHECK(!upload->write(1, bytes));
    CHECK(!upload->read(0, bytes));
#ifdef LUX_VULKAN_TEST_SEAM
    fails("flush", [&] { return upload->write(0, bytes); });
#endif
    upload.reset();
    ImageDescription description{
        {16, 16}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
    };
    description.aliasable = true;
    auto original = take(session.allocator().createImage(description));
    std::weak_ptr<Image> weak_original = original;
#ifdef LUX_VULKAN_TEST_SEAM
    fails("image_alias", [&] { return session.allocator().aliasImage(*original); });
    fails("image_alias_bind", [&] { return session.allocator().aliasImage(*original); });
#endif
    auto alias = take(session.allocator().aliasImage(*original));
    std::weak_ptr<Image> weak_alias = alias;
    const auto a = original->memoryBinding(), b = alias->memoryBinding();
    CHECK(a.memory == b.memory && a.offset == b.offset && a.size == b.size);
    CHECK(original->native() != alias->native());
#ifdef LUX_VULKAN_TEST_SEAM
    const auto before_original_drop = live;
#endif
    original.reset();
    CHECK(weak_original.expired()); // Source VkImage is really gone.
#ifdef LUX_VULKAN_TEST_SEAM
    CHECK(live[5] == before_original_drop[5] - 1);
    CHECK(live[13] == before_original_drop[13]); // Allocation survives the original VkImage.
#endif
    const auto surviving_memory = alias->memoryBinding();
    CHECK(surviving_memory.memory == b.memory && surviving_memory.offset == b.offset);
    auto pixels = take(session.allocator().createBuffer(16 * 16 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK));
    auto batch = take(session.queue().begin());
    batch.retain(alias);
    batch.retain(pixels);
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = alias->native();
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    const VkClearColorValue color{{1, 0, 0, 1}};
    vkCmdClearColorImage(batch.native(), alias->native(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        &color, 1, &barrier.subresourceRange);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {16, 16, 1};
    vkCmdCopyImageToBuffer(
        batch.native(), alias->native(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        pixels->native(), 1, &copy
    );
    VkMemoryBarrier2 host{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo visible{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    visible.memoryBarrierCount = 1;
    visible.pMemoryBarriers = &host;
    vkCmdPipelineBarrier2(batch.native(), &visible);
    const auto ticket = take(std::move(batch).submit());
    alias.reset();
    CHECK(!weak_alias.expired());
#ifdef LUX_VULKAN_TEST_SEAM
    hold_fences = true;
    CHECK(take(session.queue().poll()) < ticket.serial());
    CHECK(!weak_alias.expired());
    hold_fences = false;
#endif
    CHECK(session.drain()); // Batch releases Alias VkImage before its last shared ImageMemory reference.
    CHECK(weak_alias.expired());
    std::array<std::byte, 16 * 16 * 4> readback{};
    CHECK(pixels->read(0, readback));
    for (std::size_t i = 0; i < readback.size(); ++i)
    {
        CHECK(readback[i] == std::byte(i % 4 == 0 || i % 4 == 3 ? 255 : 0));
    }
    pixels.reset();
    CHECK(weak_original.expired());
#ifdef LUX_VULKAN_TEST_SEAM
    CHECK(live[5] == before_original_drop[5] - 2);
    CHECK(live[13] == before_original_drop[13] - 1); // Exactly one shared allocation released.
#endif
}

static void actualFenceTimeout(DeviceSession& session)
{
    constexpr VkDeviceSize bytes = 64 * 1024 * 1024;
    auto buffer = take(session.allocator().createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::DEVICE));
    std::weak_ptr<Buffer> retained = buffer;
    bool observed_timeout = false;
    for (unsigned attempt = 0; attempt < 8 && !observed_timeout; ++attempt)
    {
        auto batch = take(session.queue().begin());
        batch.retain(buffer);
        // A real bounded GPU workload, not a mocked fence status or synthetic completion flag.
        vkCmdFillBuffer(batch.native(), buffer->native(), 0, bytes, attempt);
        const auto ticket = take(std::move(batch).submit());
        observed_timeout = !take(session.queue().wait(ticket, 0));
        CHECK(!retained.expired());
        CHECK(take(session.queue().wait(ticket, UINT64_MAX)));
    }
    CHECK(observed_timeout);
    buffer.reset();
    CHECK(retained.expired());
    std::puts("real zero-deadline Fence timeout observed; backing retained until actual completion");
}

static void exercise(DeviceSession& session)
{
    auto& queue = session.queue();
    auto buffer = take(session.allocator().createBuffer(256, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        EMemoryAccess::READBACK));
    auto batch = take(queue.begin());
    batch.retain(buffer);
    vkCmdFillBuffer(batch.native(), buffer->native(), 0, buffer->size(), 0x12345678);
    VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer->native();
    barrier.size = VK_WHOLE_SIZE;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    const auto ticket = take(std::move(batch).submit());
    CHECK(queue.owns(ticket) && queue.lastSubmitted()->serial() == ticket.serial());
    CHECK(take(queue.wait(ticket, UINT64_MAX)));
    std::array<std::uint32_t, 64> output{};
    CHECK(buffer->read(0, std::as_writable_bytes(std::span(output))));
    for (auto value : output) CHECK(value == 0x12345678);

    // Earlier admitted work retains its ticket when a later submission fails.
#ifdef LUX_VULKAN_TEST_SEAM
    fails("submit", [&] {
        auto next = take(queue.begin());
        return std::move(next).submit();
    });
    CHECK(queue.lastSubmitted()->serial() == ticket.serial());
    auto next = take(queue.begin());
    next.retain(buffer);
    const auto pending = take(std::move(next).submit());
    const auto owners = live;
    hold_fences = true;
    std::weak_ptr<Buffer> retained = buffer;
    buffer.reset();
    CHECK(!retained.expired());
    CHECK(session.collect() && live == owners && !retained.expired());
    hold_fences = false;
    CHECK(take(queue.wait(pending, UINT64_MAX)));
    CHECK(retained.expired() && live[4] == owners[4] - 1);
#else
    buffer.reset();
#endif
    CHECK(session.drain());
}

int main()
{
    Validation validation;
    {
#ifdef LUX_VULKAN_TEST_SEAM
        fails("instance", [&] { return InstanceContext::create({{}, true, diagnostic, &validation}); });
        fails("messenger", [&] { return InstanceContext::create({{}, true, diagnostic, &validation}); });
#endif
        auto bootstrap = instance(validation);
        batchCapacityAndAlias(bootstrap);
        compositeBatchLifetime(bootstrap);
        DeviceOptions options;
        options.dynamic_rendering = true;
        {
            auto session = take(DeviceSession::create(bootstrap, options));
            std::printf("GPU=%s API=%u driver=%u Validation=enabled SyncValidation=enabled\n",
                session->device().properties().deviceName, session->device().properties().apiVersion,
                session->device().properties().driverVersion);
            CHECK(&session->queue() == &session->queue(EQueueRole::TRANSFER));
            submittedBufferOwnership(*session);
            sharedPhysicalQueues(*session);
            actualFenceTimeout(*session);
            exercise(*session);
            allocationRecords(*session);
#ifdef LUX_VULKAN_TEST_SEAM
            fails("buffer", [&] { return session->allocator().createBuffer( 256,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK); });
            fails("mapping", [&] { return session->allocator().createBuffer( 256,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK); });
            fails("image", [&] { return session->allocator().createImage(ImageDescription{{16, 16}, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT}); });
            CHECK(buffer_fixture::install(*session));
            const native_fixture::Config config{32, 32, 1, 1};
            auto assembly = take(native_fixture::assemble(config));
            auto programs = native_fixture::programs(config);
            const auto before = created;
            {
                auto compiled = take(compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, programs, native_fixture::target(config)));
                CHECK(created[4] == before[4] && created[5] == before[5]);
                CHECK(created[11] == before[11] + 1);
            }
            for (auto name : {"pipeline_layout", "shader", "pipeline"})
                fails(name, [&] { return compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, programs, native_fixture::target(config)); });
            fails("shader", [&] { return compileGraph(*session, (assembly).definition, (assembly).analysis, (assembly).external_bindings, programs, native_fixture::target(config)); }, 2);
            std::printf("compile_scope_buffer_allocations=0 compile_scope_image_allocations=0\n");
            const native_fixture::Config execution_config{32, 32, 1, 0};
            auto execution_assembly = take(native_fixture::assemble(execution_config));
            auto code = std::make_shared<CompiledGraph>(take(compileGraph(*session, (execution_assembly).definition, (execution_assembly).analysis, (execution_assembly).external_bindings,
                native_fixture::programs(execution_config), native_fixture::target(execution_config))));
            for (auto name : {"image", "image_view", "buffer", "mapping"})
                fails(name, [&] { return session->prepare(code, native_fixture::target(execution_config), {}); });
            const auto context_records = NativeMemoryAudit::contexts(*session);
            auto* first = take(session->prepare(code, native_fixture::target(execution_config), {}));
            auto* second = take(session->prepare(code, native_fixture::target(execution_config), {}));
            CHECK(NativeMemoryAudit::contexts(*session) == context_records + 2);
            const auto creations = created;
            CHECK(take(first->execute()) == EExecutionOutcome::EXECUTED);
            const auto accepted = *first->lastUse();
            hold_fences = true; // force the admitted first submission to remain unobserved
            fails("submit", [&] { return second->execute(); });
            CHECK(!second->lastUse() && session->queue().lastSubmitted()->serial() == accepted.serial());
            CHECK(created == creations); // execution creates no Shader/Pipeline/Layout/Buffer/Image
            const auto owners = live;
            CHECK(session->release(first));
            CHECK(NativeMemoryAudit::contexts(*session) == context_records + 1);
            CHECK(session->collect() && live == owners); // Only accepted batches own these physical resources.
            CHECK(session->release(second)); // Failed submission retains no physical resource set.
            CHECK(NativeMemoryAudit::contexts(*session) == context_records);
            std::weak_ptr<const CompiledGraph> retained_code = code;
            code.reset();
            CHECK(!retained_code.expired()); // The accepted batch alone owns the pipelines/layout now.
            CHECK(live[4] == owners[4] - 1 && live[5] == owners[5] - 1 && live[12] == owners[12] - 1);
            hold_fences = false;
            CHECK(take(session->queue().wait(accepted, UINT64_MAX)));
            CHECK(session->collect());
            CHECK(live[4] == owners[4] - 2 && live[5] == owners[5] - 2 && live[12] == owners[12] - 2);
            CHECK(retained_code.expired());
            std::printf("Graph partial submission: Context destroyed immediately; accepted batch retains backing/code\n");
#endif
        }
        options.multiple_queues = true;
#ifdef LUX_VULKAN_TEST_SEAM
        for (auto name : {"device", "allocator", "command_pool", "commands", "fence", "timeline"})
            fails(name, [&] { return DeviceSession::create(bootstrap, options); });
        fails("fence", [&] { return DeviceSession::create(bootstrap, options); }, 2);
#endif
        {
            auto session = take(DeviceSession::create(bootstrap, options));
            sharedPhysicalQueues(*session);
            for (unsigned role = 0; role < 3; ++role)
            {
                auto& source = session->queue(static_cast<EQueueRole>(role));
                auto batch = take(source.begin());
                auto source_ticket = take(std::move(batch).submit());
                auto consumer = take(session->queue().begin());
                const SubmissionWait wait{source_ticket, VK_PIPELINE_STAGE_2_COPY_BIT};
                auto joined = take(std::move(consumer).submit({&wait, 1}));
                CHECK(take(session->queue().wait(joined, UINT64_MAX)));
            }
        }
        const VkQueueFamilyProperties family{VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 1};
        auto aliased = take(selectQueues({&family, 1}, true));
        CHECK(aliased[0] == aliased[1] && aliased[1] == aliased[2]);
        options.physical_device_index = UINT32_MAX - 1;
        CHECK(!DeviceSession::create(bootstrap, options));
    }
#ifdef LUX_VULKAN_TEST_SEAM
    for (auto count : live) CHECK(count == 0);
#endif
    std::printf("validation_errors=%u warnings=%u\n", validation.errors.load(), validation.warnings.load());
    CHECK(validation.errors == 0);
}
