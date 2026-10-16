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
        "command_pool", "fence", "timeline", "shader", "pipeline_layout", "pipeline", "image_view", "image_alias"};
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

// Test-only forged credentials exercise the boundary without ever dereferencing a dead record.
namespace lux::render::vulkan
{
    struct NativeMemoryAudit final
    {
        template <class Tag> static AllocationPermit<Tag> duplicate(const AllocationPermit<Tag>& value)
        {
            return AllocationPermit<Tag>{*value.owner_, value.key_};
        }
        template <class Tag> static auto key(const AllocationPermit<Tag>& value) { return value.key_; }
    };
}

static void exclusiveRetirement(const InstanceContext& bootstrap)
{
    auto a = take(DeviceSession::create(bootstrap, {}, 3, 1));
    auto b = take(DeviceSession::create(bootstrap, {}, 3, 1));
    CHECK(a->resources().factory(StorageTypeIdView("lux.render.TextureStorage")) == nullptr);
    CHECK(a->resources().size() == 0);
    auto first = take(a->createBuffer(64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::DEVICE));
    auto second = take(a->createBuffer(64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::DEVICE));
    auto foreign = take(b->createBuffer(64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::DEVICE));
    CHECK(NativeMemoryAudit::key(first) == NativeMemoryAudit::key(foreign));
    CHECK(!a->borrow(foreign) && !a->destroyUnsubmitted(foreign) && foreign);
    auto stale = NativeMemoryAudit::duplicate(first);
    auto batch = take(a->queue().begin());
    vkCmdFillBuffer(batch.native(), a->borrow(first)->native(), 0, 64, 17);
    vkCmdFillBuffer(batch.native(), a->borrow(second)->native(), 0, 64, 19);
    auto ticket = take(std::move(batch).submit());
    CHECK(!b->retirement().retire(ticket, first) && first);
    CHECK(!a->retirement().retire(ticket, foreign) && foreign);
    CHECK(a->retirement().retire(ticket, first) && !first);
    CHECK(!a->retirement().retire(ticket, first));
    CHECK(!a->retirement().retire(ticket, stale));
    CHECK(!a->destroyUnsubmitted(stale) && stale);
    CHECK(!a->borrow(stale));
    auto full = a->retirement().retire(ticket, second);
    CHECK(!full && full.error().type == kCapacity && second && a->borrow(second));
    CHECK(take(a->queue().wait(ticket, UINT64_MAX)));
    CHECK(take(a->retirement().collect()) == 1);
    auto replacement = take(a->createBuffer(64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::DEVICE));
    CHECK(NativeMemoryAudit::key(replacement).index == NativeMemoryAudit::key(stale).index);
    CHECK(NativeMemoryAudit::key(replacement).gen != NativeMemoryAudit::key(stale).gen);
    CHECK(!a->destroyUnsubmitted(stale) && !a->retirement().retire(ticket, stale));
    CHECK(a->retirement().retire(ticket, second));
    CHECK(take(a->retirement().collect()) == 1);
    CHECK(a->destroyUnsubmitted(replacement));
    CHECK(b->destroyUnsubmitted(foreign));

    ImageDescription description{{16, 16}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    description.aliasable = true;
    auto original = take(a->createImage(description));
    auto alias = take(a->aliasImage(original));
    auto stale_image = NativeMemoryAudit::duplicate(alias);
    CHECK(!a->aliasImage(alias));
    CHECK(!b->aliasImage(original));
    CHECK(a->retirement().retire(ticket, alias));
    CHECK(!a->retirement().retire(ticket, stale_image));
    CHECK(!a->destroyUnsubmitted(stale_image));
    CHECK(take(a->retirement().collect()) == 1); // alias first, source remains valid
    CHECK(a->borrow(original));
    CHECK(!a->borrow(stale_image));
    CHECK(a->destroyUnsubmitted(original));
    auto new_image = take(a->createImage(description));
    CHECK(!a->destroyUnsubmitted(stale_image));
    CHECK(a->destroyUnsubmitted(new_image));
    std::printf("exclusive retirement: duplicate/stale/cross-session/full retry/image alias rejection PASS\n");
}

static void allocationRecords(DeviceSession& session)
{
    auto upload_permit = take(session.createBuffer(64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, EMemoryAccess::UPLOAD));
    auto* upload = session.borrow(upload_permit);
    std::array<std::byte, 64> bytes{};
    CHECK(session.write(*upload, 0, bytes));
    CHECK(!session.write(*upload, 1, bytes));
    CHECK(!session.read(*upload, 0, bytes));
#ifdef LUX_VULKAN_TEST_SEAM
    fails("flush", [&] { return session.write(*upload, 0, bytes); });
#endif
    CHECK(session.destroyUnsubmitted(upload_permit));
    ImageDescription description{{16, 16}, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    description.aliasable = true;
    auto original_permit = take(session.createImage(description));
    auto* original = session.borrow(original_permit);
#ifdef LUX_VULKAN_TEST_SEAM
    fails("image_alias", [&] { return session.aliasImage(original_permit); });
    fails("image_alias_bind", [&] { return session.aliasImage(original_permit); });
#endif
    auto alias_permit = take(session.aliasImage(original_permit));
    auto* alias = session.borrow(alias_permit);
    const auto a = session.memoryBinding(*original), b = session.memoryBinding(*alias);
    CHECK(a.memory == b.memory && a.offset == b.offset && a.owns_allocation && !b.owns_allocation);
    CHECK(original->native() != alias->native());
    CHECK(session.destroyUnsubmitted(original_permit)); // an alias keeps the actual allocation alive
    auto batch = take(session.queue().begin());
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
    const auto ticket = take(std::move(batch).submit());
    CHECK(session.retirement().retire(ticket, alias_permit));
    CHECK(!alias_permit);
    CHECK(!session.retirement().retire(ticket, alias_permit));
    CHECK(!session.destroyUnsubmitted(alias_permit));
    CHECK(session.drain()); // retires alias and then its pinned allocation exactly once
}

static void actualFenceTimeout(DeviceSession& session)
{
    constexpr VkDeviceSize bytes = 64 * 1024 * 1024;
    auto permit = take(session.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::DEVICE));
    auto* buffer = session.borrow(permit);
    bool observed_timeout = false;
    for (unsigned attempt = 0; attempt < 8 && !observed_timeout; ++attempt)
    {
        auto batch = take(session.queue().begin());
        // A real bounded GPU workload, not a mocked fence status or synthetic completion flag.
        vkCmdFillBuffer(batch.native(), buffer->native(), 0, bytes, attempt);
        const auto ticket = take(std::move(batch).submit());
        observed_timeout = !take(session.queue().wait(ticket, 0));
        CHECK(permit && session.borrow(permit) == buffer);
        CHECK(take(session.queue().wait(ticket, UINT64_MAX)));
    }
    CHECK(observed_timeout);
    CHECK(session.retirement().retire(*session.queue().lastSubmitted(), permit));
    CHECK(take(session.retirement().collect()) == 1);
    std::puts("real zero-deadline Fence timeout observed; backing retained until actual completion");
}

static void exercise(DeviceSession& session)
{
    auto& queue = session.queue();
    auto buffer_permit = take(session.createBuffer( 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        EMemoryAccess::READBACK));
    auto* buffer = session.borrow(buffer_permit);
    auto image_permit = take(session.createImage( {32, 32}, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    auto batch = take(queue.begin());
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
    CHECK(session.read(*buffer, 0, std::as_writable_bytes(std::span(output))));
    for (auto value : output) CHECK(value == 0x12345678);

    // Earlier admitted work retains its ticket when a later submission fails.
#ifdef LUX_VULKAN_TEST_SEAM
    fails("submit", [&] {
        auto next = take(queue.begin());
        return std::move(next).submit();
    });
    CHECK(queue.lastSubmitted()->serial() == ticket.serial());
    auto next = take(queue.begin());
    const auto pending = take(std::move(next).submit());
    const auto owners = live;
    hold_fences = true;
    CHECK(session.retirement().retire(pending, buffer_permit));
    CHECK(!buffer_permit && session.retirement().pending() == 1);
    CHECK(take(session.retirement().collect()) == 0 && live == owners);
    hold_fences = false;
    CHECK(take(queue.wait(pending, UINT64_MAX)));
    CHECK(take(session.retirement().collect()) == 1);
    CHECK(live[4] == owners[4] - 1);
#else
    CHECK(session.retirement().retire(ticket, buffer_permit));
    CHECK(take(session.retirement().collect()) == 1);
#endif
    CHECK(!session.retirement().retire(ticket, buffer_permit));
    CHECK(!session.destroyUnsubmitted(buffer_permit));
    CHECK(session.destroyUnsubmitted(image_permit));
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
        exclusiveRetirement(bootstrap);
        DeviceOptions options;
        options.dynamic_rendering = true;
        {
            auto session = take(DeviceSession::create(bootstrap, options));
            std::printf("GPU=%s API=%u driver=%u Validation=enabled SyncValidation=enabled\n",
                session->device().properties().deviceName, session->device().properties().apiVersion,
                session->device().properties().driverVersion);
            CHECK(&session->queue() == &session->queue(EQueueRole::TRANSFER));
            actualFenceTimeout(*session);
            exercise(*session);
            allocationRecords(*session);
#ifdef LUX_VULKAN_TEST_SEAM
            fails("buffer", [&] { return session->createBuffer( 256,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK); });
            fails("mapping", [&] { return session->createBuffer( 256,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK); });
            fails("image", [&] { return session->createImage( {16, 16}, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT); });
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
            auto* first = take(session->prepare(code, native_fixture::target(execution_config), {}));
            auto* second = take(session->prepare(code, native_fixture::target(execution_config), {}));
            const auto creations = created;
            CHECK(take(first->execute()));
            const auto accepted = *first->lastUse();
            hold_fences = true; // force the admitted first submission to remain unobserved
            fails("submit", [&] { return second->execute(); });
            CHECK(!second->lastUse() && session->queue().lastSubmitted()->serial() == accepted.serial());
            CHECK(created == creations); // execution creates no Shader/Pipeline/Layout/Buffer/Image
            const auto owners = live;
            CHECK(session->release(first));
            CHECK(take(session->retirement().collect()) == 0 && live == owners);
            hold_fences = false;
            CHECK(take(session->queue().wait(accepted, UINT64_MAX)));
            CHECK(session->collect());
            CHECK(live[4] == owners[4] - 1 && live[5] == owners[5] - 1 && live[12] == owners[12] - 1);
            CHECK(session->release(second));
            code.reset();
            std::printf("Graph partial submission: accepted ticket retained, delayed completion retains Context/backing/code\n");
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
