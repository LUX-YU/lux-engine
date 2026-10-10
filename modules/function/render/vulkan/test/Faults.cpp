#include "Support.hpp"

#include <lux/engine/render/vulkan/retirement/Retirement.hpp>
#include <lux/engine/render/vulkan/transfer/Transfer.hpp>
#include <array>
#include <cstring>
#include <fstream>
#include <exception>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace foundation_test;

namespace
{
    constexpr std::array names{
        "instance",
        "messenger",
        "device",
        "allocator",
        "buffer",
        "image",
        "descriptor_layout",
        "descriptor_pool",
        "shader",
        "pipeline_layout",
        "pipeline",
        "command_pool",
        "fence"
    };
    std::array<int, names.size()> live{};
    const char *fail_operation = "";
    int fail_countdown = 0;
    bool partial_pipeline = false;
    bool hold_fences = false;
    bool lose_on_submit = false;
    std::size_t failures = 0;

    int index(std::string_view name)
    {
        for (int i = 0; i < static_cast<int>(names.size()); ++i)
            if (name == names[i])
                return i;
        return -1;
    }

    template <class F> void fails(const char *name, F &&operation, int occurrence = 1)
    {
        const auto before = live;
        fail_operation = name;
        fail_countdown = occurrence;
        auto result = operation();
        CHECK(!result);
        CHECK(result.error().type == kNativeFailure);
        CHECK(result.error().args[0] == static_cast<std::uint32_t>(VK_ERROR_OUT_OF_HOST_MEMORY));
        CHECK(fail_countdown == 0);
        fail_operation = "";
        CHECK(live == before);
        ++failures;
    }

    template <class F> void moves(F &&create)
    {
        const auto before = live;
        {
            auto first = take(create());
            using T = decltype(first);
            static_assert(!std::is_copy_constructible_v<T>);
            static_assert(std::is_nothrow_move_constructible_v<T>);
            auto second = std::move(first);
            CHECK(!first.native() && second.native());
            auto third = take(create());
            third = std::move(second);
            CHECK(!second.native() && third.native());
        }
        CHECK(live == before);
    }
} // namespace

namespace lux::render::vulkan::test
{
    VkResult before(const char *operation) noexcept
    {
        if (lose_on_submit && std::string_view(operation) == "submit")
            return VK_ERROR_DEVICE_LOST;
        if (hold_fences && std::string_view(operation) == "fence_status")
            return VK_NOT_READY;
        if (std::string_view(operation) == fail_operation && --fail_countdown == 0)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        return VK_SUCCESS;
    }

    VkResult after(const char *operation, VkResult result) noexcept
    {
        const auto i = index(operation);
        if (result == VK_SUCCESS && i >= 0)
            ++live[i];
        if (partial_pipeline && std::string_view(operation) == "pipeline" && result == VK_SUCCESS)
        {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        return result;
    }

    void destroyed(const char *operation) noexcept
    {
        const auto i = index(operation);
        CHECK(i >= 0 && live[i] > 0);
        --live[i];
        // These checks cover the actual factory/lexical teardown order. They are
        // internal observation only, not a production ownership registry.
        if (std::string_view(operation) == "allocator")
            CHECK(live[4] == 0 && live[5] == 0);
        if (std::string_view(operation) == "device")
        {
            for (std::size_t j = 3; j < live.size(); ++j)
                CHECK(live[j] == 0);
        }
        if (std::string_view(operation) == "instance" && live[0] == 0)
            CHECK(live[1] == 0 && live[2] == 0);
    }
} // namespace lux::render::vulkan::test

int main(int argc, char **argv)
{
    if (argc == 3)
    {
        std::set_terminate([] { std::_Exit(86); });
        auto instance_owner = take(VulkanInstance::create());
        auto device = take(VulkanDevice::create(instance_owner));
        auto allocator = take(VulkanAllocator::create(device));
        auto queue = take(SubmissionQueue::create(device, 1));
        auto arena = take(StagingArena::create(*queue, device, allocator, 65536));
        auto batch = take(queue->begin());
        // An outstanding CPU borrow is an invariant failure even with NDEBUG.
        queue.reset();
        return 1;
    }
    CHECK(argc == 2);
    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    CHECK(input);
    const auto bytes = static_cast<std::size_t>(input.tellg());
    std::vector<std::uint32_t> spirv(bytes / 4);
    input.seekg(0);
    input.read(reinterpret_cast<char *>(spirv.data()), static_cast<std::streamsize>(bytes));
    CHECK(input && bytes % 4 == 0);
    Validation validation;
    const InstanceOptions options{{}, true, diagnostic, &validation};
    fails("instance", [&] { return VulkanInstance::create(options); });
    fails("messenger", [&] { return VulkanInstance::create(options); });
    moves([&] { return VulkanInstance::create(options); });
    {
        auto instance_owner = instance(validation);
        fails("device", [&] { return VulkanDevice::create(instance_owner); });
        moves([&] { return VulkanDevice::create(instance_owner); });
        auto device = take(VulkanDevice::create(instance_owner));
        fails("allocator", [&] { return VulkanAllocator::create(device); });
        moves([&] { return VulkanAllocator::create(device); });
        auto allocator = take(VulkanAllocator::create(device));
        auto buffer = [&] {
            return Buffer::create(
                allocator,
                4096,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                EMemoryAccess::READBACK
            );
        };
        auto image = [&] {
            return Image::create(
                allocator,
                {8, 8},
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
            );
        };
        fails("buffer", buffer);
        fails("mapping", buffer);
        fails("image", image);
        moves(buffer);
        moves(image);
        auto host = take(buffer());
        std::array<std::byte, 256> data{};
        fails("flush", [&] { return host.write(0, data); });
        fails("invalidate", [&] { return host.read(0, data); });
        const std::array bindings{
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}
        };
        auto create_descriptor = [&] { return DescriptorSetLayout::create(device, bindings); };
        fails("descriptor_layout", create_descriptor);
        moves(create_descriptor);
        auto descriptor = take(create_descriptor());
        const std::array sizes{VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
        auto create_pool = [&] { return DescriptorPool::create(device, 2, sizes); };
        fails("descriptor_pool", create_pool);
        moves(create_pool);
        auto pool = take(create_pool());
        fails("descriptor_set", [&] { return pool.allocate(descriptor); });
        CHECK(pool.allocate(descriptor)); // Failed allocation did not consume admission.
        const std::array layouts{descriptor.native()};
        auto create_layout = [&] { return PipelineLayout::create(device, layouts); };
        fails("pipeline_layout", create_layout);
        moves(create_layout);
        auto layout = take(create_layout());
        auto create_shader = [&] { return ShaderModule::create(device, spirv); };
        fails("shader", create_shader);
        moves(create_shader);
        auto shader = take(create_shader());
        auto create_pipeline = [&] { return ComputePipeline::create(device, shader, layout); };
        fails("pipeline", create_pipeline);
        moves(create_pipeline);
        auto active = take(create_pipeline());
        const auto old_pipeline = active.native();
        const auto before_partial = live;
        partial_pipeline = true;
        CHECK(!create_pipeline());
        partial_pipeline = false;
        CHECK(live == before_partial && active.native() == old_pipeline);
        ++failures;
        for (const auto *operation : {"command_pool", "commands", "fence"})
        {
            fails(operation, [&] { return SubmissionQueue::create(device, 3); });
        }
        fails("fence", [&] { return SubmissionQueue::create(device, 3); }, 2);
        auto queue = take(SubmissionQueue::create(device, 3));
        auto foreign = take(SubmissionQueue::create(device, 1));
        CHECK(!SubmissionQueue::create(device, 0));
        CHECK(!StagingArena::create(*queue, device, allocator, 1));
        auto arena = take(StagingArena::create(*queue, device, allocator, 65536));
        auto retirement = take(RetirementQueue::create(*queue, 2));
        CHECK(!RetirementQueue::create(*queue, 0));
        for (const auto *operation : {"reset_command", "begin"})
        {
            fails(operation, [&] { return queue->begin(); });
        }
        for (const auto *operation : {"end", "reset_fence", "submit"})
        {
            auto batch = take(queue->begin());
            fails(operation, [&] { return std::move(batch).submit(); });
            CHECK(queue->submitted() == 0 && !queue->recording());
        }
        {
            auto batch = take(queue->begin());
            CHECK(!queue->begin() && queue->begin().error().type == kBusy);
            CHECK(!arena.stage(batch, data, 3));
            fails("flush", [&] { return arena.stage(batch, data); });
            auto staged = take(arena.stage(batch, data));
            CHECK(staged.offset() == 0); // Failure did not commit the bump cursor.
            auto other = take(foreign->begin());
            CHECK(!arena.stage(other, data));
            CHECK(!recordUpload(other, staged, host));
            CHECK(!recordUpload(batch, staged, host, ~VkDeviceSize{0}));
            CHECK(!recordBufferCopy(batch, host, host, 4));
            std::array<std::byte, 65536> full{};
            CHECK(!arena.stage(batch, full));
            // Abandoned recording never publishes or consumes an in-flight slot.
        }
        CHECK(!queue->recording() && queue->submitted() == 0);
        hold_fences = true;
        auto first = take(buffer());
        auto second = take(buffer());
        auto candidate = take(buffer());
        std::vector<SubmissionTicket> tickets;
        tickets.reserve(3);
        for (int i = 0; i < 3; ++i)
        {
            auto batch = take(queue->begin());
            auto staged = take(arena.stage(batch, data));
            CHECK(staged.offset() == static_cast<VkDeviceSize>(i) * 65536);
            CHECK(recordUpload(batch, staged, host));
            CHECK(recordUpload(batch, staged, i == 0 ? second : first));
            tickets.push_back(take(std::move(batch).submit()));
        }
        CHECK(!queue->begin() && queue->begin().error().type == kCapacity);
        CHECK(queue->completed() == 0 && queue->submitted() == 3);
        CHECK(retirement.retire(tickets[2], std::move(first)));
        CHECK(retirement.retire(tickets[0], std::move(second))); // Non-FIFO retirement admission.
        CHECK(!retirement.retire(tickets[1], std::move(candidate)) && candidate.native());
        CHECK(take(retirement.collect()) == 0 && retirement.pending() == 2);
        CHECK(!foreign->wait(tickets[0], 0) && foreign->wait(tickets[0], 0).error().type == kWrongOwner);
        auto other_batch = take(foreign->begin());
        auto other_ticket = take(std::move(other_batch).submit());
        CHECK(!retirement.retire(other_ticket, std::move(candidate)) && candidate.native());
        hold_fences = false;
        fails("wait", [&] { return queue->wait(tickets[2], 5'000'000'000); });
        fails("fence_status", [&] { return queue->poll(); });
        CHECK(take(queue->wait(tickets[2], 5'000'000'000)));
        CHECK(take(foreign->wait(other_ticket, 5'000'000'000)));
        CHECK(take(retirement.collect()) == 2 && retirement.pending() == 0);
        CHECK(take(retirement.collect()) == 0);
        CHECK(retirement.retire(tickets[1], std::move(candidate)));
        CHECK(take(retirement.collect()) == 1); // Already-complete serial.
        for (int i = 0; i < 32; ++i)
        {
            auto batch = take(queue->begin());
            CHECK(arena.stage(batch, data));
            auto ticket = take(std::move(batch).submit());
            CHECK(take(queue->wait(ticket, 5'000'000'000)));
        }
        CHECK(queue->completed() == 35);
        {
            auto final_retirement = take(RetirementQueue::create(*queue, 1));
            auto final_buffer = take(buffer());
            auto batch = take(queue->begin());
            auto slice = take(arena.stage(batch, data));
            CHECK(recordUpload(batch, slice, final_buffer));
            auto ticket = take(std::move(batch).submit());
            CHECK(final_retirement.retire(ticket, std::move(final_buffer)));
            // No manual wait/shutdown: final owner teardown joins its GPU work.
        }
        CHECK(queue->completed() == 36);
        {
            auto lost_queue = take(SubmissionQueue::create(device, 1));
            auto batch = take(lost_queue->begin());
            lose_on_submit = true; // No native submission occurs on this injected path.
            auto result = std::move(batch).submit();
            lose_on_submit = false;
            CHECK(!result && result.error().args[0] == static_cast<std::uint32_t>(VK_ERROR_DEVICE_LOST));
            CHECK(lost_queue->deviceLost() && !lost_queue->begin() && !lost_queue->poll());
            CHECK(lost_queue->submitted() == 0 && lost_queue->completed() == 0);
        }
    }
    for (auto count : live)
        CHECK(count == 0);
    CHECK(validation.errors == 0);
    std::printf(
        "PASS fault_points=%zu owners=13 rollback/move/order/bounded-retirement validation_errors=%u warnings=%u\n",
        failures,
        validation.errors.load(),
        validation.warnings.load()
    );
}
