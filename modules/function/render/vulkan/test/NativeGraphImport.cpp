#include "F3Tonemap.pass.hpp"
#include "F4Readback.pass.hpp"
#include "NativeGraphSupport.hpp"

namespace native_graph_test
{
    void importedCase(const std::filesystem::path& shaders, Validation& validation)
    {
        auto host = instance(validation);
        DeviceOptions options;
        options.dynamic_rendering = true;
        auto device = checked(VulkanDevice::create(host, options));
        auto allocator = checked(VulkanAllocator::create(device));
        auto queue = checked(SubmissionQueue::create(device, 32));
        auto staging = checked(StagingArena::create(*queue, device, allocator, 1024));
        auto sampler = checked(Sampler::create(device));
        std::vector<Image> images;
        images.reserve(2);
        for (unsigned i = 0; i < 2; ++i)
        {
            images.push_back(checked(Image::create(
                allocator,
                {4, 4},
                VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
            )));
        }
        std::array<std::optional<SubmissionTicket>, 2> ready;
        for (unsigned i = 0; i < 2; ++i)
        {
            std::array<std::byte, 64> pixels;
            pixels.fill(std::byte(i == 0 ? 64 : 192));
            auto batch = checked(queue->begin());
            auto slice = checked(staging.stage(batch, pixels));
            CHECK(recordImageUpload(batch, slice, images[i], VK_IMAGE_LAYOUT_UNDEFINED));
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
            std::cref(images[0]),
            {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, device.queueFamily()
            },
            ready[0]
        }};
        NativeCompileInputs inputs{
            logical,
            device,
            allocator,
            {queue.get(), queue.get(), queue.get()},
            commands,
            native_imports,
            invocation,
            samplers
        };
        auto executable = checked(compileVulkanGraph(inputs, std::move(programs)));
        const auto requirements = executable.importViews(GraphResourceId{input.value()});
        CHECK(requirements.size() == 1);
        std::vector<ImageView> views;
        for (const auto& image : images)
        {
            views.push_back(checked(ImageView::create(image, requirements[0].range, requirements[0].type)));
        }
        std::array view_refs{std::cref(views[0])};
        std::array logical_imports{GraphImportBinding{GraphResourceId{input.value()}, GraphBackingId{1}, 0, 1, 1}};
        for (unsigned serial = 0; serial < 8; ++serial)
        {
            const auto selected = (serial / 2) % 2;
            native_imports[0].identity = logical_imports[0].backing = GraphBackingId{selected + 1};
            native_imports[0].backing = std::cref(images[selected]);
            native_imports[0].ready = ready[selected];
            view_refs[0] = std::cref(views[selected]);
            native_imports[0].views = view_refs;
            const bool first_use = serial == 0 || serial == 2;
            native_imports[0].initial = first_use
                ? NativeResourceState{VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL, device.queueFamily()}
                : NativeResourceState{VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, device.queueFamily()};
            auto frame = checked(FrameGraphBindings::create(
                executable.logical(),
                {serial, serial, serial % 2},
                logical_imports,
                invocation
            ));
            if (serial == 0)
            {
                const auto submitted = queue->submitted();
                native_imports[0].ready.reset();
                CHECK(!executable.submit(frame, native_imports));
                native_imports[0].ready = ready[selected];
                native_imports[0].identity = GraphBackingId{99};
                CHECK(!executable.submit(frame, native_imports));
                native_imports[0].identity = logical_imports[0].backing;
                logical_imports[0].history_epoch = 2;
                CHECK(!FrameGraphBindings::create(executable.logical(), {}, logical_imports, invocation));
                logical_imports[0].history_epoch = 1;
                native_imports[0].initial.family = device.queueFamily() + 100;
                auto wrong_family = executable.submit(frame, native_imports);
                CHECK(!wrong_family && wrong_family.error().type == kWrongOwner);
                native_imports[0].initial.family = device.queueFamily();
                auto foreign_queue = checked(SubmissionQueue::create(device, 1));
                auto foreign_batch = checked(foreign_queue->begin());
                auto foreign_ticket = checked(std::move(foreign_batch).submit());
                native_imports[0].ready = foreign_ticket;
                auto wrong_ticket = executable.submit(frame, native_imports);
                CHECK(!wrong_ticket && wrong_ticket.error().type == kWrongOwner);
                CHECK(checked(foreign_queue->wait(foreign_ticket, UINT64_MAX)));
                native_imports[0].ready = ready[selected];
                CHECK(queue->submitted() == submitted);
            }
            auto receipt = checked(executable.submit(frame, native_imports));
            CHECK(checked(queue->wait(receipt.completion, UINT64_MAX)));
            ready[selected] = receipt.completion;
            std::array<float, 64> pixels;
            CHECK(executable.readback(
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
        std::puts("IMPORT changed_backing_and_prepared_view history_epoch ready_negative GPU_oracle=PASS");
    }
} // namespace native_graph_test
