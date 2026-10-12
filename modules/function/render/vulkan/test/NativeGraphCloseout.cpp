#include "F3Tonemap.pass.hpp"
#include "F4CWrite.pass.hpp"
#include "F4CopyBuffer.pass.hpp"
#include "F4CopyUpload.pass.hpp"
#include "F4HostRead.pass.hpp"
#include "F4Readback.pass.hpp"
#include "NativeGraphSupport.hpp"

using namespace native_graph_test;

void computeHost(const std::filesystem::path& shaders, Validation& validation, bool multiple, bool partial)
{
    auto host = instance(validation);
    DeviceOptions options;
    options.multiple_queues = multiple;
    auto device = checked(VulkanDevice::create(host, options));
    std::printf(
        "DEVICE %s driver=%u API=%u multiple=%u\n",
        device.properties().deviceName,
        device.properties().driverVersion,
        device.properties().apiVersion,
        unsigned(multiple)
    );
    auto allocator = checked(VulkanAllocator::create(device));
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(device.physical(), &memory);
    for (unsigned type = 0; type < memory.memoryTypeCount; ++type)
    {
        std::printf("MEMORY type=%u flags=%u\n", type, memory.memoryTypes[type].propertyFlags);
    }
    auto queue = checked(SubmissionQueue::create(device, 24));
    auto compute = checked(SubmissionQueue::create(device, 24, EQueueRole::COMPUTE));
    auto transfer = checked(SubmissionQueue::create(device, 24, EQueueRole::TRANSFER));
    auto upload = checked(Buffer::create(allocator, 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, EMemoryAccess::UPLOAD));
    std::array<unsigned, 16> initial{};
    CHECK(upload.write(0, std::as_bytes(std::span{initial})));
    RenderGraphBuilder builder;
    auto source = builder.importBuffer("c1.initial", {64, 4}, EPersistentScope::VIEW);
    auto output = builder.buffer({64, 4}, "c1.output");
    F4CopyBuffer copy;
    copy.source = {source, {0, 64}};
    copy.destination = {output, {0, 64}};
    auto init = checked(builder.transfer("c1.init", EExecutionScope::VIEW, copy));
    F4CWrite write;
    write.output = {output, {0, 64}};
    auto pass = checked(builder.compute("c1.write", {{"F4CWrite", "default"}}, EExecutionScope::VIEW, write));
    CHECK(builder.source(passKey("c1.write"), "output", PassProducer{passKey("c1.init")}));
    const BufferRange exported = partial ? BufferRange{16, 16} : BufferRange{0, 64};
    CHECK(builder.exportBuffer(output, PassProducer{passKey("c1.write")}, EGraphOutput::READBACK, exported));
    auto definition = checked(std::move(builder).finish());
    auto logical = checked(compileLogicalGraph(definition));
    auto invocation = makeGraphInvocationData(definition);
    std::vector<NativeShaderProgram> programs;
    programs.push_back(program(device, logical, pass, "F4CWrite", PassSchema<F4CWrite>::contract(), shaders, true));
    std::array commands{
        NativePassCommand{init, CopyCommand{}, multiple ? EQueueRole::TRANSFER : EQueueRole::GRAPHICS},
        NativePassCommand{pass, DispatchCommand{16, 1, 1}, multiple ? EQueueRole::COMPUTE : EQueueRole::GRAPHICS}
    };
    std::array native{NativeImportBinding{
        GraphResourceId{source.value()},
        GraphBackingId{1},
        std::cref(upload),
        {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED},
        {},
        true
    }};
    auto plan = checked(compileFixture(
        {logical, device, allocator, {queue.get(), compute.get(), transfer.get()}, commands, native, invocation},
        std::move(programs)
    ));
    std::array imports{GraphImportBinding{GraphResourceId{source.value()}, GraphBackingId{1}, 0, 1}};
    auto bindings = checked(FrameGraphBindings::create(plan.plan.logical(), {}, imports, invocation));
    std::vector<NativeTraceEvent> trace(plan.invocations.traceCapacity());
    native[0].host_ready = false;
    const auto not_ready = executeGraph(plan.invocations, bindings.frame().frame_slot, bindings, native);
    CHECK(!not_ready && not_ready.error().type == kInvalidArgument);
    native[0].host_ready = true;
    VkEventCreateInfo event_info{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VkEvent gate{};
    CHECK(vkCreateEvent(device.native(), &event_info, nullptr, &gate) == VK_SUCCESS);
    auto pending = checked(queue->begin());
    vkCmdWaitEvents(
        pending.native(),
        1,
        &gate,
        VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        nullptr,
        0,
        nullptr,
        0,
        nullptr
    );
    const auto gate_ticket = checked(std::move(pending).submit());
    auto submission = executeGraph(plan.invocations, bindings.frame().frame_slot, bindings, native, trace);
    std::array<std::byte, 4> premature_bytes{};
    RenderResult<void> premature =
        submission ? plan.invocations
                         .readback(*submission, GraphResourceId{output.value()}, exported.byte_offset, premature_bytes)
                   : lux::cxx::unexpected(submission.error());
    // Release the actual GPU before asserting results, including a failed admission.
    CHECK(vkSetEvent(device.native(), gate) == VK_SUCCESS);
    CHECK(checked(queue->wait(gate_ticket, UINT64_MAX)));
    vkDestroyEvent(device.native(), gate, nullptr);
    auto receipt = checked(std::move(submission));
    CHECK(!premature && premature.error().type == kBusy);
    std::puts("C1 not_ready=REJECTED incomplete_receipt=BUSY");
    CHECK(checked(queue->wait(receipt.completion, UINT64_MAX)));
    std::array<unsigned, 16> actual{};
    const auto first = unsigned(exported.byte_offset / sizeof(unsigned));
    const auto count = unsigned(exported.byte_count / sizeof(unsigned));
    CHECK(plan.invocations.readback(
        receipt,
        GraphResourceId{output.value()},
        exported.byte_offset,
        std::as_writable_bytes(std::span{actual}.subspan(first, count))
    ));
    for (unsigned i = first; i < first + count; ++i)
    {
        CHECK(actual[i] == 100 + i);
    }
    std::puts(nativeTraceJson(std::span{trace}.first(receipt.trace_count)).c_str());
    bool compute_host = false;
    for (const auto& event : std::span{trace}.first(receipt.trace_count))
    {
        compute_host |= event.pass == pass && event.after.access == VK_ACCESS_2_HOST_READ_BIT &&
                        (event.before.access & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT) != 0;
    }
    std::printf("C1 GPU_byte_oracle=PASS shader_to_host_dependency=%u\n", unsigned(compute_host));
    CHECK(compute_host);
    if (partial)
    {
        auto outside = plan.invocations.readback(
            receipt,
            GraphResourceId{output.value()},
            0,
            std::as_writable_bytes(std::span{actual}.first(1))
        );
        std::printf("C1 partial_export outside_read_admitted=%u\n", unsigned(bool(outside)));
        CHECK(!outside && outside.error().type == kInvalidArgument);
    }
}

void aliasImport(Validation& validation)
{
    auto host = instance(validation);
    auto device = checked(VulkanDevice::create(host));
    auto allocator = checked(VulkanAllocator::create(device));
    auto queue = checked(SubmissionQueue::create(device, 24));
    ImageDescription desc{
        {4, 4},
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_SAMPLE_COUNT_1_BIT,
        1,
        1,
        true
    };
    auto first = checked(Image::create(allocator, desc));
    auto second = checked(Image::alias(first));
    const auto a = first.memoryBinding();
    const auto b = second.memoryBinding();
    CHECK(first.native() != second.native() && a.memory == b.memory && a.offset == b.offset);
    auto upload = checked(Buffer::create(allocator, 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, EMemoryAccess::UPLOAD));
    std::array<std::byte, 64> bytes{};
    CHECK(upload.write(0, bytes));
    RenderGraphBuilder builder;
    TextureDesc texture;
    texture.width = texture.height = 4;
    texture.format = lux::rdesc::ETextureFormat::RGBA8_UNORM;
    auto source = builder.importBuffer("c2.source", {64, 4}, EPersistentScope::VIEW);
    std::array images{
        builder.importTexture("c2.A", texture, EPersistentScope::VIEW),
        builder.importTexture("c2.B", texture, EPersistentScope::VIEW)
    };
    std::vector<NativePassCommand> commands;
    for (unsigned i = 0; i < 2; ++i)
    {
        F4CopyUpload copy;
        copy.source = {source, {0, 64}};
        copy.destination.texture = images[i];
        const auto name = "c2.write." + std::to_string(i);
        auto pass = checked(builder.transfer(name, EExecutionScope::VIEW, copy));
        CHECK(builder.exportTexture(images[i], PassProducer{passKey(name)}));
        commands.push_back({pass, CopyCommand{}, EQueueRole::GRAPHICS});
    }
    auto definition = checked(std::move(builder).finish());
    auto logical = checked(compileLogicalGraph(definition));
    auto values = makeGraphInvocationData(definition);
    std::array native{
        NativeImportBinding{
            GraphResourceId{source.value()},
            GraphBackingId{1},
            std::cref(upload),
            {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED},
            {},
            true
        },
        NativeImportBinding{GraphResourceId{images[0].value()}, GraphBackingId{2}, std::cref(first), {}, {}, true},
        NativeImportBinding{GraphResourceId{images[1].value()}, GraphBackingId{3}, std::cref(second), {}, {}, true}
    };
    auto plan = checked(compileFixture(
        {logical, device, allocator, {queue.get(), queue.get(), queue.get()}, commands, native, values},
        {}
    ));
    std::array imports{
        GraphImportBinding{GraphResourceId{source.value()}, GraphBackingId{1}, 0, 1},
        GraphImportBinding{GraphResourceId{images[0].value()}, GraphBackingId{2}, 0, 1},
        GraphImportBinding{GraphResourceId{images[1].value()}, GraphBackingId{3}, 0, 1}
    };
    auto frame = checked(FrameGraphBindings::create(plan.plan.logical(), {}, imports, values));
    auto result = executeGraph(plan.invocations, frame.frame().frame_slot, frame, native);
    if (result)
    {
        CHECK(checked(queue->wait(result->completion, UINT64_MAX)));
    }
    std::printf(
        "C2 distinct_handles=1 memory_overlap=1 admitted=%u validation_errors=%u\n",
        unsigned(bool(result)),
        validation.errors.load()
    );
    CHECK(!result && result.error().type == kInvalidArgument);
    auto independent = checked(Image::create(allocator, desc));
    native[2].backing = std::cref(independent);
    auto accepted = checked(executeGraph(plan.invocations, frame.frame().frame_slot, frame, native));
    CHECK(checked(queue->wait(accepted.completion, UINT64_MAX)));
    std::puts("C2 nonoverlapping_native_allocations=ACCEPTED");
    native[2].backing = std::cref(first);
    auto duplicate = executeGraph(plan.invocations, frame.frame().frame_slot, frame, native);
    CHECK(!duplicate && duplicate.error().type == kInvalidArgument);
    std::puts("C2 same_native_handle=REJECTED");
    native[2].backing = std::cref(independent);
    VkEventCreateInfo event_info{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VkEvent gate{};
    CHECK(vkCreateEvent(device.native(), &event_info, nullptr, &gate) == VK_SUCCESS);
    auto pending = checked(queue->begin());
    vkCmdWaitEvents(
        pending.native(),
        1,
        &gate,
        VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        nullptr,
        0,
        nullptr,
        0,
        nullptr
    );
    auto gate_ticket = checked(std::move(pending).submit());
    auto active = executeGraph(plan.invocations, frame.frame().frame_slot, frame, native);
    RenderResult<GraphSubmission> overlapping = lux::cxx::unexpected(RenderError{kInvalidArgument});
    if (active)
    {
        for (auto& input : native)
        {
            input.ready = active->completion;
        }
        native[1].backing = std::cref(second);
        native[1].identity = imports[1].backing = GraphBackingId{4};
        auto next = checked(FrameGraphBindings::create(plan.plan.logical(), {1, 1, 1}, imports, values));
        overlapping = executeGraph(plan.invocations, next.frame().frame_slot, next, native);
    }
    CHECK(vkSetEvent(device.native(), gate) == VK_SUCCESS);
    CHECK(checked(queue->wait(gate_ticket, UINT64_MAX)));
    vkDestroyEvent(device.native(), gate, nullptr);
    CHECK(active);
    CHECK(checked(queue->wait(active->completion, UINT64_MAX)));
    if (overlapping)
    {
        CHECK(checked(queue->wait(overlapping->completion, UINT64_MAX)));
    }
    CHECK(!overlapping && overlapping.error().type == kBusy);
    std::puts("C2 previous_inflight_distinct_alias_with_ready_ticket=BUSY");
}

namespace native_graph_test
{
    void replacementCase(const std::filesystem::path& shaders, Validation& validation)
    {
        auto host = instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        auto device = checked(VulkanDevice::create(host, options));
        auto allocator = checked(VulkanAllocator::create(device));
        auto queue = checked(SubmissionQueue::create(device, 32));
        auto staging = checked(StagingArena::create(*queue, device, allocator, 1024));
        auto sampler = checked(Sampler::create(device));
        std::vector<std::unique_ptr<Image>> images;
        images.reserve(2);
        for (unsigned i = 0; i < 2; ++i)
        {
            images.push_back(std::make_unique<Image>(checked(Image::create(
                allocator,
                {4, 4},
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
            ))));
        }
        std::array<std::optional<SubmissionTicket>, 2> ready;
        for (unsigned i = 0; i < 2; ++i)
        {
            std::array<std::byte, 64> pixels;
            pixels.fill(std::byte(i == 0 ? 64 : 192));
            auto batch = checked(queue->begin());
            auto slice = checked(staging.stage(batch, pixels));
            CHECK(recordImageUpload(batch, slice, *images[i], VK_IMAGE_LAYOUT_UNDEFINED));
            ready[i] = checked(std::move(batch).submit());
        }
        RenderGraphBuilder builder;
        TextureDesc texture;
        texture.width = texture.height = 4;
        texture.format = lux::rdesc::ETextureFormat::RGBA8_UNORM;
        GraphImportContract history;
        history.temporal_history = true;
        const auto input = builder.importTexture("history", texture, EPersistentScope::VIEW, history);
        texture.format = lux::rdesc::ETextureFormat::RGBA32_SFLOAT;
        F3Tonemap params;
        params.input.texture = input;
        params.nearest.sampler = GraphSampler{1};
        params.output.texture = builder.texture(texture, "imported.output");
        const auto tone = checked(builder.graphics("tone", {{"F3Tonemap", "default"}}, EExecutionScope::VIEW, params));
        F4Readback read;
        read.source.texture = params.output.texture;
        read.destination.buffer = builder.buffer({256, 4}, "imported.bytes");
        const auto copy = checked(builder.transfer("read", EExecutionScope::VIEW, read));
        CHECK(builder.exportBuffer(read.destination.buffer, PassProducer{passKey("read")}, EGraphOutput::READBACK));
        auto definition = checked(std::move(builder).finish());
        auto logical = checked(compileLogicalGraph(definition));
        auto invocation = makeGraphInvocationData(definition);
        invocation.history_epoch = 1;
        std::vector<NativeShaderProgram> programs;
        programs.push_back(
            program(device, logical, tone, "F3Tonemap", PassSchema<F3Tonemap>::contract(), shaders, false)
        );
        const std::array commands{
            NativePassCommand{tone, DrawCommand{}, EQueueRole::GRAPHICS},
            NativePassCommand{copy, CopyCommand{}, EQueueRole::GRAPHICS}
        };
        const std::array samplers{std::pair{GraphSampler{1}, std::cref(sampler)}};
        std::array native_imports{NativeImportBinding{
            GraphResourceId{input.value()},
            GraphBackingId{1},
            std::cref(*images[0]),
            {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, device.queueFamily()
            },
            ready[0]
        }};
        NativeGraphFixtureInputs inputs{
            logical,
            device,
            allocator,
            {queue.get(), queue.get(), queue.get()},
            commands,
            native_imports,
            invocation,
            samplers
        };
        auto executable = checked(compileFixture(inputs, std::move(programs)));
        const auto requirements = executable.plan.importViews(GraphResourceId{input.value()});
        CHECK(requirements.size() == 1);
        std::vector<std::unique_ptr<ImageView>> views;
        for (const auto& image : images)
        {
            views.push_back(std::make_unique<ImageView>(
                checked(ImageView::create(*image, requirements[0].range, requirements[0].type))
            ));
        }
        std::array view_refs{std::cref(*views[0])};
        std::array logical_imports{GraphImportBinding{GraphResourceId{input.value()}, GraphBackingId{1}, 0, 1, 1}};
        for (unsigned serial = 0; serial < 8; ++serial)
        {
            const auto selected = serial < 2 ? 0u : 1u;
            native_imports[0].identity = logical_imports[0].backing = GraphBackingId{selected + 1};
            native_imports[0].backing = std::cref(*images[selected]);
            native_imports[0].ready = ready[selected];
            view_refs[0] = std::cref(*views[selected]);
            native_imports[0].views = view_refs;
            const bool first_use = serial == 0 || serial == 2;
            native_imports[0].initial = first_use
                ? NativeResourceState{VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, device.queueFamily()}
                : NativeResourceState{VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, device.queueFamily()};
            auto frame = checked(FrameGraphBindings::create(
                executable.plan.logical(),
                {serial, serial, serial % 2},
                logical_imports,
                invocation
            ));
            if (serial == 0)
            {
                const auto submitted = queue->submitted();
                native_imports[0].ready.reset();
                CHECK(!executeGraph(executable.invocations, frame.frame().frame_slot, frame, native_imports));
                native_imports[0].ready = ready[selected];
                native_imports[0].identity = GraphBackingId{99};
                CHECK(!executeGraph(executable.invocations, frame.frame().frame_slot, frame, native_imports));
                native_imports[0].identity = logical_imports[0].backing;
                logical_imports[0].history_epoch = 2;
                CHECK(!FrameGraphBindings::create(executable.plan.logical(), {}, logical_imports, invocation));
                logical_imports[0].history_epoch = 1;
                native_imports[0].initial.family = device.queueFamily() + 100;
                auto wrong_family = executeGraph(executable.invocations, frame.frame().frame_slot, frame, native_imports);
                CHECK(!wrong_family && wrong_family.error().type == kWrongOwner);
                native_imports[0].initial.family = device.queueFamily();
                auto foreign_queue = checked(SubmissionQueue::create(device, 1));
                auto foreign_batch = checked(foreign_queue->begin());
                auto foreign_ticket = checked(std::move(foreign_batch).submit());
                native_imports[0].ready = foreign_ticket;
                auto wrong_ticket = executeGraph(executable.invocations, frame.frame().frame_slot, frame, native_imports);
                CHECK(!wrong_ticket && wrong_ticket.error().type == kWrongOwner);
                CHECK(checked(foreign_queue->wait(foreign_ticket, UINT64_MAX)));
                native_imports[0].ready = ready[selected];
                CHECK(queue->submitted() == submitted);
            }
            auto receipt = checked(executeGraph(executable.invocations, frame.frame().frame_slot, frame, native_imports));
            CHECK(checked(queue->wait(receipt.completion, UINT64_MAX)));
            ready[selected] = receipt.completion;
            if (serial == 2)
            {
                views[0].reset();
                images[0].reset();
                std::puts("C3 A_image_and_external_view_destroyed plan_alive one_completed_slot_still_references_A");
            }
            std::array<float, 64> pixels;
            CHECK(executable.invocations.readback(
                receipt,
                GraphResourceId{read.destination.buffer.value()},
                0,
                std::as_writable_bytes(std::span{pixels})
            ));
            const auto value = float(selected == 0 ? 64 : 192) / 255.0f;
            for (unsigned pixel = 0; pixel < 16; ++pixel)
            {
                CHECK(std::abs(pixels[pixel * 4] - value / (1 + value)) < 1e-6f);
            }
        }
        std::puts("C3 completed_B_after_A_destroy CPU_oracle=PASS");
    }
} // namespace native_graph_test

int main(int argc, char** argv)
{
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    CHECK(argc == 3);
    Validation validation;
    const std::string mode(argv[1]);
    if (mode == "C1" || mode == "C1-multi" || mode == "C1-partial")
    {
        computeHost(argv[2], validation, mode == "C1-multi", mode == "C1-partial");
    }
    else if (mode == "C2")
    {
        aliasImport(validation);
    }
    else
    {
        CHECK(mode == "C3");
        replacementCase(argv[2], validation);
    }
    std::printf("VALIDATION errors=%u warnings=%u\n", validation.errors.load(), validation.warnings.load());
    CHECK(validation.errors == 0);
}
