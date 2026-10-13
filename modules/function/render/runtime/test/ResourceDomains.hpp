#pragma once
#include "EffectAttachment.hpp"

// Replaces the superseded Domain tree tests, whose exact P2-A source remains in archive/.
static_assert(ResourceFactoryFor<BufferFactory, Buffer>);
static_assert(!std::is_copy_constructible_v<ResourceLease<Buffer>>);
static_assert(std::is_nothrow_move_constructible_v<ResourceLease<Buffer>>);

template <typename T> struct ValueFactory
{
    unsigned& calls;
    T value;

    RenderResult<std::unique_ptr<T>> operator()() noexcept
    {
        ++calls;
        return std::make_unique<T>(value);
    }

    bool isCompatible(const T& existing) const noexcept
    {
        return existing == value;
    }
};

void domains(Validation& validation)
{
    BackendOptions options;
    options.instance = {{}, true, diagnostic, &validation};
    InstanceContext host{checked(VulkanInstance::create(options.instance))};
    auto session = checked(DeviceSession::create(host, options));
    const auto devices = checked(host.physicalDevices());
    CHECK(std::find(devices.begin(), devices.end(), session->device.device.physical()) != devices.end());
    CHECK(&session->instance == &host);
    CHECK(session->device.device.instance() == host.instance.native());
    CHECK(session->resources.allocator.native() == session->device.allocator.native());
    auto& store = session->resources.scoped;
    unsigned calls{};
    ValueFactory<int> integers{calls, 7};
    ValueFactory<double> doubles{calls, 1.25};
    {
        auto a = checked(store.getOrCreate<int>({}, "main", integers));
        auto b = checked(store.getOrCreate<int>({}, "main", integers));
        auto preview = checked(store.getOrCreate<int>({}, "preview", integers));
        auto scalar = checked(store.getOrCreate<double>({}, "main", doubles));
        CHECK(a.get() == 7 && scalar.get() == 1.25 && calls == 3);
        CHECK(&a.get() == &b.get() && &a.get() != &preview.get() && a.useCount() == 2);
        auto conflict = integers;
        conflict.value = 8;
        CHECK(!store.getOrCreate<int>({}, "main", conflict));
        CHECK(calls == 3 && store.size() == 3);
        auto moved = std::move(a);
        b = {};
        CHECK(moved.useCount() == 1 && store.collect() && moved.get() == 7);
    }
    CHECK(store.collect() && store.size() == 0);
    auto factory =
        session->resources.bufferFactory(256, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto buffer = checked(store.getOrCreate<Buffer>({}, "gpu", factory));
    const auto old = buffer.id();
    auto* address = &buffer.get();
    std::vector<ResourceLease<int>> extra;
    for (unsigned i = 0; i < 128; ++i)
    {
        extra.push_back(checked(store.getOrCreate<int>({}, std::to_string(i), integers)));
    }
    extra[0] = {};
    CHECK(store.collect() && &buffer.get() == address);
    auto incompatible = factory;
    incompatible.size = 512;
    CHECK(!store.getOrCreate<Buffer>({}, "gpu", incompatible));
    auto* queue = session->device.queues[0];
    {
        RenderGraphBuilder builder;
        auto source = builder.importBuffer("r3a.pending", {256, 4}, EPersistentScope::RUNTIME);
        F5CReadBuffer params;
        params.source = {source, {0, 16}};
        auto pass = checked(builder.readback("read", EExecutionScope::VIEW, params));
        auto definition = checked(std::move(builder).finish());
        auto logical = checked(compileLogicalGraph(definition));
        auto values = makeGraphInvocationData(definition);
        const std::array commands{NativePassCommand{pass, CopyCommand{}, EQueueRole::GRAPHICS}};
        auto code =
            checked(compileVulkanGraph({logical, session->device.device, session->device.queues, commands}, {}));
        const auto backing = GraphBackingId{(std::uint64_t{old.gen} << 32) | (std::uint64_t{old.index} + 1)};
        std::array native{NativeImportBinding{
            GraphResourceId{source.value()},
            backing,
            std::cref(buffer.get()),
            {VK_PIPELINE_STAGE_2_TRANSFER_BIT,
             VK_ACCESS_2_TRANSFER_WRITE_BIT,
             VK_IMAGE_LAYOUT_UNDEFINED,
             queue->nativeQueue().family},
            buffer.ready(),
            true
        }};
        auto execution = std::make_unique<GraphExecution>(GraphExecution{
            checked(GraphExecutionContext::create(
                code,
                {session->device.device, session->device.allocator, native, values, {}, 2}
            )),
            {BindingExecution{true, values}},
            {}
        });
        auto attachment = std::make_unique<EffectAttachment>(RenderEffectId{}, EffectBinding{}, RenderGraphId{}, code);
        attachment->leases.push_back(std::move(buffer));
        attachment->imports.push_back({GraphResourceId{source.value()}, backing, 0, 1});
        attachment->context = execution.get();
        auto frame = checked(FrameGraphBindings::create(code.logical(), {}, attachment->imports, values));
        VkEventCreateInfo event_info{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        VkEvent gate{};
        CHECK(vkCreateEvent(session->device.device.native(), &event_info, nullptr, &gate) == VK_SUCCESS);
        auto batch = checked(queue->begin());
        vkCmdWaitEvents(
            batch.native(),
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
        auto ticket = checked(std::move(batch).submit());
        auto submitted = executeGraph(execution->native, 0, frame, native);
        attachment->retainAccepted(session->device.queues);
        native[0].ready = attachment->leases[0].lastGraphicsUse();
        native[0].initial.stages = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        native[0].initial.access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        auto second = executeGraph(execution->native, 1, frame, native);
        attachment->retainAccepted(session->device.queues);
        const auto size = store.size();
        attachment.reset();
        const auto collected = store.collect();
        const bool retained = collected && store.size() == size && queue->completed() < ticket.serial();
        CHECK(vkSetEvent(session->device.device.native(), gate) == VK_SUCCESS);
        CHECK(checked(queue->wait(ticket, UINT64_MAX)));
        vkDestroyEvent(session->device.device.native(), gate, nullptr);
        CHECK(retained && submitted && second && !execution->bindings[0].occupied);
        auto receipt = checked(std::move(second));
        CHECK(checked(queue->wait(receipt.completion, UINT64_MAX)));
        std::array<unsigned, 4> read{};
        CHECK(execution->native.readbackPass(receipt, pass, 0, 0, std::as_writable_bytes(std::span{read})));
        CHECK(std::all_of(read.begin(), read.end(), [](unsigned word) { return word == 0; }));
        execution.reset();
        CHECK(store.collect() && store.size() == size - 1);
    }
    auto replacement = checked(store.getOrCreate<Buffer>({}, "gpu", factory));
    CHECK(replacement.id().index == old.index && replacement.id().gen != old.gen);
    replacement = {};
    extra.clear();
    CHECK(store.collect() && store.size() == 0);
    session.reset();
    CHECK(!checked(host.physicalDevices()).empty()); // instance still alive after complete Session teardown
    std::puts("R3A scoped typed/key/config/lease/move/generation/stable-address and real in-flight retirement PASS");
}
