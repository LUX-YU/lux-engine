#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/capabilities/RenderCapability.hpp>
#include <lux/engine/render/extensions/FeatureRegistration.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <thread>

using namespace lux;
using namespace lux::render;
namespace ipc = lux::platform::ipc;

#define CHECK(value)                                                                                                   \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(value))                                                                                                  \
        {                                                                                                              \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #value);                                                   \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)

template <class T> auto take(T result)
{
    CHECK(result);
    return std::move(*result);
}

struct ProbeUpload final
{
    static constexpr std::string_view name = "example.capability.upload";
    static constexpr bool frame_scoped = false;

    struct Request
    {
        CapabilityInput destination;
        std::span<const std::byte> source;
    };

    struct Response
    {
        RenderError error;
        std::uint64_t bytes = 0;
    };
};

struct ProbeSize final
{
    static constexpr std::string_view name = "example.capability.size";
    static constexpr bool frame_scoped = false;
    using Request = CapabilityInput;
    using Response = ProbeUpload::Response;
};

struct ProbeStorage final
{
    struct Descriptor final
    {
        using Config = void;
        using Ops = void;
        static inline const StorageDescriptor
            descriptor{StorageNameView{"example.storage.buffer"}, 0, 0, {}, "A real independently owned GPU buffer."};
    };

    static inline std::atomic_uint allocations{}, releases{};
    static inline std::atomic_uint64_t submitted_copies{};
    detail::DeviceSession& device;
    detail::ResourceUploader& uploader;
    nri::Buffer* buffer = nullptr;
    std::uint64_t bytes = 0;

    ProbeStorage(StorageCreateContext& dependencies) noexcept
        : device(dependencies.device), uploader(*dependencies.uploads)
    {
    }

    ~ProbeStorage() noexcept
    {
        if (buffer)
        {
            device.deferRelease(
                [device = &device, buffer = buffer]() noexcept
                {
                    device->nriCore().DestroyBuffer(buffer);
                    ++releases;
                }
            );
        }
    }

    static RenderResult<std::unique_ptr<ProbeStorage>> create(StorageCreateContext& dependencies, StorageId) noexcept
    {
        if (!dependencies.uploads)
        {
            return cxx::unexpected(RenderError{kDeviceUnsupported});
        }
        auto storage = std::make_unique<ProbeStorage>(dependencies);
        const auto result = storage->device.nriCore().CreatePlacedBuffer(
            storage->device.device(),
            NriDeviceHeap,
            {4096, 4, nri::BufferUsageBits::SHADER_RESOURCE_STORAGE},
            storage->buffer
        );
        if (result != nri::Result::SUCCESS)
        {
            return cxx::unexpected(RenderError{kDeviceFailure});
        }
        ++allocations;
        return storage;
    }

    static StorageFactory factory() noexcept
    {
        return makeStorageFactory<Descriptor, ProbeStorage, StorageCreateContext, &create>(nullptr);
    }

    exec::task<ProbeUpload::Response> write(std::span<const std::byte> source) noexcept
    {
        if (source.size() > 4096)
        {
            co_return ProbeUpload::Response{RenderError{kCommandContract}};
        }
        const detail::BufferWrite write{buffer, 0, source};
        auto result = co_await uploader.writeBuffers({&write, 1});
        if (!result)
        {
            co_return ProbeUpload::Response{result.error()};
        }
        submitted_copies = uploader.statistics().copy_submissions;
        bytes = source.size();
        co_return ProbeUpload::Response{{}, bytes};
    }
};

struct ProbeHandler final
{
    static constexpr std::string_view name = "example.input.buffer";
    ResourceRegistry& resources;

    static ProbeHandler* create(ResourceRegistry& resources) noexcept
    {
        return new ProbeHandler{resources};
    }

    exec::task<ProbeUpload::Response> upload(ProbeUpload::Request&& request) noexcept
    {
        auto storage = resources.borrowInput<ProbeStorage>(request.destination);
        if (!storage)
        {
            co_return ProbeUpload::Response{storage.error()};
        }
        co_return co_await (*storage)->write(request.source);
    }

    void size(const ProbeSize::Request& request, ProbeSize::Response& response) noexcept
    {
        const auto storage = resources.borrowInput<ProbeStorage>(request);
        response = storage ? ProbeSize::Response{{}, (*storage)->bytes} : ProbeSize::Response{storage.error()};
    }

    using Bindings =
        std::tuple<CommandMethod<ProbeUpload, &ProbeHandler::upload>, CommandMethod<ProbeSize, &ProbeHandler::size>>;
};

static_assert(SyncCommandHandlerFor<ProbeSize, ProbeHandler, &ProbeHandler::size>);
static_assert(AsyncCommandHandlerFor<ProbeUpload, ProbeHandler, &ProbeHandler::upload>);

template <bool Alternate> struct ProbeFeature final
{
    using Config = void;
    using Storages = std::tuple<ProbeStorage>;
    using Commands = ProbeHandler::Bindings;
    static constexpr FeatureName name{Alternate ? "example.buffer.b" : "example.buffer.a"};

    static const FeatureGraphOps& graph(bool) noexcept
    {
        static const std::array inputs{CapabilityStorageInput{
            "example.buffer.bytes",
            1,
            ECapabilityInputScope::GLOBAL,
            ProbeStorage::Descriptor::descriptor.type,
            Alternate ? "b" : "a"
        }};
        static const FeatureGraphOps graph{.storage_inputs = inputs};
        return graph;
    }

    static const CapabilitySupport& capabilities() noexcept
    {
        static const CapabilitySupport value = []
        {
            CapabilitySupport result;
            result.implementation = name.name();
            RenderCapability capability;
            capability.name = "example.buffer";
            RenderPayloadContract input;
            input.schema = "example.buffer.bytes";
            input.setCppType(cxx::typeToken<ProbeUpload::Request>());
            capability.inputs.push_back(std::move(input));
            capability.outputs = {"bytes"};
            result.capabilities.push_back(std::move(capability));
            return result;
        }();
        return value;
    }
};

struct RejectedStorage final
{
    struct Descriptor final
    {
        using Config = void;
        using Ops = void;
        static inline const StorageDescriptor
            descriptor{StorageNameView{"example.storage.reject"}, 0, 0, {}, "A recoverable factory failure."};
    };

    static RenderResult<std::unique_ptr<RejectedStorage>> create(StorageCreateContext&, StorageId) noexcept
    {
        return cxx::unexpected(RenderError{kDeviceCapacity});
    }

    static StorageFactory factory() noexcept
    {
        return makeStorageFactory<Descriptor, RejectedStorage, StorageCreateContext, &create>(nullptr);
    }
};

struct RejectedInstallation final
{
    using Config = void;
    using Storages = std::tuple<ProbeStorage, RejectedStorage>;
    using Commands = std::tuple<>;
    static constexpr FeatureName name{"example.install.failure"};

    static const FeatureGraphOps& graph(bool) noexcept
    {
        static const std::array inputs{
            CapabilityStorageInput{
                "example.buffer.bytes",
                1,
                ECapabilityInputScope::GLOBAL,
                ProbeStorage::Descriptor::descriptor.type,
                "unpublished"
            },
            CapabilityStorageInput{
                "example.reject",
                1,
                ECapabilityInputScope::GLOBAL,
                RejectedStorage::Descriptor::descriptor.type,
                "rejected"
            }
        };
        static const FeatureGraphOps result{.storage_inputs = inputs};
        return result;
    }

    static const CapabilitySupport& capabilities() noexcept
    {
        static const CapabilitySupport result = []
        {
            CapabilitySupport value;
            value.implementation = name.name();
            value.capabilities.push_back(RenderCapability{"example.install.failure"});
            return value;
        }();
        return result;
    }
};

struct IncompatibleFeature final
{
    using Config = void;
    using Storages = ProbeFeature<false>::Storages;
    using Commands = ProbeFeature<false>::Commands;
    static constexpr FeatureName name{"example.incompatible"};

    static const FeatureGraphOps& graph(bool paged) noexcept
    {
        return ProbeFeature<false>::graph(paged);
    }

    static const CapabilitySupport& capabilities() noexcept
    {
        static const CapabilitySupport result = []
        {
            auto value = ProbeFeature<false>::capabilities();
            value.implementation = name.name();
            value.capabilities.front().inputs.front().setCppType(cxx::typeToken<std::uint64_t>());
            return value;
        }();
        return result;
    }
};

template <RenderCommand Command>
auto call(ipc::IpcRequester& client, CommandId command, typename Command::Request request)
{
    static std::uint64_t next = 0;
    const auto correlation = ++next;
    ipc::IpcMessage message;
    message.emplace<typename Command::Request>(std::move(request));
    CHECK(client.trySend(commandTag(command), correlation, message));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;)
    {
        const auto epoch = ipc::IpcCore::instance().workEpoch();
        std::uint64_t received = 0;
        if (client.tryReceive(received, message) == ipc::EIpcReceive::RECEIVED)
        {
            CHECK(received == correlation);
            return std::move(message.get<typename Command::Response>());
        }
        CHECK(std::chrono::steady_clock::now() < deadline);
        ipc::IpcCore::instance().waitUntil(epoch, deadline);
    }
}

int main()
{
    auto execution = take(execution::ExecutionRuntime::create({2, 32, {16}}));
    RenderServerConfig config{"installed.extension", true};
    config.uploads.emplace();
    auto server = take(RenderServer::create(config, execution));
    // Registration is metadata only. Order does not choose the requested implementation.
    take(server->registerFeature<ProbeFeature<true>>());
    take(server->registerFeature<ProbeFeature<false>>());
    take(server->registerFeature<RejectedInstallation>());
    const auto incompatible = server->registerFeature<IncompatibleFeature>();
    CHECK(!incompatible && incompatible.error().type == kFeatureResourceMismatch);
    CHECK(ProbeStorage::allocations == 0);
    const auto list = take(server->commandId<ListCapabilities>());
    const auto install = take(server->commandId<InstallCapabilities>());
    const auto upload = take(server->commandId<ProbeUpload>());
    const auto size = take(server->commandId<ProbeSize>());
    auto client = take(ipc::IpcCore::instance().createRequester(config.channel_name));
    RenderResult<void> ran;
    std::thread thread([&]() noexcept { ran = server->run(); });
    const auto choices = call<ListCapabilities>(client, list, {"example.buffer", 1});
    CHECK(choices.error.type == 0 && choices.implementations.size() == 2);
    CHECK(ProbeStorage::allocations == 0);
    std::vector<CapabilityInput> destinations;
    for (const auto& choice : choices.implementations)
    {
        CHECK(choice.unavailable.type == 0);
        const auto first = call<InstallCapabilities>(client, install, {choice.selection, GlobalRenderScope{}, {}});
        const auto again = call<InstallCapabilities>(client, install, {choice.selection, GlobalRenderScope{}, {}});
        CHECK(first.error.type == 0 && again.error.type == 0 && first.inputs.size() == 1);
        CHECK(first.inputs.front().destination == again.inputs.front().destination);
        destinations.push_back(first.inputs.front().destination);
    }
    CHECK(ProbeStorage::allocations == 2 && destinations[0] != destinations[1]);
    std::array<std::byte, 1024> bytes{};
    for (const auto destination : destinations)
    {
        const auto written = call<ProbeUpload>(client, upload, {destination, bytes});
        CHECK(written.error.type == 0 && written.bytes == bytes.size());
        const auto observed = call<ProbeSize>(client, size, destination);
        CHECK(observed.error.type == 0 && observed.bytes == bytes.size());
    }
    auto wrong = ConfigValue::make(std::uint32_t{1});
    CHECK(wrong);
    const auto rejected = call<InstallCapabilities>(
        client,
        install,
        {choices.implementations.front().selection, GlobalRenderScope{}, std::move(*wrong)}
    );
    CHECK(rejected.error.type == kInvalidConfig && ProbeStorage::allocations == 2);
    const auto failure_choice = call<ListCapabilities>(client, list, {"example.install.failure", 1});
    CHECK(failure_choice.implementations.size() == 1);
    const auto failed = call<InstallCapabilities>(
        client,
        install,
        {failure_choice.implementations.front().selection, GlobalRenderScope{}, {}}
    );
    CHECK(failed.error.type == kDeviceCapacity && failed.inputs.empty() && ProbeStorage::allocations == 3);
    const auto retained = call<ProbeSize>(client, size, destinations.front());
    CHECK(retained.error.type == 0 && retained.bytes == bytes.size());
    client.reset();
    server->stop();
    thread.join();
    CHECK(ran && server->stats().validation_errors == 0 && ProbeStorage::submitted_copies > 0);
    server.reset();
    CHECK(ProbeStorage::allocations == ProbeStorage::releases);
    execution.requestStop();
    CHECK(execution.join());
    std::puts("SDK: two capability choices, one producer, sync/async Handler, real upload and Session cleanup");
}
