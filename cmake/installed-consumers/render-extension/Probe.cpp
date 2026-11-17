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
#include <lux/engine/render/extensions/resource/ResourceRegistry.hpp>
#include <lux/engine/render/extensions/resource/StorageBinding.hpp>
#include <lux/engine/render/extensions/upload/ResourceUploader.hpp>
#include <thread>
#include <source_location>

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

template <class T> auto take(T result, std::source_location location = std::source_location::current())
{
    if (!result)
    {
        std::fprintf(stderr, "Failed result at line %u\n", location.line());
        if constexpr (requires { result.error().type; })
        {
            std::fprintf(stderr, "Render error %llu\n", static_cast<unsigned long long>(result.error().type));
        }
        std::abort();
    }
    return std::move(*result);
}

struct ProbeUpload final
{
    static constexpr std::string_view name = "example.capability.upload";
    static constexpr bool requires_frame_boundary = false;

    struct Request
    {
        ResourceInstanceId resource;
        std::span<const std::byte> source;
    };

    using Data = std::span<const std::byte>;

    static ResourceInstanceId resourceId(const Request& request) noexcept
    {
        return request.resource;
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
        ResourceInstanceId resource;
    };

    using Data = ResourceInstanceId;
    using Response = ProbeUpload::Response;

    static ResourceInstanceId resourceId(const Request& request) noexcept
    {
        return request.resource;
    }
};

template <bool Alternate> struct ProbeResource final
{
    using Config = void;
    using Ops = void;
    using Commands = std::tuple<ProbeUpload, ProbeSize>;
    static inline const ResourceDefinition descriptor{
        ResourceNameView{Alternate ? "example.resource.buffer.b" : "example.resource.buffer.a"},
        0, 0, {}, "An independently owned GPU buffer.", EResourceDomain::DEVICE,
        resourceCommandDescriptors(static_cast<Commands*>(nullptr))
    };
};

struct ProbeStorage final
{
    static inline std::atomic_uint allocations{}, releases{};
    static inline std::atomic_uint64_t submitted_copies{};
    detail::DeviceSession& device;
    detail::ResourceUploader& uploader;
    nri::Buffer* buffer = nullptr;
    std::uint64_t bytes = 0;

    ProbeStorage(ResourceCreateContext& dependencies) noexcept
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

    static RenderResult<std::unique_ptr<ProbeStorage>>
    create(ResourceCreateContext& dependencies, ResourceInstanceId) noexcept
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

    static ResourceRegistration factory() noexcept
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
        static const std::array commands{
            CommandBinding::declare<ProbeUpload, &upload>(), CommandBinding::declare<ProbeSize, &size>()
        };
        return makeResourceRegistration<ProbeResource<false>, ProbeStorage, &create>(nullptr, &binding, commands);
    }

    static exec::task<ProbeUpload::Response> upload(ProbeStorage& storage, ProbeUpload::Request&& request) noexcept
    {
        co_return co_await storage.write(request.source);
    }

    static void size(ProbeStorage& storage, const ProbeSize::Request&, ProbeSize::Response& response) noexcept
    {
        response = {{}, storage.bytes};
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

static_assert(SyncCommandHandlerFor<ProbeSize, void, &ProbeStorage::size>);
static_assert(AsyncCommandHandlerFor<ProbeUpload, void, &ProbeStorage::upload>);

template <bool Alternate> struct ProbeStorageType final
{
    using Descriptor = ProbeResource<Alternate>;
    static ResourceRegistration factory() noexcept
    {
        auto result = ProbeStorage::factory();
        result.descriptor = &Descriptor::descriptor;
        return result;
    }
};

struct ProbeCapability final
{
    using PrerequisiteCapabilities = std::tuple<>;
    using Commands = std::tuple<>;
    using Resources = std::tuple<ProbeResource<false>, ProbeResource<true>>;
};

struct ProbeGraphCapability final
{
    using PrerequisiteCapabilities = std::tuple<>;
    using Commands = std::tuple<>;
    using Resources = std::tuple<ProbeResource<false>>;
};

template <bool Alternate> struct ProbeFeature final
{
    using Capability = ProbeCapability;
    using Features = std::tuple<>;
    using Config = void;
    using Storages = std::tuple<ProbeStorageType<false>, ProbeStorageType<true>>;
    using Bindings = std::tuple<>;
    static constexpr FeatureName name{Alternate ? "example.buffer.b" : "example.buffer.a"};

    static CapabilityImplementation implementation() noexcept
    {
        CapabilityImplementation result;
        result.implementation = name.name();
        result.graph.target_formats = {rdesc::ETextureFormat::RGBA8_UNORM};
        result.graph.target_samples = {1};
        result.graph.target_kinds = {ETargetKind::OFFSCREEN};
        return result;
    }
};

template <bool Matching> struct ProbeEffect final
{
    using Capability = ProbeGraphCapability;
    using Features = std::tuple<ProbeEffect>;
    using Config = void;
    using Storages = ProbeFeature<false>::Storages;
    using Bindings = ProbeFeature<false>::Bindings;
    static constexpr FeatureName name{Matching ? "example.graph.match" : "example.graph.mismatch"};

    using Ports = ProbeEffect;

    static RenderResult<void> describe(const void*, std::string_view, FeaturePorts& ports) noexcept
    {
        (Matching ? ports.storage.global : ports.storage.scene).push_back(
            {"buffer",
             EFeaturePort::OUTPUT,
             GraphBufferDesc{4096},
             EGraphOrigin::IMPORTED,
             {},
             ResourceRequirement{ResourceName{
                 ProbeResource<false>::descriptor.type.name()
             }}}
        );
        GraphImageDesc image{16, 16, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM};
        ports.resources.push_back({"color", EFeaturePort::OUTPUT, image});
        return {};
    }

    static RenderResult<void> build( const void*, std::string_view, FeatureBuildContext& context ) noexcept
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
        static inline const ResourceDefinition
            descriptor{ResourceNameView{"example.storage.reject"}, 0, 0, {}, "A recoverable factory failure."};
    };

    static RenderResult<std::unique_ptr<RejectedStorage>> create(ResourceCreateContext&, ResourceInstanceId) noexcept
    {
        return cxx::unexpected(RenderError{kDeviceCapacity});
    }

    static ResourceRegistration factory() noexcept
    {
        return makeResourceRegistration<Descriptor, RejectedStorage, &create>(nullptr);
    }
};

struct RejectedCapability final
{
    using PrerequisiteCapabilities = std::tuple<>;
    using Commands = std::tuple<>;
    using Resources = std::tuple<ProbeResource<false>, RejectedStorage::Descriptor>;
};

struct RejectedInstallation final
{
    using Capability = RejectedCapability;
    using Features = std::tuple<>;
    using Config = void;
    using Storages = std::tuple<ProbeStorageType<false>, RejectedStorage>;
    using Bindings = std::tuple<>;

    static CapabilityImplementation implementation() noexcept
    {
        CapabilityImplementation result;
        result.implementation = "example.install.failure";
        result.graph.target_formats = {rdesc::ETextureFormat::RGBA8_UNORM};
        result.graph.target_samples = {1};
        result.graph.target_kinds = {ETargetKind::OFFSCREEN};
        return result;
    }
};

struct UndeclaredEffectCapability final
{
    using PrerequisiteCapabilities = std::tuple<>;
    using Commands = std::tuple<ProbeSize>;
    using Resources = std::tuple<>;
};

struct UndeclaredInputFeature final
{
    using Capability = UndeclaredEffectCapability;
    using Features = std::tuple<>;
    using Config = void;
    using Storages = ProbeFeature<false>::Storages;
    using Bindings = std::tuple<>;
    static CapabilityImplementation implementation() noexcept { return ProbeFeature<false>::implementation(); }
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
            auto response = std::move(message.get<typename Command::Response>());
            if (response.error.type != 0)
            {
                for (const auto table : renderErrorDescriptorTables())
                {
                    for (const auto& error : table)
                    {
                        if (error::errorId(error.name) == response.error.type)
                        {
                            std::fprintf(stderr, "%.*s: %.*s [%llu,%llu,%llu]\n",
                                int(Command::name.size()), Command::name.data(),
                                int(error.name.size()), error.name.data(),
                                static_cast<unsigned long long>(response.error.args[0]),
                                static_cast<unsigned long long>(response.error.args[1]),
                                static_cast<unsigned long long>(response.error.args[2]));
                        }
                    }
                }
            }
            return response;
        }
        CHECK(std::chrono::steady_clock::now() < deadline);
        ipc::IpcCore::instance().waitUntil(epoch, deadline);
    }
}

int main()
{
    auto execution = take(execution::ExecutionRuntime::create({2, 32, {16}}));
    const auto* validation = std::getenv("LUX_NATIVE_VALIDATION");
    RenderServerConfig config{"installed.extension", !validation || std::string_view(validation) != "0"};
    config.uploads.emplace();
    auto server = take(RenderServer::create(config, *execution));
    // Registration is metadata only. Order does not choose the requested implementation.
    take(server->registerCapabilityImplementation<ProbeFeature<true>>());
    take(server->registerCapabilityImplementation<ProbeFeature<false>>());
    take(server->registerCapabilityImplementation<RejectedInstallation>());
    take(server->registerCapabilityImplementation<ProbeEffect<true>>());
    take(server->registerCapabilityImplementation<ProbeEffect<false>>());
    // Implementations cannot replace a Capability's Resource identity or duplicate its commands.
    auto remapped_description = describeCapability<ProbeCapability>();
    const std::array remapped_resources{&ProbeResource<false>::descriptor};
    remapped_description.resources = remapped_resources;
    const auto remapped = server->registerCapability(remapped_description);
    CHECK(!remapped && remapped.error().type == kCapabilityConflict);
    auto repeated_description = describeCapability<ProbeGraphCapability>();
    const std::array repeated_commands{&commandDescriptor<ProbeSize>()};
    repeated_description.commands = repeated_commands;
    const auto repeated = server->registerCapability(repeated_description);
    CHECK(!repeated && repeated.error().type == kCapabilityDeclaration);
    const auto undeclared = server->registerCapabilityImplementation<UndeclaredInputFeature>();
    CHECK(!undeclared && undeclared.error().type == kCapabilityDeclaration);
    auto client = take(ipc::IpcCore::instance().createRequester(config.channel_name));
    RenderResult<void> ran;
    std::thread thread([&]() noexcept { ran = server->run(); });
    const std::array names{
        CommandName{ListCapabilities::name}, CommandName{CreateEffect::name}, CommandName{ProbeUpload::name},
        CommandName{ProbeSize::name}, CommandName{GetOrCreateResource::name}
    };
    const auto ids = call<QueryCommandIds>(client, QueryCommandIds::id, names);
    CHECK(ids.error.type == 0 && ids.ids.size() == names.size());
    const auto list = ids.ids[0], create_effect = ids.ids[1], upload = ids.ids[2], size = ids.ids[3];
    const auto choices =
        call<ListCapabilities>(client, list, {std::string(cxx::make_basic_type_info<ProbeCapability>().name())});
    CHECK(choices.error.type == 0 && choices.implementations.size() == 2);
    CHECK(ProbeStorage::allocations == 0);
    const auto make_effect = [&](
        CapabilityImplementationId selected, VCapabilityConfiguration config,
        cxx::type_info capability = cxx::make_basic_type_info<ProbeCapability>()
    )
    {
        const auto graph = call<ListCapabilities>(
            client,
            list,
            {std::string(cxx::make_basic_type_info<ProbeGraphCapability>().name()), "example.graph.match"}
        );
        CreateEffectRequest request{{rdesc::ETextureFormat::RGBA8_UNORM}};
        request.composition.contributions.push_back({
            "commands", capability, selected, std::move(config)
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
    std::vector<ResourceInstanceId> resources;
    for (auto name : {ProbeResource<false>::descriptor.type, ProbeResource<true>::descriptor.type})
    {
        const auto created = call<GetOrCreateResource>(client, ids.ids[4], {GlobalRenderScope{}, name});
        CHECK(created.error.type == 0);
        resources.push_back(created.resource_id);
        const auto written = call<ProbeUpload>(client, upload, {created.resource_id, bytes});
        CHECK(written.error.type == 0 && written.bytes == bytes.size());
        const auto observed = call<ProbeSize>(client, size, {created.resource_id});
        CHECK(observed.error.type == 0 && observed.bytes == bytes.size());
    }
    CHECK(resources[0] != resources[1]);
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
    const auto failed = make_effect(failure_choice.implementations.front().implementation_id, ConfigValue{},
            cxx::make_basic_type_info<RejectedCapability>());
    // Storage factories are not called by CreateEffect. A rejected unused dependency has no allocation side effect.
    CHECK(failed.error.type == 0 && ProbeStorage::allocations == 2);
    const auto retained = call<ProbeSize>(client, size, {resources.front()});
    CHECK(retained.error.type == 0 && retained.bytes == bytes.size());
    client.reset();
    server->stop();
    thread.join();
    CHECK(ran && server->stats().validation_errors == 0 && ProbeStorage::submitted_copies > 0);
    server.reset();
    CHECK(ProbeStorage::allocations == ProbeStorage::releases);
    execution.reset();
    std::puts("SDK: two capability choices, exact methods, real upload and Session cleanup");
}
