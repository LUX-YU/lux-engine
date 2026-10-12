#include "Allocation.hpp"
#include "F4CopyBuffer.pass.hpp"
#include "F6Multi.pass.hpp"
#include "F6Readback.pass.hpp"
#include "NativeGraphSupport.hpp"

using namespace native_graph_test;

static OwnerShape owner(const lux::rdesc::PassShaderContract& schema, unsigned field, std::string_view name)
{
    auto part = schema;
    part.resources = schema.resources.subspan(field, 1);
    const std::array complete{part};
    return checked(declareOwnerShape(name, 1, part.resources[0].owner, complete));
}

static void run(const std::filesystem::path& shaders, Validation& validation, unsigned scenes, unsigned fif, bool reuse)
{
    auto host = instance(validation);
    auto device = checked(VulkanDevice::create(host));
    auto allocator = checked(VulkanAllocator::create(device));
    auto queue = checked(SubmissionQueue::create(device, 128));
    const auto schema = PassSchema<F6Multi>::contract();
    CHECK(schema.resources[0].data && schema.resources[1].data && schema.resources[2].data);
    CHECK(schema.resources[0].data->type_id == schema.resources[1].data->type_id);
    CHECK(schema.resources[0].data->type_id != schema.resources[2].data->type_id);
    const std::array sizes{
        DataSchema<LightGpu>::gpuLayout().size,
        DataSchema<LightGpu>::gpuLayout().size,
        DataSchema<MaterialGpu>::gpuLayout().size,
        16u
    };
    RenderGraphBuilder builder;
    std::array<GraphBuffer, 4> handles;
    for (unsigned i = 0; i < 4; ++i)
    {
        handles[i] = builder.importBuffer(
            "data." + std::to_string(i),
            {sizes[i], i == 3 ? 4u : 16u},
            i == 3 && !reuse ? EPersistentScope::VIEW : EPersistentScope::SCENE
        );
    }
    F6Multi params;
    params.main_light.buffer = handles[0];
    params.preview_light.buffer = handles[1];
    params.material.buffer = handles[2];
    params.output.buffer = reuse ? builder.buffer({16, 4}, "scene.output") : handles[3];
    std::vector<NativePassCommand> commands;
    if (reuse)
    {
        F4CopyBuffer initial;
        initial.source.buffer = handles[3];
        initial.destination.buffer = params.output.buffer;
        auto init = checked(builder.transfer("initialize", EExecutionScope::SCENE, initial));
        commands.push_back({init, CopyCommand{}, EQueueRole::GRAPHICS});
    }
    const auto pass = checked(
        builder
            .compute("multi", {{"F6Multi", "default"}}, reuse ? EExecutionScope::SCENE : EExecutionScope::VIEW, params)
    );
    if (reuse)
    {
        CHECK(builder.source(passKey("multi"), "output", PassProducer{passKey("initialize")}));
    }
    else
    {
        CHECK(builder.source(passKey("multi"), "output", ImportedProducer{}));
    }
    F6Readback read;
    read.source.buffer = params.output.buffer;
    const auto read_pass = checked(builder.readback("read", EExecutionScope::VIEW, read));
    CHECK(builder.source(passKey("read"), "source", PassProducer{passKey("multi")}));
    auto definition = checked(std::move(builder).finish());
    auto logical = checked(compileLogicalGraph(definition));
    auto values = makeGraphInvocationData(definition);
    const std::array owners{
        owner(schema, 0, "lighting.main"),
        owner(schema, 1, "lighting.preview"),
        owner(schema, 2, "material.surface"),
        owner(schema, 3, "pass.output")
    };
    std::array<OwnerAssignment, 4> assignments;
    for (unsigned i = 0; i < 4; ++i)
    {
        assignments[i] = {schema.resources[i].path, owners[i].identity};
    }
    const auto caps = queryLayoutCaps(device);
    using Clock = std::chrono::steady_clock;
    const auto elapsed = [](auto start)
    { return std::chrono::duration<double, std::micro>(Clock::now() - start).count(); };
    const auto layout_start = Clock::now();
    auto layout = checked(compileLayout(schema, assignments, owners, caps));
    const auto layout_us = elapsed(layout_start);
    CHECK(layout.identity().owners.size() == 4 && layout.identity().sets.size() == 2);
    CHECK(layout.identity().fields[0].binding != layout.identity().fields[1].binding);
    auto duplicate = assignments;
    duplicate[1].field_path = duplicate[0].field_path;
    CHECK(!compileLayout(schema, duplicate, owners, caps));
    CHECK(!compileLayout(schema, std::span(assignments).first(3), owners, caps));
    duplicate = assignments;
    duplicate[0].field_path = "absent";
    CHECK(!compileLayout(schema, duplicate, owners, caps));
    auto wrong = owners;
    wrong[0].fields[0].data_identity += "different inactive ABI field";
    CHECK(!compileLayout(schema, assignments, wrong, caps));
    auto foreign = assignments;
    foreign[0].owner = owners[2].identity;
    CHECK(!compileLayout(schema, foreign, owners, caps));
    auto lowered = caps;
    lowered.limits.maxBoundDescriptorSets = 1;
    CHECK(!compileLayout(schema, assignments, owners, lowered));

    // Complete owner shapes include fields unused by this particular Shader.
    auto complete = owners;
    auto inactive = complete[0].fields[0];
    inactive.semantic = "unused";
    complete[0].fields.push_back(inactive);
    complete[0].identity = shaderOwnerId(complete[0]);
    auto complete_assignments = assignments;
    complete_assignments[0].owner = complete[0].identity;
    auto full = checked(compileLayout(schema, complete_assignments, complete, caps));
    CHECK(!layout.matches(full));
    std::reverse(complete[0].fields.begin(), complete[0].fields.end());
    CHECK(shaderOwnerId(complete[0]) == complete[0].identity);
    CHECK(full.matches(checked(compileLayout(schema, complete_assignments, complete, caps))));
    complete[0].fields[0].count += 1;
    complete[0].identity = shaderOwnerId(complete[0]);
    complete_assignments[0].owner = complete[0].identity;
    CHECK(!full.matches(checked(compileLayout(schema, complete_assignments, complete, caps))));
    const auto shader_start = Clock::now();
    auto variant = makeCompiledShaderVariant(
        {"F6Multi",
         "default",
         "glslc VulkanSDK 1.4.304.0",
         "vulkan1.3",
         {{"F6Multi.comp.glsl", text(shaders / "F6Multi.comp.glsl")}},
         {}},
        {{4, words(shaders / "F6Multi.comp.spv")}},
        schema
    );
    if (!variant)
    {
        std::fprintf(stderr, "%s\n", variant.error().c_str());
    }
    CHECK(variant);
    const auto shader_us = elapsed(shader_start);
    // Real reflection rejects corrupted GPU metadata even when DataSchema is validated.
    std::vector<lux::rdesc::PassResourceField> changed_fields(schema.resources.begin(), schema.resources.end());
    auto changed_schema = schema;
    changed_schema.resources = changed_fields;
    auto abi_fields = std::vector<lux::rdesc::DataGpuField>(
        schema.resources[0].data->gpu.fields.begin(),
        schema.resources[0].data->gpu.fields.end()
    );
    abi_fields[0].offset += 4;
    changed_fields[0].data->gpu.fields = abi_fields;
    CHECK(!makeCompiledShaderVariant(variant->identity().inputs, variant->identity().stages, changed_schema));
    changed_fields[0] = schema.resources[0];
    changed_fields[0].data->type_id ^= 1;
    CHECK(!makeCompiledShaderVariant(variant->identity().inputs, variant->identity().stages, changed_schema));
    changed_fields[0] = schema.resources[0];
    changed_fields[0].data->gpu.fingerprint = schema.resources[1].data->gpu.fingerprint;
    changed_fields[0].data->gpu.fields = abi_fields;
    CHECK(!makeCompiledShaderVariant(variant->identity().inputs, variant->identity().stages, changed_schema));
    wrong = owners;
    wrong[0].identity = owners[1].identity; // Simulated collision is never a cache hit.
    CHECK(!compileLayout(schema, assignments, wrong, caps));
    wrong = owners;
    wrong[0].fields[0].count += 1;
    wrong[0].identity = shaderOwnerId(wrong[0]);
    CHECK(!compileLayout(schema, assignments, wrong, caps));
    wrong = owners;
    wrong[0].fields[0].flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    wrong[0].identity = shaderOwnerId(wrong[0]);
    CHECK(!compileLayout(schema, assignments, wrong, caps));
    const auto program_start = Clock::now();
    std::vector<NativeShaderProgram> programs;
    programs.push_back(checked(
        NativeShaderProgram::create(device, logical, pass, *variant, schema, assignments, owners, ComputeDescription{})
    ));
    const auto program_us = elapsed(program_start);
    const auto pipeline = programs[0].native();
    const auto pipeline_layout = programs[0].pipelineLayout();
    commands.push_back({pass, DispatchCommand{4, 1, 1}, EQueueRole::GRAPHICS});
    commands.push_back({read_pass, CopyCommand{}, EQueueRole::GRAPHICS});
    auto plan = checked(
        compileVulkanGraph({logical, device, {queue.get(), queue.get(), queue.get()}, commands}, std::move(programs))
    );
    auto shared_plan = plan;
    CHECK(shared_plan.shaderPrograms().data() == plan.shaderPrograms().data());
    CHECK(shared_plan.shaderPrograms()[0].native() == pipeline);
    CHECK(shared_plan.shaderPrograms()[0].pipelineLayout() == pipeline_layout);

    std::vector<Buffer> buffers;
    buffers.reserve(scenes * 3 + 4 * fif);
    for (unsigned scene = 0; scene < scenes; ++scene)
    {
        for (unsigned field = 0; field < 3; ++field)
        {
            buffers.push_back(checked(
                Buffer::create(allocator, sizes[field], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, EMemoryAccess::UPLOAD)
            ));
            std::array<std::byte, 32> bytes{};
            if (field < 2)
            {
                LightGpu light{};
                light.position.intensity = 2.0f;
                for (unsigned c = 0; c < 4; ++c)
                {
                    light.color[c] = float(scene * 10 + field * 4 + c + 1);
                }
                CHECK(DataSchema<LightGpu>::pack(light, std::span(bytes).first(sizes[field])));
            }
            else
            {
                MaterialGpu material{};
                for (unsigned c = 0; c < 4; ++c)
                {
                    material.tint[c] = float(c + 2);
                }
                material.roughness = 0.5f;
                CHECK(DataSchema<MaterialGpu>::pack(material, std::span(bytes).first(sizes[field])));
            }
            CHECK(buffers.back().write(0, std::span(bytes).first(sizes[field])));
        }
    }
    for (unsigned slot = 0; slot < 4 * fif; ++slot)
    {
        buffers.push_back(checked(
            Buffer::create(
                allocator,
                16,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                EMemoryAccess::UPLOAD
            )
        ));
        const std::array<float, 4> zero{};
        CHECK(buffers.back().write(0, std::as_bytes(std::span(zero))));
    }
    // The same production descriptor admission also rejects bad multi-owner values.
    std::array<OwnerDescriptorValue, 4> descriptor_values{
        OwnerDescriptorValue{
            owners[0].identity,
            "records",
            0,
            BufferDescriptorValue{std::cref(buffers[0]), 0, sizes[0]}
        },
        OwnerDescriptorValue{
            owners[1].identity,
            "records",
            0,
            BufferDescriptorValue{std::cref(buffers[1]), 0, sizes[1]}
        },
        OwnerDescriptorValue{
            owners[2].identity,
            "records",
            0,
            BufferDescriptorValue{std::cref(buffers[2]), 0, sizes[2]}
        },
        OwnerDescriptorValue{
            owners[3].identity,
            "output",
            0,
            BufferDescriptorValue{std::cref(buffers[scenes * 3]), 0, 16}
        }
    };
    {
        auto complete_sets = checked(BoundDescriptorSets::create(device, plan.shaderPrograms()[0], descriptor_values));
        CHECK(!BoundDescriptorSets::create(device, plan.shaderPrograms()[0], std::span(descriptor_values).first(3)));
        auto duplicate_values = descriptor_values;
        duplicate_values[1].owner = owners[0].identity;
        CHECK(!BoundDescriptorSets::create(device, plan.shaderPrograms()[0], duplicate_values));
        std::vector<OwnerDescriptorValue> extra(descriptor_values.begin(), descriptor_values.end());
        extra.push_back(descriptor_values[0]);
        CHECK(!BoundDescriptorSets::create(device, plan.shaderPrograms()[0], extra));
        if (scenes == 1 && fif == 2 && !reuse)
        {
            auto foreign_device = checked(VulkanDevice::create(host));
            auto foreign_allocator = checked(VulkanAllocator::create(foreign_device));
            auto foreign_buffer = checked(
                Buffer::create(foreign_allocator, sizes[0], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, EMemoryAccess::UPLOAD)
            );
            duplicate_values = descriptor_values;
            duplicate_values[0].value = BufferDescriptorValue{std::cref(foreign_buffer), 0, sizes[0]};
            CHECK(!BoundDescriptorSets::create(device, plan.shaderPrograms()[0], duplicate_values));
        }
    }
    std::vector<NativeImportBinding> imports;
    imports.reserve(4);
    for (unsigned i = 0; i < 4; ++i)
    {
        imports.push_back({{}, {}, std::cref(buffers[0])});
    }
    std::array<GraphImportBinding, 4> bindings;
    const auto bind = [&](unsigned scene, unsigned slot, bool swap)
    {
        for (unsigned i = 0; i < 4; ++i)
        {
            const auto index = i == 3 ? scenes * 3 + (reuse ? scene : slot) : scene * 3 + (swap && i < 2 ? 1 - i : i);
            const auto resource = GraphResourceId{handles[i].value()};
            imports[i] = {
                resource,
                GraphBackingId{index + 1},
                std::cref(buffers[index]),
                {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT},
                {},
                true
            };
            bindings[i] = {resource, GraphBackingId{index + 1}, 0, 1};
        }
    };
    bind(0, 0, false);
    const auto invocation_start = Clock::now();
    allocation_count = allocation_bytes = 0;
    measuring = true;
    auto invocations = checked(NativeGraphInvocations::create(plan, {device, allocator, imports, values, {}, 4 * fif}));
    measuring = false;
    const auto invocation_us = elapsed(invocation_start);
    const auto cold_allocations = allocation_count.load(), cold_bytes = allocation_bytes.load();
    std::array<double, 36> samples{};
    unsigned sample_count = 0;
    std::array<std::optional<GraphSubmission>, 12> receipts{};
    std::array<std::optional<SubmissionTicket>, 2> scene_last_use;
    std::array<NativeRangeState, 1> output_state;
    std::vector<NativeTraceEvent> trace(invocations.traceCapacity());
    unsigned reused = 0;
    // Warm every slot. Next batches have no cold allocation or native creation.
    for (unsigned batch = 0; batch < 3; ++batch)
    {
        // Change Scene A's actual data only after every previous GPU borrow completed.
        // Scene B must keep its original result while both retain the same Native program.
        if (batch == 2)
        {
            MaterialGpu changed{};
            for (unsigned c = 0; c < 4; ++c)
            {
                changed.tint[c] = float(c + 2);
            }
            changed.roughness = 20.5f;
            std::array<std::byte, 32> bytes{};
            CHECK(DataSchema<MaterialGpu>::pack(changed, bytes));
            CHECK(buffers[2].write(0, bytes));
        }
        allocation_count = allocation_bytes = 0;
        std::array<std::optional<GraphSubmission>, 2> donors;
        measuring = batch != 0;
        for (unsigned slot = 0; slot < 4 * fif; ++slot)
        {
            const unsigned view = slot % 4, scene = view % scenes;
            if (slot % 4 == 0)
            {
                donors = {};
            }
            bind(scene, slot, batch == 2);
            if (scene_last_use[scene])
            {
                for (unsigned i = 0; i < 3; ++i)
                {
                    imports[i].initial = {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
                    imports[i].ready = scene_last_use[scene];
                    imports[i].host_ready = false;
                }
            }
            if (batch == 2 && slot == 0)
            {
                imports[2].initial = {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT};
                imports[2].ready.reset();
                imports[2].host_ready = true;
            }
            if (reuse && scene_last_use[scene])
            {
                imports[3].initial = {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
                imports[3].ready = scene_last_use[scene];
                imports[3].host_ready = false;
            }
            if (!reuse && receipts[slot])
            {
                CHECK(
                    checked(
                        invocations.resourceStates(*receipts[slot], GraphResourceId{handles[3].value()}, output_state)
                    ) == 1
                );
                imports[3].ranges = output_state;
            }
            values.scene = scene + 1;
            values.view = view + 1;
            values.target = view + 1;
            auto frame = checked(
                FrameGraphBindings::create(plan.logical(), {batch * fif + slot / 4, 0, slot}, bindings, values)
            );
            const auto submit_start = Clock::now();
            receipts[slot] =
                checked(invocations.submit(slot, frame, imports, trace, reuse ? donors[scene] : std::nullopt));
            if (batch != 0)
            {
                samples[sample_count++] = elapsed(submit_start);
            }
            if (!donors[scene])
            {
                donors[scene] = receipts[slot];
            }
            for (unsigned t = 0; t < receipts[slot]->trace_count; ++t)
            {
                reused += trace[t].operation == ENativeTrace::REUSE_SCENE;
            }
            scene_last_use[scene] = receipts[slot]->completion;
        }
        measuring = false;
        CHECK(allocation_count == 0 && allocation_bytes == 0);
        for (unsigned slot = 0; slot < 4 * fif; ++slot)
        {
            CHECK(checked(queue->wait(receipts[slot]->completion, UINT64_MAX)));
        }
        // Donor readback requires completion of its accepted consumer borrows too.
        for (unsigned slot = 0; slot < 4 * fif; ++slot)
        {
            std::array<float, 4> actual;
            CHECK(invocations.readbackPass(*receipts[slot], read_pass, 0, 0, std::as_writable_bytes(std::span(actual)))
            );
            recordReadback(
                "F6Multi",
                batch * 12 + slot,
                GraphResourceId{params.output.buffer.value()},
                std::as_bytes(std::span(actual))
            );
            const unsigned scene = slot % 4 % scenes;
            for (unsigned c = 0; c < 4; ++c)
            {
                const float main = float(scene * 10 + (batch == 2 ? 4 : 0) + c + 1);
                const float preview = float(scene * 10 + (batch == 2 ? 0 : 4) + c + 1);
                const float addition = batch == 2 && scene == 0 ? 22.5f : 2.5f;
                CHECK(actual[c] == main * float(c + 2) + preview + addition);
            }
        }
    }
    std::printf(
        "MULTI_OWNER_COLD layout_us=%.3f shader_validation_us=%.3f relocation_native_program_us=%.3f "
        "invocation_us=%.3f heap_count=%llu heap_bytes=%llu descriptor_sets=%u descriptor_native_bytes=UNAVAILABLE\n",
        layout_us,
        shader_us,
        program_us,
        invocation_us,
        static_cast<unsigned long long>(cold_allocations),
        static_cast<unsigned long long>(cold_bytes),
        4 * fif * 2
    );
    for (unsigned i = 0; i < sample_count; ++i)
    {
        std::printf("MULTI_OWNER_SAMPLE %u %.3f\n", i, samples[i]);
    }
    std::sort(samples.begin(), samples.begin() + sample_count);
    std::printf(
        "MULTI_OWNER_HOT unit=us p50=%.3f p95=%.3f max=%.3f qualification=NEW_BASELINE_VALIDATION_ON\n",
        samples[sample_count / 2],
        samples[(sample_count - 1) * 95 / 100],
        samples[sample_count - 1]
    );
    CHECK(reused == (reuse ? 3 * fif * (4 - scenes) * 2 : 0));
    std::printf("MULTI_OWNER scene_pass_reuses=%u\n", reused);
    std::printf(
        "MULTI_OWNER scenes=%u views=4 FIF=%u pipelines=1 shared_code=1 slots=%u oracle=PASS warm_allocations=0\n",
        scenes,
        fif,
        4 * fif
    );
}

int main(int argc, char** argv)
{
    CHECK(argc == 2);
    Validation validation;
    for (unsigned scenes : {1u, 2u})
    {
        for (unsigned fif : {2u, 3u})
        {
            for (bool reuse : {false, true})
            {
                run(argv[1], validation, scenes, fif, reuse);
            }
        }
    }
    CHECK(validation.errors.load() == 0);
}
