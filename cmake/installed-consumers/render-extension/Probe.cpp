#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/capabilities/CapabilityCatalog.hpp>
#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/extensions/CommandMethod.hpp>
#include <lux/engine/render/extensions/Registration.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>
#include <lux/engine/render/extensions/features/FeatureBuildContext.hpp>
#include <lux/engine/render/extensions/storage/ResourceRegistry.hpp>
#include <lux/engine/render/extensions/storage/StorageBinding.hpp>
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
    static constexpr bool requires_frame_boundary = false;

    struct Request
    {
        CapabilityImplementationId implementation;
        std::span<const std::byte> source;
    };

    using Data = std::span<const std::byte>;

    static CapabilityImplementationId implementationId(const Request& request) noexcept
    {
        return request.implementation;
    }

    struct Response
    {
        RenderError error;
        std::uint64_t bytes = 0;
    };
};

struct ProbeSize final
{
    static constexpr std::string_view name = "example.capability.size";
    static constexpr bool requires_frame_boundary = false;

    struct Request
    {
        CapabilityImplementationId implementation;
    };

    using Data = CapabilityImplementationId;
    using Response = ProbeUpload::Response;

    static CapabilityImplementationId implementationId(const Request& request) noexcept
    {
        return request.implementation;
    }
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
        static const detail::StorageBindingOps binding{
            [](void* object,
               const VGraphResourceDesc&,
               const void*,
               detail::StorageSubscription&,
               std::span<detail::ResourceInput> outputs) noexcept -> RenderResult<void>
            {
                auto& storage = *static_cast<ProbeStorage*>(object);
                for (auto& output : outputs)
                {
                    output = {storage.buffer, 0, 4096};
                }
                return {};
            }
        };
        return makeStorageFactory<Descriptor, ProbeStorage, StorageCreateContext, &create>(nullptr, &binding);
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
    static ProbeHandler* create(ResourceRegistry&) noexcept
    {
        return new ProbeHandler;
    }

    exec::task<ProbeUpload::Response> upload(ProbeStorage& storage, ProbeUpload::Request&& request) noexcept
    {
        co_return co_await storage.write(request.source);
    }

    void size(ProbeStorage& storage, const ProbeSize::Request&, ProbeSize::Response& response) noexcept
    {
        response = {{}, storage.bytes};
    }
};

static_assert(SyncCommandHandlerFor<ProbeSize, ProbeHandler, &ProbeHandler::size>);
static_assert(AsyncCommandHandlerFor<ProbeUpload, ProbeHandler, &ProbeHandler::upload>);

struct ProbeCapability final
{
    using PrerequisiteCapabilities = std::tuple<>;
    using Commands = std::tuple<ProbeUpload, ProbeSize>;
    using PerFrameData = std::tuple<>;
};

struct ProbeGraphCapability final
{
    using PrerequisiteCapabilities = std::tuple<>;
    using Commands = ProbeCapability::Commands;
    using PerFrameData = std::tuple<>;
};

struct RejectedCapability final
{
    using PrerequisiteCapabilities = std::tuple<>;
    using Commands = std::tuple<>;
    using PerFrameData = std::tuple<>;
};

struct IncompleteDataCapability final
{
    using PrerequisiteCapabilities = std::tuple<>;
    using Commands = ProbeCapability::Commands;
    using PerFrameData = std::tuple<std::uint32_t>;
};

template <bool Alternate> struct ProbeFeature final
{
    using Capability = ProbeCapability;
    using Features = std::tuple<>;
    using FrameBindings = std::tuple<>;
    using Config = void;
    using Storages = std::tuple<ProbeStorage>;
    using Bindings = std::
        tuple<CommandMethod<ProbeUpload, &ProbeHandler::upload, 0>, CommandMethod<ProbeSize, &ProbeHandler::size, 0>>;
    static constexpr FeatureName name{Alternate ? "example.buffer.b" : "example.buffer.a"};

    static CapabilityImplementation implementation() noexcept
    {
        static const std::array dependencies{CapabilityStorageDependency{
            EStorageScope::GLOBAL,
            ProbeStorage::Descriptor::descriptor.type,
            Alternate ? "b" : "a"
        }};
        CapabilityImplementation result;
        result.implementation = name.name();
        result.storage_dependencies = dependencies;
        return result;
    }
};

template <bool Matching> struct ProbeEffect final
{
    using Capability = ProbeGraphCapability;
    using Features = std::tuple<ProbeEffect>;
    using FrameBindings = std::tuple<>;
    using Config = void;
    using Storages = ProbeFeature<false>::Storages;
    using Bindings = ProbeFeature<false>::Bindings;
    static constexpr FeatureName name{Matching ? "example.graph.match" : "example.graph.mismatch"};

    static const FeatureGraphOps& graph(bool) noexcept
    {
        static const FeatureGraphOps result{
            .describe_ports = [](const void*, std::string_view, FeaturePorts& ports) noexcept -> RenderResult<void>
            {
                ports.storage.global.push_back(
                    {"buffer",
                     EFeaturePort::OUTPUT,
                     GraphBufferDesc{4096},
                     EGraphOrigin::IMPORTED,
                     {},
                     StorageRequirement{
                         StorageName{ProbeStorage::Descriptor::descriptor.type.name()},
                         Matching ? "a" : "wrong-destination"
                     }}
                );
                GraphImageDesc image{16, 16, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM};
                ports.resources.push_back({"color", EFeaturePort::OUTPUT, image});
                return {};
            },
            .build = [](const void*, std::string_view, FeatureBuildContext& context) noexcept -> RenderResult<void>
            {
                auto pass = context.graph.addTransferPass(
                    "fill",
                    TransferWork{
                        {FillBuffer{{context.resource("buffer")}, 42},
                         ClearImage{{context.resource("color")}, {0, 0, 0, 1}}}
                    }
                );
                if (!pass)
                {
                    return cxx::unexpected(pass.error());
                }
                return context.publish("color", pass->written[1]);
            }
        };
        return result;
    }

    static CapabilityImplementation implementation() noexcept
    {
        static const auto result = []
        {
            auto value = ProbeFeature<false>::implementation();
            value.implementation = name.name();
            value.graph.ports = {
                {"color",
                 ECapabilityPortDirection::OUTPUT,
                 "example.color",
                 CapabilityImageShape{rdesc::ETextureFormat::RGBA8_UNORM, 1, 1},
                 "viewport"}
            };
            value.graph.target_formats = {rdesc::ETextureFormat::RGBA8_UNORM};
            value.graph.target_samples = {1};
            value.graph.target_kinds = {ETargetKind::OFFSCREEN};
            return value;
        }();
        return result;
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
    using Capability = RejectedCapability;
    using Features = std::tuple<>;
    using FrameBindings = std::tuple<>;
    using Config = void;
    using Storages = std::tuple<ProbeStorage, RejectedStorage>;
    using Bindings = std::tuple<>;

    static CapabilityImplementation implementation() noexcept
    {
        static const std::array dependencies{
            CapabilityStorageDependency{
                EStorageScope::GLOBAL,
                ProbeStorage::Descriptor::descriptor.type,
                "unpublished"
            },
            CapabilityStorageDependency{EStorageScope::GLOBAL, RejectedStorage::Descriptor::descriptor.type, "rejected"}
        };
        CapabilityImplementation result;
        result.implementation = "example.install.failure";
        result.storage_dependencies = dependencies;
        return result;
    }
};

struct IncompatibleFeature final
{
    using Capability = IncompleteDataCapability;
    using Features = std::tuple<>;
    using FrameBindings = std::tuple<>; // Missing required per-frame method must reject registration.
    using Config = void;
    using Storages = ProbeFeature<false>::Storages;
    using Bindings = ProbeFeature<false>::Bindings;

    static CapabilityImplementation implementation() noexcept
    {
        return ProbeFeature<false>::implementation();
    }
};

struct RemappedFeature final
{
    using Capability = ProbeCapability;
    using Features = std::tuple<>;
    using FrameBindings = std::tuple<>;
    using Config = void;
    using Storages = ProbeFeature<false>::Storages;
    using Bindings = ProbeFeature<false>::Bindings;

    static CapabilityImplementation implementation() noexcept
    {
        static const std::array dependencies{CapabilityStorageDependency{
            EStorageScope::GLOBAL,
            ProbeStorage::Descriptor::descriptor.type,
            "silently-remapped"
        }};
        auto result = ProbeFeature<false>::implementation();
        result.storage_dependencies = dependencies;
        return result;
    }
};

struct UndeclaredInputFeature final
{
    using Capability = ProbeCapability;
    using Features = std::tuple<>;
    using FrameBindings = std::tuple<>;
    using Config = void;
    using Storages = ProbeFeature<false>::Storages;
    using Bindings = std::tuple<>; // Missing required discrete methods, not a payload-schema mismatch.

    static CapabilityImplementation implementation() noexcept
    {
        return ProbeFeature<false>::implementation();
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
    auto server = take(RenderServer::create(config, *execution));
    // Registration is metadata only. Order does not choose the requested implementation.
    take(server->registerCapabilityImplementation<ProbeFeature<true>>());
    take(server->registerCapabilityImplementation<ProbeFeature<false>>());
    take(server->registerCapabilityImplementation<RejectedInstallation>());
    take(server->registerCapabilityImplementation<ProbeEffect<true>>());
    take(server->registerCapabilityImplementation<ProbeEffect<false>>());
    const auto incompatible = server->registerCapabilityImplementation<IncompatibleFeature>();
    CHECK(!incompatible && incompatible.error().type == kCapabilityDeclaration);
    const auto remapped = server->registerCapabilityImplementation<RemappedFeature>();
    CHECK(!remapped && remapped.error().type == kCapabilityConflict);
    const auto undeclared = server->registerCapabilityImplementation<UndeclaredInputFeature>();
    CHECK(!undeclared && undeclared.error().type == kCapabilityDeclaration);
    const auto list = take(server->commandId<ListCapabilities>());
    const auto create_effect = take(server->commandId<CreateEffect>());
    const auto upload = take(server->commandId<ProbeUpload>());
    const auto size = take(server->commandId<ProbeSize>());
    auto client = take(ipc::IpcCore::instance().createRequester(config.channel_name));
    RenderResult<void> ran;
    std::thread thread([&]() noexcept { ran = server->run(); });
    const auto choices =
        call<ListCapabilities>(client, list, {std::string(cxx::make_basic_type_info<ProbeCapability>().name())});
    CHECK(choices.error.type == 0 && choices.implementations.size() == 2);
    CHECK(ProbeStorage::allocations == 0);
    const auto make_effect = [&](CapabilityImplementationId selected, VCapabilityConfiguration config)
    {
        const auto graph = call<ListCapabilities>(
            client,
            list,
            {std::string(cxx::make_basic_type_info<ProbeGraphCapability>().name()), "example.graph.match"}
        );
        CreateEffectRequest request{{rdesc::ETextureFormat::RGBA8_UNORM}};
        request.composition.contributions.push_back({
            "commands", cxx::make_basic_type_info<ProbeCapability>(), selected, std::move(config)
        });
        request.composition.contributions.push_back(
            {"output", graph.implementations.front().capability.type, graph.implementations.front().implementation_id,
             ConfigValue{}}
        );
        request.composition.outputs.push_back({"output", "color"});
        return call<CreateEffect>(client, create_effect, std::move(request));
    };
    std::vector<CapabilityImplementationId> implementations;
    for (const auto& choice : choices.implementations)
    {
        const auto effect = make_effect(choice.implementation_id, ConfigValue{});
        CHECK(effect.error.type == 0 && ProbeStorage::allocations == 0);
        implementations.push_back(choice.implementation_id);
    }
    CHECK(implementations[0] != implementations[1]);
    std::array<std::byte, 1024> bytes{};
    for (const auto implementation : implementations)
    {
        const auto written = call<ProbeUpload>(client, upload, {implementation, bytes});
        CHECK(written.error.type == 0 && written.bytes == bytes.size());
        const auto observed = call<ProbeSize>(client, size, {implementation});
        CHECK(observed.error.type == 0 && observed.bytes == bytes.size());
    }
    for (const char* name : {"example.graph.match", "example.graph.mismatch"})
    {
        const auto choice = call<ListCapabilities>(
            client,
            list,
            {std::string(cxx::make_basic_type_info<ProbeGraphCapability>().name()), name}
        );
        CHECK(choice.error.type == 0 && choice.implementations.size() == 1);
        CreateEffectRequest request{{rdesc::ETextureFormat::RGBA8_UNORM}};
        request.composition.contributions.push_back(
            {"probe", choice.implementations.front().capability.type, choice.implementations.front().implementation_id,
             ConfigValue{}}
        );
        request.composition.outputs.push_back({"probe", "color"});
        const auto effect = call<CreateEffect>(client, create_effect, std::move(request));
        if (std::string_view(name) == "example.graph.match")
        {
            CHECK(effect.error.type == 0 && !effect.effect.isNull());
        }
        else
        {
            CHECK(effect.error.type == kStorageMismatch && effect.effect.isNull());
        }
        CHECK(ProbeStorage::allocations == 2);
    }
    const auto rejected =
        make_effect(choices.implementations.front().implementation_id, take(ConfigValue::make(std::uint32_t{1})));
    CHECK(rejected.error.type == kInvalidConfig && ProbeStorage::allocations == 2);
    const auto failure_choice =
        call<ListCapabilities>(client, list, {std::string(cxx::make_basic_type_info<RejectedCapability>().name())});
    CHECK(failure_choice.implementations.size() == 1);
    const auto failed = make_effect(failure_choice.implementations.front().implementation_id, ConfigValue{});
    // Storage factories are not called by CreateEffect. A rejected unused dependency has no allocation side effect.
    CHECK(failed.error.type == 0 && ProbeStorage::allocations == 2);
    const auto retained = call<ProbeSize>(client, size, {implementations.front()});
    CHECK(retained.error.type == 0 && retained.bytes == bytes.size());
    client.reset();
    server->stop();
    thread.join();
    CHECK(ran && server->stats().validation_errors == 0 && ProbeStorage::submitted_copies > 0);
    server.reset();
    CHECK(ProbeStorage::allocations == ProbeStorage::releases);
    execution->requestStop();
    CHECK(execution->join());
    std::puts("SDK: two capability choices, exact methods, real upload and Session cleanup");
}
