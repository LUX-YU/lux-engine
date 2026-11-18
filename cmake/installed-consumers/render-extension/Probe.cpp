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

struct ProbeIdentity final
{
    static constexpr std::string_view name = "example.handler.identity";
    using Request = std::uint32_t;
    struct Response final
    {
        RenderError error;
        std::uint32_t value = 0;
    };
};

struct ProbeHandler final
{
    static constexpr std::string_view name = "example.handler";
    static ProbeHandler* create(ResourceRegistry&) noexcept { return new ProbeHandler; }
    void read(const ProbeIdentity::Request& request, ProbeIdentity::Response& response) noexcept
    {
        response = {{}, request};
    }
    using Bindings = std::tuple<CommandMethod<ProbeIdentity, &ProbeHandler::read>>;
};

struct ProbeEffectCommand final
{
    static constexpr std::string_view name = "example.effect.route";
    using Request = EffectId;
    using Data = std::uint32_t;
    using Response = ProbeIdentity::Response;

    static EffectId effectId(const Request& request) noexcept { return request; }
};

struct RoutedCapability final
{
    using PrerequisiteCapabilities = std::tuple<>;
    using Commands = std::tuple<ProbeEffectCommand>;
    using Resources = std::tuple<>;
};

template <std::uint32_t Value> struct RoutedImplementation final
{
    using Capability = RoutedCapability;
    using Features = std::tuple<>;

    static void read(const EffectId&, ProbeEffectCommand::Response& response) noexcept
    {
        response = {{}, Value};
    }

    using Bindings = std::tuple<CommandMethod<ProbeEffectCommand, &read>>;
    static CapabilityImplementation implementation() noexcept
    {
        CapabilityImplementation result;
        result.implementation = Value == 17 ? "example.effect.route.a" : "example.effect.route.b";
        result.outputs = {{rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN}};
        return result;
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

struct ProbeReference final
{
    static constexpr std::string_view name = "example.resource.reference";

    struct Request final
    {
        ResourceInstanceId owner;
        ResourceInstanceId dependency;
    };
    using Data = Request;
    using Response = AckResponse;

    static ResourceInstanceId resourceId(const Request& request) noexcept { return request.owner; }
};

template <EResourceDomain Domain> struct DomainResource final
{
    using Config = void;
    using Ops = void;
    using Commands = std::tuple<ProbeReference>;
    static inline const ResourceDefinition descriptor{
        ResourceNameView{Domain == EResourceDomain::SCENE ? "example.resource.scene" : "example.resource.view"},
        0, 0, {}, "Resource domain probe.", Domain, resourceCommandDescriptors(static_cast<Commands*>(nullptr))
    };
};

struct DomainStorage final
{
    static inline std::vector<ResourceInstanceId> destroyed;
    ResourceRegistry& registry;
    ResourceInstanceId id;

    ~DomainStorage() noexcept { destroyed.push_back(id); }

    static RenderResult<std::unique_ptr<DomainStorage>>
    create(ResourceCreateContext& context, ResourceInstanceId id) noexcept
    {
        return std::make_unique<DomainStorage>(context.resources, id);
    }

    static void reference(DomainStorage& storage, const ProbeReference::Request& request, AckResponse& reply) noexcept
    {
        reply.error = storage.registry.canReference(storage.id, request.dependency) ? RenderError{} :
            RenderError{kStorageMismatch};
    }

    template <EResourceDomain Domain> static ResourceRegistration factory() noexcept
    {
        static const std::array commands{CommandBinding::declare<ProbeReference, &reference>()};
        return makeResourceRegistration<DomainResource<Domain>, DomainStorage, &create>(nullptr, nullptr, commands);
    }
};

struct ProbeGraphCapability final
{
    using PrerequisiteCapabilities = std::tuple<>;
    using Commands = std::tuple<>;
    using Resources = std::tuple<ProbeResource<false>>;
};

struct QualificationAuditCapability final
{
    using PrerequisiteCapabilities = std::tuple<>;
    using Commands = std::tuple<>;
    using Resources = std::tuple<>;
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
        result.outputs = {{rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN}};
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
            value.outputs = {{rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN}};
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
        result.outputs = {{rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN}};
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

static void checkResourceDomains(ipc::IpcRequester& client, ResourceInstanceId global)
{
    const std::array names{
        CommandName{CreateScene::name}, CommandName{CreateView::name}, CommandName{RemoveScene::name},
        CommandName{RemoveView::name}, CommandName{GetOrCreateResource::name}, CommandName{QueryResource::name},
        CommandName{ProbeReference::name}
    };
    const auto commands = call<QueryCommandIds>(client, QueryCommandIds::id, names);
    CHECK(commands.error.type == 0 && commands.ids.size() == names.size());
    const auto& ids = commands.ids;
    const auto make_scene = [&]
    {
        const auto reply = call<CreateScene>(client, ids[0], {});
        CHECK(reply.error.type == 0);
        return reply.scene;
    };
    const auto make_view = [&](SceneId scene) -> ViewRequest
    {
        const auto reply = call<CreateView>(client, ids[1], {scene});
        CHECK(reply.error.type == 0);
        return {scene, reply.view};
    };
    const auto ensure = [&](const VRenderScope& scope, ResourceNameView name)
    {
        const auto reply = call<GetOrCreateResource>(client, ids[4], {scope, name});
        CHECK(reply.error.type == 0);
        return reply.resource_id;
    };
    const auto reference = [&](ResourceInstanceId owner, ResourceInstanceId dependency)
    {
        return call<ProbeReference>(client, ids[6], {owner, dependency}).error.type;
    };
    const auto first = make_scene();
    const auto second = make_scene();
    const auto first_view = make_view(first);
    const auto second_view = make_view(second);
    CHECK(first_view.view == second_view.view);
    constexpr auto scene_domain = EResourceDomain::SCENE;
    constexpr auto view_domain = EResourceDomain::VIEW;
    const auto scene_name = DomainResource<scene_domain>::descriptor.type;
    const auto view_name = DomainResource<view_domain>::descriptor.type;
    const auto global_name = ProbeResource<false>::descriptor.type;
    CHECK(ensure(first, global_name) == global);
    CHECK(ensure(second_view, global_name) == global);
    const auto first_scene_resource = ensure(first, scene_name);
    const auto second_scene_resource = ensure(second_view, scene_name);
    CHECK(first_scene_resource != second_scene_resource);
    CHECK(ensure(first_view, scene_name) == first_scene_resource);
    const auto first_resource = ensure(first_view, view_name);
    const auto second_resource = ensure(second_view, view_name);
    CHECK(first_resource != second_resource && first_resource != first_scene_resource);
    CHECK(call<QueryResource>(client, ids[5], {first, view_name}).error.type == kStorageMismatch);
    CHECK(reference(first_resource, global) == 0);
    CHECK(reference(first_resource, first_scene_resource) == 0);
    CHECK(reference(first_resource, second_scene_resource) == kStorageMismatch);
    CHECK(reference(first_resource, second_resource) == kStorageMismatch);
    CHECK(reference(first_scene_resource, first_resource) == kStorageMismatch);

    CHECK(call<RemoveScene>(client, ids[2], {first}).error.type == 0);
    CHECK((DomainStorage::destroyed == std::vector{first_resource, first_scene_resource}));
    CHECK(reference(first_resource, global) == kStorageIdentity);
    CHECK(call<QueryResource>(client, ids[5], {first_view, global_name}).error.type == kRuntimeIdentity);
    CHECK(reference(second_resource, second_scene_resource) == 0);
    CHECK(call<RemoveView>(client, ids[3], second_view).error.type == 0);
    const auto replacement = make_view(second);
    const auto replacement_resource = ensure(replacement, view_name);
    CHECK(replacement.view != second_view.view && replacement_resource != second_resource);
    CHECK(reference(second_resource, global) == kStorageIdentity);
    CHECK(reference(replacement_resource, second_scene_resource) == 0);
    CHECK(ensure(second, global_name) == global && ProbeStorage::allocations == 2);
    // Stop tears down the remaining Scene and View after settling accepted requests.
}

int main(int argc, char** argv)
{
    auto execution = take(execution::ExecutionRuntime::create({2, 32, {16}}));
    const auto* validation = std::getenv("LUX_NATIVE_VALIDATION");
    RenderServerConfig config{"installed.extension", !validation || std::string_view(validation) != "0"};
    config.uploads.emplace();
    DeviceOptions device;
    if (argc > 1 && std::string_view(argv[1]) == "d3d12") device.backend = EGraphicsBackend::D3D12;
    auto server = take(RenderServer::create(config, *execution, device));
    // The module installs its unique declarations once; implementations reference that closure.
    take(server->registerResource<ProbeStorageType<false>>());
    take(server->registerResource<ProbeStorageType<true>>());
    take(server->registerResource(DomainStorage::factory<EResourceDomain::SCENE>()));
    take(server->registerResource(DomainStorage::factory<EResourceDomain::VIEW>()));
    take(server->registerResource<RejectedStorage>());
    take(server->registerCapability<RoutedCapability>());
    const std::array effect_calls{
        CommandBinding::declare<ProbeEffectCommand, &RoutedImplementation<17>::read>(),
        CommandBinding::declare<ProbeEffectCommand, &RoutedImplementation<29>::read>()
    };
    CHECK(server->registerCommands(effect_calls));
    const auto route_a = take(server->registerCapabilityImplementation<RoutedImplementation<17>>());
    const auto route_b = take(server->registerCapabilityImplementation<RoutedImplementation<29>>());
    take(server->registerCapability<ProbeCapability>());
    take(server->registerCapability<ProbeGraphCapability>());
    take(server->registerCapability<RejectedCapability>());
    take(server->registerCapability<UndeclaredEffectCapability>());
    const auto graph_feature = take(server->registerFeature<ProbeEffect<true>>());
    take(server->registerFeature<ProbeEffect<false>>());
    // Registration is metadata only. Order does not choose the requested implementation.
    take(server->registerCapabilityImplementation<ProbeFeature<true>>());
    const auto original = take(server->registerCapabilityImplementation<ProbeFeature<false>>());
    take(server->registerCapabilityImplementation<RejectedInstallation>());
    take(server->registerCapabilityImplementation<ProbeEffect<true>>());
    take(server->registerCapabilityImplementation<ProbeEffect<false>>());
    const auto audit = take(server->registerCapability<QualificationAuditCapability>());
    auto candidate = ProbeFeature<false>::implementation();
    candidate.implementation = "example.audit.undeclared_resource";
    candidate.features = {graph_feature};
    const auto undeclared_resource = take(server->addCapabilityImplementation(audit, candidate));
    const auto graph_capability = take(server->capabilityId(cxx::make_basic_type_info<ProbeGraphCapability>()));
    candidate.implementation = "example.graph.output_matrix";
    candidate.outputs.push_back(candidate.outputs.front());
    candidate.outputs.push_back({rdesc::ETextureFormat::BGRA8_UNORM, 1, ETargetKind::SURFACE});
    const auto output_matrix = take(server->addCapabilityImplementation(graph_capability, candidate));
    candidate.outputs.push_back({rdesc::ETextureFormat::BGRA8_UNORM, 4, ETargetKind::SURFACE});
    const auto invalid = server->replaceCapabilityImplementation(output_matrix, candidate);
    CHECK(!invalid && invalid.error().type == kInvalidTargetKind);
    const auto duplicate_resource = server->registerResource<ProbeStorageType<false>>();
    CHECK(!duplicate_resource && duplicate_resource.error().type == kStorageMismatch);
    const auto duplicate_feature = server->registerFeature<ProbeEffect<true>>();
    CHECK(!duplicate_feature && duplicate_feature.error().type == kFeatureDuplicate);
    const auto duplicate_implementation = server->registerCapabilityImplementation<ProbeFeature<false>>();
    CHECK(!duplicate_implementation && duplicate_implementation.error().type == kCapabilityConflict);
    auto changed = ProbeFeature<false>::implementation();
    changed.description = "A different payload must not replace the registered identity.";
    const auto capability = take(server->capabilityId(cxx::make_basic_type_info<ProbeCapability>()));
    const auto conflicting = server->addCapabilityImplementation(capability, changed);
    CHECK(!conflicting && conflicting.error().type == kCapabilityConflict);
    const auto replacement = take(server->replaceCapabilityImplementation(original, changed));
    CHECK(replacement != original);
    CHECK(!server->removeCapabilityImplementation(original));
    auto temporary = ProbeFeature<false>::implementation();
    temporary.implementation = "example.temporary";
    const auto temporary_id = take(server->addCapabilityImplementation(capability, temporary));
    CHECK(server->removeCapabilityImplementation(temporary_id));

    const auto handler = take(server->registerHandler<ProbeHandler, ProbeHandler::Bindings>());
    const auto duplicate_handler = server->registerHandler<ProbeHandler, ProbeHandler::Bindings>();
    CHECK(!duplicate_handler && duplicate_handler.error().type == kCommandContract);
    CHECK(take(server->handlerId(cxx::make_basic_type_info<ProbeHandler>())) == handler);
    const auto other_handler = server->registerHandler<ProbeHandler, ProbeHandler::Bindings>("other");
    CHECK(!other_handler);
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
    const auto handler_commands = call<QueryCommandIds>(client, QueryCommandIds::id,
        std::array{CommandName{ProbeIdentity::name}});
    CHECK(handler_commands.error.type == 0 && handler_commands.ids.size() == 1);
    const auto identity = call<ProbeIdentity>(client, handler_commands.ids.front(), 42);
    CHECK(identity.error.type == 0 && identity.value == 42);
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
    const auto make_effect = [&](CapabilityImplementationId selected, ConfigValue config)
    {
        const auto graph = call<ListCapabilities>(
            client,
            list,
            {std::string(cxx::make_basic_type_info<ProbeGraphCapability>().name()), "example.graph.match"}
        );
        CreateEffectRequest request{{rdesc::ETextureFormat::RGBA8_UNORM}};
        const auto& output = graph.implementations.front();
        request.implementations = {{selected, "commands"}, {output.implementation_id, "output"}};
        request.definition.features.push_back({output.features.front(), "output", std::move(config)});
        request.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{request.output.format, 1}});
        return call<CreateEffect>(client, create_effect, std::move(request));
    };
    const auto graph_choice = call<ListCapabilities>(
        client, list, {std::string(cxx::make_basic_type_info<ProbeGraphCapability>().name()), "example.graph.match"}
    ).implementations.front();
    {
        auto qualify = [&](std::vector<EffectImplementationUse> implementations, std::string contribution,
                           EffectOutputDesc output = {rdesc::ETextureFormat::RGBA8_UNORM})
        {
            CreateEffectRequest request{output};
            request.implementations = std::move(implementations);
            request.definition.features.push_back({graph_feature, std::move(contribution), {}});
            request.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{request.output.format, 1}});
            return call<CreateEffect>(client, create_effect, std::move(request));
        };
        const auto owner = graph_choice.implementation_id;
        const auto wrong_feature = qualify({{route_a, "commands"}, {owner, "output"}}, "commands/node");
        const auto wrong_resource = qualify({{undeclared_resource, "output"}}, "output/node");
        CHECK(wrong_feature.error.type == kCapabilityDeclaration && wrong_feature.effect.isNull());
        CHECK(wrong_resource.error.type == kCapabilityDeclaration && wrong_resource.effect.isNull());
        CHECK(qualify({{owner, "output"}}, "output_invalid").error.type == kCapabilityDeclaration);
        CHECK(qualify({{owner, "output"}, {owner, "output/nested"}}, "output/nested/node").error.type ==
            kCapabilityDeclaration);
        CHECK(!qualify({{owner, "output"}, {undeclared_resource, "other"}}, "other/node").error.type);
        CHECK(!qualify({}, "free").error.type);

        const EffectOutputDesc supported{rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN};
        const EffectOutputDesc crossed{rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::SURFACE};
        const auto output_status = [&](EffectOutputDesc output)
        {
            const auto options = call<ListCapabilities>(client, list, {{}, candidate.implementation, output});
            CHECK(!options.error.type && options.implementations.size() == 1);
            return options.implementations.front().unavailable.type;
        };
        CHECK(!output_status(supported));
        CHECK(!qualify({{output_matrix, "output"}}, "output/node", supported).error.type);
        CHECK(output_status(crossed) == kRuntimeUnsupported);
        CHECK(qualify({{output_matrix, "output"}}, "output/node", crossed).error.type == kRuntimeUnsupported);
        std::puts("SDK QUALIFICATION: contribution ownership, resource union, free graph and exact output tuples");
    }
    auto make_routed = [&](std::span<const CapabilityImplementationId> routes)
    {
        CreateEffectRequest request{{rdesc::ETextureFormat::RGBA8_UNORM}};
        request.implementations.push_back({graph_choice.implementation_id, "output"});
        for (std::size_t i = 0; i < routes.size(); ++i)
            request.implementations.push_back({routes[i], i == 0 ? "a" : "b"});
        request.definition.features.push_back({graph_choice.features.front(), "output", {}});
        request.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{request.output.format, 1}});
        return call<CreateEffect>(client, create_effect, std::move(request));
    };
    const auto effect_a = make_routed({&route_a, 1});
    const auto effect_b = make_routed({&route_b, 1});
    const std::array incompatible{route_a, route_b};
    CHECK(!effect_a.error.type && !effect_b.error.type);
    CHECK(make_routed(incompatible).error.type == kCapabilityDeclaration);
    const std::array route_name{CommandName{ProbeEffectCommand::name}};
    const auto route_id = call<QueryCommandIds>(client, QueryCommandIds::id, route_name).ids.front();
    CHECK(call<ProbeEffectCommand>(client, route_id, effect_a.effect).value == 17);
    CHECK(call<ProbeEffectCommand>(client, route_id, effect_b.effect).value == 29);
    CHECK(call<ProbeEffectCommand>(client, route_id, {}).error.type == kRuntimeIdentity);
    std::puts("EFFECT ROUTING PASS: installed calls selected by EffectId");

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
        const auto& selected = choice.implementations.front();
        request.implementations.push_back({selected.implementation_id, "probe"});
        request.definition.features.push_back({selected.features.front(), "probe", {}});
        request.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{request.output.format, 1}});
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
    const auto retained = call<ProbeSize>(client, size, {resources.front()});
    CHECK(retained.error.type == 0 && retained.bytes == bytes.size());
    checkResourceDomains(client, resources.front());
    client.reset();
    server->stop();
    thread.join();
    CHECK(ran && server->stats().validation_errors == 0 && ProbeStorage::submitted_copies > 0);
    server.reset();
    CHECK(ProbeStorage::allocations == ProbeStorage::releases);
    CHECK(DomainStorage::destroyed.size() == 5);
    execution.reset();
    std::puts("SDK: real upload, shared Device Resource, Scene/View isolation and stale IDs, ordered cleanup");
}
