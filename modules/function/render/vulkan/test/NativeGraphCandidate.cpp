#include "F4Fill.pass.hpp"
#include "F4HostRead.pass.hpp"
#include "NativeGraphSupport.hpp"
#include <cstring>

using namespace native_graph_test;

namespace
{
    constexpr std::array owner_names{
        "instance",
        "messenger",
        "device",
        "allocator",
        "buffer",
        "image",
        "image_alias",
        "image_view",
        "sampler",
        "descriptor_layout",
        "descriptor_pool",
        "shader",
        "pipeline_layout",
        "pipeline",
        "graphics_pipeline",
        "command_pool",
        "fence",
        "timeline"
    };
    std::array<unsigned, owner_names.size()> live{}, creations{};
    std::string_view failure;
    unsigned countdown{};
    bool hold_evidence{}, device_lost{};

    std::size_t ownerIndex(std::string_view name)
    {
        return std::find(owner_names.begin(), owner_names.end(), name) - owner_names.begin();
    }
} // namespace

namespace lux::render::vulkan::test
{
    VkResult before(const char* operation) noexcept
    {
        if (hold_evidence && std::string_view(operation) == "fence_status")
        {
            return VK_NOT_READY;
        }
        if (device_lost && std::string_view(operation) == "submit")
        {
            return VK_ERROR_DEVICE_LOST;
        }
        if (failure == operation && --countdown == 0)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        return VK_SUCCESS;
    }

    VkResult after(const char* operation, VkResult result) noexcept
    {
        const auto index = ownerIndex(operation);
        if (index < live.size() && result == VK_SUCCESS)
        {
            ++live[index];
            ++creations[index];
        }
        return result;
    }

    void destroyed(const char* operation) noexcept
    {
        const auto index = ownerIndex(operation);
        CHECK(index < live.size() && live[index] != 0);
        --live[index];
        if (std::string_view(operation) == "allocator")
        {
            CHECK(
                live[ownerIndex("buffer")] == 0 && live[ownerIndex("image")] == 0 &&
                live[ownerIndex("image_alias")] == 0
            );
        }
    }
} // namespace lux::render::vulkan::test

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    Validation validation;
    {
        auto host = instance(validation);
        auto device = checked(VulkanDevice::create(host));
        auto allocator = checked(VulkanAllocator::create(device));
        auto queue = checked(SubmissionQueue::create(device, 8));
        RenderGraphBuilder builder;
        TextureDesc desc;
        desc.width = desc.height = 4;
        F4Fill fill;
        fill.output.texture = builder.texture(desc, "candidate.image");
        const auto compute =
            checked(builder.compute("candidate.fill", {{"F4Fill", "default"}}, EExecutionScope::VIEW, fill));
        F4HostRead read;
        read.source.texture = fill.output.texture;
        const auto copy = checked(builder.readback("candidate.read", EExecutionScope::VIEW, read));
        auto definition = checked(std::move(builder).finish());
        auto logical = checked(compileLogicalGraph(definition));
        auto values = makeGraphInvocationData(definition);
        const std::array commands{
            NativePassCommand{compute, DispatchCommand{4, 4, 1}, EQueueRole::GRAPHICS},
            NativePassCommand{copy, CopyCommand{}, EQueueRole::GRAPHICS}
        };
        NativeCompileInputs
            inputs{logical, device, allocator, {queue.get(), queue.get(), queue.get()}, commands, {}, values};
        const auto shaders = [&]
        {
            std::vector<NativeShaderProgram> result;
            result.push_back(program(device, logical, compute, "F4Fill", PassSchema<F4Fill>::contract(), argv[1], true)
            );
            return result;
        };
        {
            auto candidates = shaders();
            const auto& candidate = candidates[0];
            auto image =
                checked(Image::create(allocator, {4, 4}, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT));
            auto view = checked(ImageView::create(image, VK_IMAGE_ASPECT_COLOR_BIT));
            auto small =
                checked(Image::create(allocator, {2, 2}, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT));
            auto small_view = checked(ImageView::create(small, VK_IMAGE_ASPECT_COLOR_BIT));
            const auto owner = candidate.identity().layout.owners[0].identity;
            const std::array original{
                OwnerDescriptorValue{owner, "output", 0, ImageDescriptorValue{std::cref(view), VK_IMAGE_LAYOUT_GENERAL}}
            };
            auto descriptor = checked(BoundDescriptorSets::create(device, candidate, original));
            std::array<VDescriptorValue, 1> values{original[0].value};
            auto batch = checked(queue->begin());
            const auto ticket = checked(std::move(batch).submit());
            const std::array last_use{ticket};
            hold_evidence = true;
            auto busy = descriptor.rewrite(values, last_use);
            CHECK(!busy && busy.error().type == kBusy);
            hold_evidence = false;
            CHECK(checked(queue->wait(ticket, UINT64_MAX)));
            CHECK(descriptor.rewrite(values, last_use));
            values[0] = ImageDescriptorValue{std::cref(small_view), VK_IMAGE_LAYOUT_GENERAL};
            CHECK(!descriptor.rewrite(values, last_use));
            values[0] = original[0].value;
            auto other_device = checked(VulkanDevice::create(host));
            auto other_queue = checked(SubmissionQueue::create(other_device, 1));
            auto other_batch = checked(other_queue->begin());
            const std::array foreign{checked(std::move(other_batch).submit())};
            auto wrong_owner = descriptor.rewrite(values, foreign);
            CHECK(!wrong_owner && wrong_owner.error().type == kWrongOwner);
            CHECK(checked(other_queue->wait(foreign[0], UINT64_MAX)));
            std::puts("DESCRIPTOR_REWRITE busy_foreign_shape_negative=PASS");
        }
        auto active = std::make_unique<ExecutableGraphPlan>(checked(compileVulkanGraph(inputs, shaders())));
        const auto* last_good = active.get();
        unsigned failures = 0;
        auto wrong_commands = commands;
        std::get<DispatchCommand>(wrong_commands[0].command).x =
            device.properties().limits.maxComputeWorkGroupCount[0] + 1;
        auto wrong_inputs = inputs;
        wrong_inputs.commands = wrong_commands;
        CHECK(!compileVulkanGraph(wrong_inputs, shaders()));
        wrong_commands[0].command = DrawCommand{};
        CHECK(!compileVulkanGraph(wrong_inputs, shaders()));
        wrong_inputs.commands = inputs.commands;
        wrong_inputs.frame_capacity = 1;
        CHECK(!compileVulkanGraph(wrong_inputs, shaders()));
        for (const auto operation : {"image", "buffer", "mapping", "image_view", "descriptor_pool", "descriptor_set"})
        {
            for (unsigned occurrence : {1u, 2u})
            {
                const auto before = live;
                auto programs = shaders();
                failure = operation;
                countdown = occurrence;
                const auto candidate = compileVulkanGraph(inputs, std::move(programs));
                CHECK(!candidate && countdown == 0);
                CHECK(candidate.error().type == kNativeFailure);
                CHECK(candidate.error().args[0] == static_cast<std::uint32_t>(VK_ERROR_OUT_OF_DEVICE_MEMORY));
                failure = {};
                CHECK(live == before && active.get() == last_good);
                ++failures;
            }
        }
        std::array<GraphImportBinding, 0> imports;
        std::array<std::optional<GraphSubmission>, 2> receipts;
        hold_evidence = true;
        for (unsigned slot = 0; slot < 2; ++slot)
        {
            auto frame = checked(FrameGraphBindings::create(active->logical(), {slot, slot, slot}, imports, values));
            const auto created_before = creations;
            receipts[slot] = checked(active->submit(frame, {}));
            CHECK(creations == created_before);
        }
        auto busy_frame = checked(FrameGraphBindings::create(active->logical(), {2, 2, 0}, imports, values));
        auto busy = active->submit(busy_frame, {});
        CHECK(!busy && busy.error().type == kBusy);
        auto replacement = std::make_unique<ExecutableGraphPlan>(checked(compileVulkanGraph(inputs, shaders())));
        auto retirement = checked(RetirementQueue::create(*queue, 1));
        const auto last = *checked(active->lastUse());
        CHECK(retirement.retire(last, std::move(active)));
        CHECK(!active && retirement.pending() == 1);
        const auto pinned = live;
        CHECK(checked(retirement.collect()) == 0 && live == pinned);
        auto full = retirement.retire(last, std::move(replacement));
        CHECK(!full && full.error().type == kCapacity && replacement);
        hold_evidence = false;
        CHECK(checked(queue->wait(last, UINT64_MAX)));
        std::array<float, 64> pixels;
        CHECK(const_cast<ExecutableGraphPlan*>(last_good)
                  ->readbackPass(*receipts[1], copy, 0, 0, std::as_writable_bytes(std::span{pixels})));
        CHECK(pixels[0] == 1.0f && pixels[1] == 2.0f && pixels[2] == 3.0f);
        CHECK(checked(retirement.collect()) == 1 && retirement.pending() == 0);
        active = std::move(replacement);
        auto frame = checked(FrameGraphBindings::create(active->logical(), {}, imports, values));
        auto receipt = checked(active->submit(frame, {}));
        CHECK(checked(queue->wait(receipt.completion, UINT64_MAX)));
        CHECK(active->readbackPass(receipt, copy, 0, 0, std::as_writable_bytes(std::span{pixels})));

        // Driver failure is injected only after real outstanding GPU work has
        // completed; this cannot use a simulated loss to hide live native work.
        device_lost = true;
        auto failed = active->submit(frame, {});
        CHECK(!failed && failed.error().type == kNativeFailure);
        CHECK(failed.error().args[0] == static_cast<std::uint32_t>(VK_ERROR_DEVICE_LOST));
        CHECK(!active->lastUse());
        CHECK(!active->submit(frame, {}));
        std::printf(
            "GRAPH_FAULTS failures=%u last_good=PASS FIF=2 retirement_capacity=PASS device_lost_terminal=PASS "
            "hot_native_creations=0\n",
            failures
        );
    }
    CHECK(std::all_of(live.begin(), live.end(), [](auto count) { return count == 0; }));
    CHECK(validation.errors == 0);
    std::printf("GRAPH_FAULTS validation_errors=%u\n", validation.errors.load());
}
