#pragma once

// Included by the ordinary and fault-injected GPU executables.
template <typename Id>
concept DomainAddress = requires(DeviceSession& session, Id id) { session.getResourceDomain(id); };
static_assert(DomainAddress<ResourceDomainId>);
static_assert(!DomainAddress<RenderSceneId>);
static_assert(!std::is_convertible_v<RenderSceneId, ResourceDomainId>);
static_assert(ResourceFactoryFor<BufferFactory, Buffer>);
static_assert(std::same_as<decltype(std::declval<ResourceDomain&>().tryGet<Buffer>()), const Buffer*>);

struct CountedBufferFactory
{
    BufferFactory factory;
    unsigned& calls;

    RenderResult<std::unique_ptr<Buffer>> operator()() noexcept
    {
        ++calls;
        return factory();
    }

    bool isCompatible(const Buffer& buffer) const noexcept
    {
        return factory.isCompatible(buffer);
    }
};

struct RootLifetime
{
    bool& alive;

    explicit RootLifetime(bool& value) noexcept : alive(value)
    {
        alive = true;
    }

    ~RootLifetime()
    {
        alive = false;
    }
};

struct ChildLifetime
{
    const bool& root_alive;
    unsigned& destroyed;
    SubmissionTicket ticket;

    ChildLifetime(const bool& alive, unsigned& count, SubmissionTicket submitted) noexcept
        : root_alive(alive), destroyed(count), ticket(submitted)
    {
    }

    ~ChildLifetime()
    {
        CHECK(root_alive && ticket.owner().completed() >= ticket.serial());
        ++destroyed;
    }
};

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
    auto session = checked(DeviceSession::create(options));
    const auto devices = checked(session->instance.physicalDevices());
    CHECK(std::find(devices.begin(), devices.end(), session->device.device.physical()) != devices.end());
    CHECK(session->device.device.instance() == session->instance.instance.native());
    CHECK(session->resources.allocator.native() == session->device.allocator.native());
    auto& root = session->getResourceDomain(session->rootDomain());
    unsigned calls{};
    ValueFactory<int> integer{calls, 7};
    ValueFactory<double> scalar{calls, 1.25};
    CHECK(checked(root.getOrCreate<int>(integer)).get() == 7);
    CHECK(checked(root.getOrCreate<double>(scalar)).get() == 1.25);
    CHECK(root.getOrCreate<int>(integer) && root.getOrCreate<double>(scalar) && calls == 2);
    CHECK(root.erase<int>() && root.tryGet<double>() && !root.tryGet<int>());
    calls = 0;
    CountedBufferFactory factory{session->resources.bufferFactory(256, VK_BUFFER_USAGE_TRANSFER_DST_BIT), calls};
    auto& buffer = checked(root.getOrCreate<Buffer>(factory)).get();
    CHECK(buffer.native() && calls == 1);
    CHECK(&checked(root.getOrCreate<Buffer>(factory)).get() == &buffer && calls == 1);
    auto different = factory;
    different.factory.size = 512;
    auto conflict = root.getOrCreate<Buffer>(different);
    CHECK(!conflict && conflict.error().type == kDefinitionMismatch && calls == 1);
    auto child_id = checked(session->createResourceDomain(session->rootDomain()));
    auto& child = session->getResourceDomain(child_id);
    CHECK(child.tryGet<Buffer>() == &buffer && !child.tryGetLocal<Buffer>());
    auto& local = checked(child.getOrCreate<Buffer>(different)).get();
    CHECK(child.tryGet<Buffer>() == &local && root.tryGet<Buffer>() == &buffer && calls == 2);
    CHECK(&child.getLocal<Buffer>() == &local);
    CHECK(!session->removeResourceDomain(session->rootDomain()));
    auto grandchild = checked(session->createResourceDomain(child_id));
    CHECK(!session->removeResourceDomain(child_id));
    CHECK(session->removeResourceDomain(grandchild));
    auto* queue = session->device.queues[0];
    std::vector<ResourceDomainId> extra;
    for (unsigned i = 0; i < 128; ++i)
    {
        extra.push_back(checked(session->createResourceDomain(session->rootDomain())));
    }
    CHECK(session->removeResourceDomain(extra[0])); // dense swap/pop must not move any live Domain
    CHECK(&session->getResourceDomain(child_id) == &child && child.tryGet<Buffer>() == &local);
    CHECK(&session->getResourceDomain(session->rootDomain()) == &root);
    CHECK(session->device.queues[0] == queue);
    auto native =
        checked(Buffer::create(session->device.allocator, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::DEVICE));
    auto owned = std::make_unique<Buffer>(std::move(native));
    CHECK(!child.getOrCreate<Buffer>(std::move(owned))); // explicit occupied-type conflict
    CHECK(child.erase<Buffer>() && !child.erase<Buffer>());
    CHECK(child.tryGet<Buffer>() == &buffer);
    auto adopted =
        checked(Buffer::create(session->device.allocator, 128, VK_BUFFER_USAGE_TRANSFER_DST_BIT, EMemoryAccess::DEVICE)
        );
    auto pointer = std::make_unique<Buffer>(std::move(adopted));
    auto* address = pointer.get();
    CHECK(&checked(child.getOrCreate<Buffer>(std::move(pointer))).get() == address);
    CHECK(session->removeResourceDomain(child_id));
    CHECK(!session->tryGetResourceDomain(child_id));
    auto replacement = checked(session->createResourceDomain(session->rootDomain()));
    CHECK(replacement.index == child_id.index && replacement.gen != child_id.gen);
    CHECK(!session->removeResourceDomain(child_id));
    bool root_alive{};
    unsigned child_destroyed{};
    auto batch = checked(queue->begin());
    vkCmdFillBuffer(batch.native(), buffer.native(), 0, buffer.size(), 0x12345678);
    const auto ticket = checked(std::move(batch).submit());
    CHECK(queue->lastSubmitted() && queue->lastSubmitted()->serial() == ticket.serial());
    CHECK(root.getOrCreate<RootLifetime>(std::make_unique<RootLifetime>(root_alive)));
    CHECK(session->getResourceDomain(replacement)
              .getOrCreate<ChildLifetime>(std::make_unique<ChildLifetime>(root_alive, child_destroyed, ticket)));
    session.reset();
    CHECK(!root_alive && child_destroyed == 1);
    std::puts("P2A Domain Native Buffer lazy/config/adopt/ancestor/generation/stable-address PASS");
}
