#include "F4CopyBuffer.pass.hpp"
#include "F4CopyImage.pass.hpp"
#include "F4CopyUpload.pass.hpp"
#include "F4HostRead.pass.hpp"
#include "NativeGraphSupport.hpp"

namespace native_graph_test
{
    void copyCase(Validation& validation)
    {
        auto host = instance(validation);
        auto device = checked(VulkanDevice::create(host));
        auto allocator = checked(VulkanAllocator::create(device));
        auto queue = checked(SubmissionQueue::create(device, 32));
        auto upload = checked(Buffer::create(allocator, 112, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, EMemoryAccess::UPLOAD));
        std::array<std::byte, 112> bytes;
        for (std::size_t i = 0; i < bytes.size(); ++i)
        {
            bytes[i] = std::byte(i);
        }
        CHECK(upload.write(0, bytes));
        RenderGraphBuilder builder;
        const auto input = builder.importBuffer("copy.input", {80, 4}, EPersistentScope::VIEW);
        const auto scratch = builder.buffer({80, 4}, "copy.scratch");
        TextureDesc texture;
        texture.width = texture.height = 4;
        texture.mip_count = 2;
        texture.format = lux::rdesc::ETextureFormat::RGBA8_UNORM;
        const auto first = builder.texture(texture, "copy.first");
        const auto second = builder.texture(texture, "copy.second");
        const ImageRange range{EAspect::COLOR, 0, 2, 0, 1};
        F4CopyBuffer buffer;
        buffer.source = {input, {0, 80}};
        buffer.destination = {scratch, {0, 80}};
        const auto a = checked(builder.transfer("copy.buffer", EExecutionScope::VIEW, buffer));
        F4CopyUpload image;
        image.source = {scratch, {0, 80}};
        image.destination = {first, range};
        const auto b = checked(builder.transfer("copy.upload", EExecutionScope::VIEW, image));
        F4CopyImage clone;
        clone.source = {first, range};
        clone.destination = {second, range};
        const auto c = checked(builder.transfer("copy.image", EExecutionScope::VIEW, clone));
        F4HostRead read;
        read.source = {second, range};
        const auto d = checked(builder.readback("copy.host", EExecutionScope::VIEW, read));
        CHECK(builder.exportTexture(second, PassProducer{passKey("copy.image")}));
        auto definition = checked(std::move(builder).finish());
        auto logical = checked(compileLogicalGraph(definition));
        auto invocation = makeGraphInvocationData(definition);
        const std::array commands{
            NativePassCommand{a, CopyCommand{}, EQueueRole::GRAPHICS},
            NativePassCommand{b, CopyCommand{}, EQueueRole::GRAPHICS},
            NativePassCommand{c, CopyCommand{}, EQueueRole::GRAPHICS},
            NativePassCommand{d, CopyCommand{}, EQueueRole::GRAPHICS}
        };
        std::array native{NativeImportBinding{
            GraphResourceId{input.value()},
            GraphBackingId{1},
            std::cref(upload),
            {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, device.queueFamily()},
            {},
            true
        }};
        NativeCompileInputs
            inputs{logical, device, allocator, {queue.get(), queue.get(), queue.get()}, commands, native, invocation};
        auto executable = checked(compileVulkanGraph(inputs, {}));
        std::array imports{GraphImportBinding{GraphResourceId{input.value()}, GraphBackingId{1}, 16, 1}};
        std::optional<GraphSubmission> last_receipt;
        for (unsigned serial = 0; serial < 4; ++serial)
        {
            imports[0].dynamic_offset = 16 + (serial / 2) * 16;
            auto frame = checked(
                FrameGraphBindings::create(executable.logical(), {serial, serial, serial % 2}, imports, invocation)
            );
            const auto receipt = checked(executable.submit(frame, native));
            last_receipt = receipt;
            CHECK(checked(queue->wait(receipt.completion, UINT64_MAX)));
            std::array<std::byte, 80> actual;
            CHECK(executable.readbackPass(receipt, d, 0, 0, actual));
            CHECK(std::equal(actual.begin(), actual.end(), bytes.begin() + imports[0].dynamic_offset));
            CHECK(!executable.readbackPass(receipt, d, 1, 0, actual));
        }
        const auto exported = checked(executable.exportedBacking(*last_receipt, GraphResourceId{second.value()}));
        std::array<NativeRangeState, 2> states;
        CHECK(checked(executable.resourceStates(*last_receipt, GraphResourceId{second.value()}, states)) == 2);
        CHECK(!executable.resourceStates(*last_receipt, GraphResourceId{second.value()}, std::span{states}.first(1)));
        RenderGraphBuilder consumer;
        const auto borrowed = consumer.importTexture("external.image", texture, EPersistentScope::VIEW);
        F4HostRead external_read;
        external_read.source = {borrowed, range};
        const auto host_pass = checked(consumer.readback("external.host", EExecutionScope::VIEW, external_read));
        auto consumer_definition = checked(std::move(consumer).finish());
        auto consumer_logical = checked(compileLogicalGraph(consumer_definition));
        auto consumer_values = makeGraphInvocationData(consumer_definition);
        const std::array consumer_commands{NativePassCommand{host_pass, CopyCommand{}, EQueueRole::GRAPHICS}};
        std::array consumer_imports{NativeImportBinding{
            GraphResourceId{borrowed.value()},
            GraphBackingId{100},
            exported,
            {},
            {},
            false,
            {},
            states
        }};
        NativeCompileInputs consumer_inputs{
            consumer_logical,
            device,
            allocator,
            {queue.get(), queue.get(), queue.get()},
            consumer_commands,
            consumer_imports,
            consumer_values
        };
        auto consumer_plan = checked(compileVulkanGraph(consumer_inputs, {}));
        std::array consumer_bindings{GraphImportBinding{GraphResourceId{borrowed.value()}, GraphBackingId{100}, 0, 1}};
        auto external_frame =
            checked(FrameGraphBindings::create(consumer_plan.logical(), {}, consumer_bindings, consumer_values));
        consumer_imports[0].ranges = std::span{states}.first(1);
        const auto before_invalid = queue->submitted();
        CHECK(!consumer_plan.submit(external_frame, consumer_imports));
        CHECK(queue->submitted() == before_invalid);
        consumer_imports[0].ranges = states;
        const auto external_receipt = checked(consumer_plan.submit(external_frame, consumer_imports));
        const auto extended = checked(executable.retainExternalUse(*last_receipt, external_receipt.completion));
        CHECK(!executable.completed(*last_receipt));
        CHECK(!executable.resourceStates(extended, GraphResourceId{second.value()}, states));
        const auto last = checked(executable.lastUse());
        CHECK(last);
        auto retired = checked(RetirementQueue::create(*queue, 1));
        auto owned = std::make_unique<ExecutableGraphPlan>(std::move(executable));
        CHECK(retired.retire(*last, std::move(owned)));
        CHECK(!owned && retired.pending() == 1);
        CHECK(checked(queue->wait(extended.completion, UINT64_MAX)));
        std::array<std::byte, 80> external_pixels;
        CHECK(consumer_plan.readbackPass(external_receipt, host_pass, 0, 0, external_pixels));
        CHECK(std::equal(external_pixels.begin(), external_pixels.end(), bytes.begin() + 32));
        CHECK(checked(retired.collect()) == 1 && retired.pending() == 0);
        std::puts("COPY buffer-buffer-image-image-host mips=2 dynamic_offsets=16,32 oracle=PASS "
                  "export_handoff_retirement=PASS");
    }
} // namespace native_graph_test
