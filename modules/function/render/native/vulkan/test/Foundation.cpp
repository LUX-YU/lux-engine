#include "Support.hpp"
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <cstring>
#include <string_view>
#ifdef LUX_VULKAN_TEST_SEAM
#include "SceneFixture.hpp"
#include <lux/engine/render/native/vulkan/CompiledGraph.hpp>
#endif

using namespace foundation_test;

#ifdef LUX_VULKAN_TEST_SEAM
namespace
{
    constexpr std::array names{"instance", "messenger", "device", "allocator", "buffer", "image",
        "command_pool", "fence", "timeline", "shader", "pipeline_layout", "pipeline"};
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

static void exercise(DeviceSession& session)
{
    auto& queue = session.queue();
    auto buffer = take(Buffer::create(session.allocator, 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        EMemoryAccess::READBACK));
    auto image = take(Image::create(session.allocator, {32, 32}, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT));
    auto batch = take(queue.begin());
    vkCmdFillBuffer(batch.native(), buffer.native(), 0, buffer.size(), 0x12345678);
    VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer.native();
    barrier.size = VK_WHOLE_SIZE;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(batch.native(), &dependency);
    const auto ticket = take(std::move(batch).submit());
    CHECK(queue.owns(ticket) && queue.lastSubmitted()->serial() == ticket.serial());
    CHECK(take(queue.wait(ticket, UINT64_MAX)));
    std::array<std::uint32_t, 64> output{};
    CHECK(buffer.read(0, std::as_writable_bytes(std::span(output))));
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
    CHECK(session.retirement().retire(pending, std::move(buffer)));
    CHECK(!buffer.native() && session.retirement().pending() == 1);
    CHECK(take(session.retirement().collect()) == 0 && live == owners);
    hold_fences = false;
    CHECK(take(queue.wait(pending, UINT64_MAX)));
    CHECK(take(session.retirement().collect()) == 1);
    CHECK(live[4] == owners[4] - 1);
#else
    CHECK(session.retirement().retire(ticket, std::move(buffer)));
    CHECK(take(session.retirement().collect()) == 1);
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
        DeviceOptions options;
        options.dynamic_rendering = true;
        {
            auto session = take(DeviceSession::create(bootstrap, options));
            std::printf("GPU=%s API=%u driver=%u Validation=enabled SyncValidation=enabled\n",
                session->device.properties().deviceName, session->device.properties().apiVersion,
                session->device.properties().driverVersion);
            CHECK(&session->queue() == &session->queue(EQueueRole::TRANSFER));
            exercise(*session);
#ifdef LUX_VULKAN_TEST_SEAM
            fails("buffer", [&] { return Buffer::create(session->allocator, 256,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK); });
            fails("mapping", [&] { return Buffer::create(session->allocator, 256,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::READBACK); });
            fails("image", [&] { return Image::create(session->allocator, {16, 16}, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT); });
            const native_fixture::Config config{32, 32, 1, 1};
            auto assembly = take(native_fixture::assemble(config));
            auto programs = native_fixture::programs(config);
            const auto before = created;
            {
                auto compiled = take(compileGraph(*session, assembly, programs, native_fixture::target(config)));
                CHECK(created[4] == before[4] && created[5] == before[5]);
                CHECK(created[11] == before[11] + 1);
            }
            for (auto name : {"pipeline_layout", "shader", "pipeline"})
                fails(name, [&] { return compileGraph(*session, assembly, programs, native_fixture::target(config)); });
            fails("shader", [&] { return compileGraph(*session, assembly, programs, native_fixture::target(config)); }, 2);
            std::printf("compile_scope_buffer_allocations=0 compile_scope_image_allocations=0\n");
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
