#include "F3SharedA.pass.hpp"
#include "F3SharedB.pass.hpp"
#include "F4Readback.pass.hpp"
#include "NativeGraphSupport.hpp"

namespace native_graph_test
{
    void sharedCase(const std::filesystem::path& shaders, Validation& validation)
    {
        auto host = instance(validation);
        auto device = checked(VulkanDevice::create(host, DeviceOptions{.dynamic_rendering = true}));
        auto allocator = checked(VulkanAllocator::create(device));
        auto queue = checked(SubmissionQueue::create(device, 24));
        RenderGraphBuilder builder;
        std::vector<Buffer> buffers;
        std::vector<GraphBuffer> handles;
        std::vector<NativeImportBinding> native_imports;
        std::vector<GraphImportBinding> imports;
        buffers.reserve(4);
        for (unsigned i = 0; i < 4; ++i)
        {
            buffers.push_back(
                checked(Buffer::create(allocator, 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, EMemoryAccess::UPLOAD))
            );
            const std::array<float, 4> data{float(i + 1), float(i + 2), float(i + 3), float(i + 4)};
            CHECK(buffers.back().write(0, std::as_bytes(std::span{data})));
            handles.push_back(builder.importBuffer("shared." + std::to_string(i), {16, 16}, EPersistentScope::SCENE));
            const auto resource = GraphResourceId{handles.back().value()};
            imports.push_back({resource, GraphBackingId{i + 1}, 0, 1});
            native_imports.push_back(
                {resource,
                 GraphBackingId{i + 1},
                 std::cref(buffers.back()),
                 {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT},
                 {},
                 true}
            );
        }
        TextureDesc texture;
        texture.width = texture.height = 2;
        F3SharedA a;
        a.scene_a.buffer = handles[0];
        a.feature_a.buffer = handles[1];
        a.output.texture = builder.texture(texture, "shared.output.a");
        F3SharedB b;
        b.scene_b.buffer = handles[2];
        b.feature_b.buffer = handles[3];
        b.output.texture = builder.texture(texture, "shared.output.b");
        const auto pa = checked(builder.graphics("shared.a", {{"F3SharedA", "default"}}, EExecutionScope::VIEW, a));
        const auto pb = checked(builder.graphics("shared.b", {{"F3SharedB", "default"}}, EExecutionScope::VIEW, b));
        std::vector<NativePassCommand> commands{
            {pa, DrawCommand{}, EQueueRole::GRAPHICS},
            {pb, DrawCommand{}, EQueueRole::GRAPHICS}
        };
        std::array<GraphBuffer, 2> outputs;
        for (unsigned i = 0; i < 2; ++i)
        {
            F4Readback read;
            read.source.texture = i == 0 ? a.output.texture : b.output.texture;
            read.destination.buffer = outputs[i] = builder.buffer({64, 4}, "shared.read." + std::to_string(i));
            const auto name = "shared.copy." + std::to_string(i);
            const auto copy = checked(builder.transfer(name, EExecutionScope::VIEW, read));
            commands.push_back({copy, CopyCommand{}, EQueueRole::GRAPHICS});
            CHECK(builder.exportBuffer(outputs[i], PassProducer{passKey(name)}, EGraphOutput::READBACK));
        }
        auto definition = checked(std::move(builder).finish());
        auto logical = checked(compileLogicalGraph(definition));
        auto invocation = makeGraphInvocationData(definition);
        const std::array schemas{PassSchema<F3SharedA>::contract(), PassSchema<F3SharedB>::contract()};
        const std::array owners{
            checked(declareOwnerShape("f4.shared.scene", 1, lux::rdesc::EFieldOwner::SCENE, schemas)),
            checked(declareOwnerShape("f4.shared.feature", 1, lux::rdesc::EFieldOwner::FEATURE, schemas))
        };
        const OwnerAssignment assignment{owners[0].identity, owners[1].identity, {}};
        std::vector<NativeShaderProgram> programs;
        programs.push_back(
            program(device, logical, pa, "F3SharedA", schemas[0], shaders, false, 0, nullptr, owners, assignment)
        );
        programs.push_back(
            program(device, logical, pb, "F3SharedB", schemas[1], shaders, false, 0, nullptr, owners, assignment)
        );
        auto executable = checked(compileVulkanGraph(
            {logical, device, allocator, {queue.get(), queue.get(), queue.get()}, commands, native_imports, invocation},
            std::move(programs)
        ));
        auto bindings = checked(FrameGraphBindings::create(executable.logical(), {}, imports, invocation));
        auto submitted = checked(executable.submit(bindings, native_imports));
        CHECK(checked(queue->wait(submitted.completion, UINT64_MAX)));
        for (unsigned i = 0; i < 2; ++i)
        {
            std::array<float, 16> pixels;
            CHECK(executable.readback(
                submitted,
                GraphResourceId{outputs[i].value()},
                0,
                std::as_writable_bytes(std::span{pixels})
            ));
            for (unsigned pixel = 0; pixel < 4; ++pixel)
            {
                for (unsigned c = 0; c < 4; ++c)
                {
                    CHECK(pixels[pixel * 4 + c] == float(3 + i * 4 + c * 2));
                }
            }
        }
        std::puts("SHARED native_graph full_owner_shape unused_fields CPU_oracle=PASS");
    }
} // namespace native_graph_test
