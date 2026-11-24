#include <lux/engine/render/runtime/commands/CommandBinding.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exec/create.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/platform/ipc/IpcCore.hpp>
#include <lux/engine/render/runtime/commands/Commands.hpp>
#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/runtime/commands/CommandMethod.hpp>
#include <lux/engine/render/runtime/commands/Registration.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/effect/EffectCompileContext.hpp>
#include <lux/engine/render/effect/EffectImplementation.hpp>
#include <lux/engine/render/effect/EffectInitializationContext.hpp>
#include <lux/engine/render/resource/ResourceRegistry.hpp>
#include <lux/engine/render/resource/StorageBinding.hpp>
#include <lux/engine/render/device/ResourceUploader.hpp>
#include <optional>
#include <thread>
#include <type_traits>
#include <source_location>

using namespace lux;
using namespace lux::render;
namespace ipc = lux::platform::ipc;

static_assert(!std::is_default_constructible_v<EffectCompileContext>);
static_assert(!std::is_copy_constructible_v<EffectCompileContext>);
static_assert(!std::is_polymorphic_v<EffectCompileContext>);

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

struct CommandContractProbe final
{
    static void mayThrow(const ProbeIdentity::Request&, ProbeIdentity::Response&) {}
    static void wrongRequest(const std::string&, ProbeIdentity::Response&) noexcept {}

    static auto validSender(ProbeIdentity::Request&&) noexcept
    {
        return stdexec::just(ProbeIdentity::Response{}) | stdexec::then([](auto value) noexcept { return value; });
    }

    static auto wrongError(ProbeIdentity::Request&&) noexcept
    {
        return exec::create<stdexec::set_value_t(ProbeIdentity::Response), stdexec::set_error_t(int)>(
            [](auto& context) noexcept { stdexec::set_error(std::move(context.receiver), 1); }
        );
    }
};

template <auto Method>
concept DeclaresIdentityCommand = requires { CommandBinding::declare<ProbeIdentity, Method>(); };

template <auto Method>
concept NamesIdentityMethod = requires { typename CommandMethod<ProbeIdentity, Method>; };

template <class Handler, class Bindings>
concept RegistersHandler = requires(RenderServer& server) { server.registerHandler<Handler, Bindings>(); };

template <class Storage>
concept RegistersStorage = requires(RenderServer& server) { server.registerResource<Storage>(); };

static_assert(NamesIdentityMethod<&ProbeHandler::read> && DeclaresIdentityCommand<&ProbeHandler::read>);
static_assert(!NamesIdentityMethod<&CommandContractProbe::mayThrow>);
static_assert(!DeclaresIdentityCommand<&CommandContractProbe::wrongRequest>);
static_assert(DeclaresIdentityCommand<&CommandContractProbe::validSender>);
static_assert(!DeclaresIdentityCommand<&CommandContractProbe::wrongError>);
static_assert(RegistersHandler<ProbeHandler, ProbeHandler::Bindings>);
static_assert(!RegistersHandler<CommandContractProbe, ProbeHandler::Bindings>);
static_assert(!RegistersHandler<ProbeHandler, std::tuple<int>>);
static_assert(!RegistersStorage<CommandContractProbe>);

struct ProbeEffectCommand final
{
    static constexpr std::string_view name = "example.effect.route";
    struct Request final
    {
        EffectId effect;
        graph::NodeId node;
        std::optional<std::uint32_t> value;
    };
    struct Response final
    {
        RenderError error;
        std::uint32_t value = 0;
        std::uint64_t revision = 0;
    };
    using Data = std::uint32_t;

    static EffectId effectId(const Request& request) noexcept { return request.effect; }
    static graph::NodeId nodeId(const Request& request) noexcept { return request.node; }
};

struct ProbeEffectState final
{
    std::uint32_t value;
    std::uint64_t revision = 1;
};

static constexpr std::string_view routed_capability = "example.effect.route";
static constexpr std::string_view route_a = "example.effect.route.a";
static constexpr std::string_view route_b = "example.effect.route.b";
static constexpr std::string_view route_failure = "example.effect.route.failed_state";
static constexpr std::string_view transfer_capability = "example.graph.transfer";
static constexpr auto previous_color = effectPortSemantic("previous");
static constexpr auto output_color = effectPortSemantic("color");
static constexpr auto readback_value = effectPortSemantic("value");
static constexpr std::string_view color_type = "example.graph.color";
static constexpr std::string_view readback_binding = "readback";
static constexpr std::uint32_t transfer_bytes = 4096;
static constexpr std::uint32_t history_distance = 2;
static constexpr std::uint32_t readback_frames = history_distance + 1;
static constexpr std::uint32_t fallback_value = 17;
static constexpr std::uint32_t expected_value = 42;

inline constexpr EffectPortDefinition kRoutedCapabilityPorts[]{
            {previous_color, "previous", {color_type, 1}, graph::EPinDirection::INPUT},
            {output_color, "color", {color_type, 1}, graph::EPinDirection::OUTPUT}
        };

inline constexpr EffectCapability kRoutedCapability{
    .id = graph::nodeTypeId(routed_capability), .canonical_name = routed_capability, .ports = kRoutedCapabilityPorts
};

static RenderResult<ResourceVersion> clearColor(EffectCompileContext& context) noexcept
{
    const auto& target = context.target();
    if (target.format != rdesc::ETextureFormat::RGBA8_UNORM || target.samples != 1)
        return cxx::unexpected(RenderError{kRuntimeUnsupported});
    GraphImageDesc image{16, 16};
    image.extent.source = EImageExtentSource::TARGET;
    const auto output = context.declareResource({"color", image});
    if (!output) return cxx::unexpected(output.error());
    const auto clear = context.addTransferPass("clear", TransferWork{{ClearImage{{{*output, 0}}, {0, 0, 0, 1}}}});
    if (!clear) return cxx::unexpected(clear.error());
    return clear->written.front();
}

static RenderResult<void> buildRouted(EffectCompileContext& context, std::string_view) noexcept
{
    const auto previous = context.inputs(previous_color);
    if (!previous.empty()) return context.publish(output_color, previous.front());
    const auto color = clearColor(context);
    if (!color) return cxx::unexpected(color.error());
    return context.publish(output_color, {{output_color, *color}});
}

template <std::uint32_t Value>
static void updateRouted(
    ProbeEffectState& state, const ProbeEffectCommand::Request& request, ProbeEffectCommand::Response& response
) noexcept
{
    if (request.value && *request.value > 100)
    {
        response.error = {kInvalidConfig};
        return;
    }
    if (request.value)
    {
        if (state.revision == UINT64_MAX)
        {
            response.error = {kRuntimeCapacity};
            return;
        }
        state.value = *request.value;
        ++state.revision;
    }
    response = {{}, state.value, state.revision};
}

template <std::uint32_t Value>
static RenderResult<void> initializeRouted(EffectInitializationContext& context, std::string_view arguments) noexcept
{
    if (!arguments.empty()) return cxx::unexpected(RenderError{kInvalidConfig});
    return context.initializeState<ProbeEffectState>(ProbeEffectState{Value});
}

static EffectImplementation routedImplementation(
    std::string_view name, EffectImplementation::Initialize initialize
)
{
    return {
        .capability = graph::nodeTypeId(routed_capability),
        .identity = {std::string{name}, 1},
        .supported_outputs = {output_color},
        .initialize = initialize,
        .build = buildRouted
    };
}

template <bool Alternate> struct ProbeResource final
{
    using Config = void;

    static inline const ResourceDefinition descriptor{
        ResourceNameView{Alternate ? "example.resource.buffer.b" : "example.resource.buffer.a"},
        0, 0, {}, "An independently owned GPU buffer.", EResourceDomain::DEVICE
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

    static std::span<const CommandBinding> commandBindings() noexcept
    {
        static const std::array commands{
            CommandBinding::declare<ProbeUpload, &upload>(), CommandBinding::declare<ProbeSize, &size>()
        };
        return commands;
    }

    static ResourceRegistration factory() noexcept
    {
        const detail::StorageBindingOps binding{
            .describeGraphImport = [](const void*, std::uint32_t projection) noexcept
                -> RenderResult<ResourceProjection>
            {
                if (projection != 0) return cxx::unexpected(RenderError{kStorageContract});
                return ResourceProjection{
                    GraphBufferDesc{4096}, {}, EGraphUse::TRANSFER_DESTINATION
                };
            },
            .resolveGraphImport = [](
                void* object, std::uint32_t, const VGraphResourceDesc&, std::span<detail::ResourceInput> outputs
            ) noexcept -> RenderResult<void>
            {
                auto& storage = *static_cast<ProbeStorage*>(object);
                for (auto& output : outputs) output = {storage.buffer, 0, 4096};
                return {};
            }
        };
        return makeResourceRegistration<ProbeResource<false>, ProbeStorage, &create>(&binding);
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

    static inline const ResourceDefinition descriptor{
        ResourceNameView{Domain == EResourceDomain::SCENE ? "example.resource.scene" : "example.resource.view"},
        0, 0, {}, "Resource domain probe.", Domain
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

    static std::span<const CommandBinding> commandBindings() noexcept
    {
        static const std::array commands{CommandBinding::declare<ProbeReference, &reference>()};
        return commands;
    }

    template <EResourceDomain Domain> static ResourceRegistration factory() noexcept
    {
        return makeResourceRegistration<DomainResource<Domain>, DomainStorage, &create>();
    }
};

struct ProbeReadback final
{
    struct Descriptor final
    {
        using Config = void;

        static inline const ResourceDefinition descriptor{
            ResourceNameView{"example.resource.readback"}, 0, 0, {}, "An offscreen transfer result.",
            EResourceDomain::VIEW
        };
    };

    static inline std::array<std::uint32_t, readback_frames> observed{};
    detail::DeviceSession& device;
    std::array<nri::Buffer*, readback_frames> buffers{};

    explicit ProbeReadback(ResourceCreateContext& context) noexcept : device(context.device) {}

    ~ProbeReadback() noexcept
    {
        if (!buffers.front()) return;
        device.deferRelease([session = &device, buffers = buffers]() noexcept
        {
            const auto& core = session->nriCore();
            for (std::size_t slot = 0; slot < buffers.size(); ++slot)
            {
                auto* buffer = buffers[slot];
                const auto* mapped = core.MapBuffer(*buffer, 0, sizeof(observed[slot]));
                CHECK(mapped);
                CHECK(session->invalidateMappedRange(*buffer, 0, sizeof(observed[slot])) == nri::Result::SUCCESS);
                std::memcpy(&observed[slot], mapped, sizeof(observed[slot]));
                core.UnmapBuffer(*buffer);
                core.DestroyBuffer(buffer);
            }
        });
    }

    static RenderResult<std::unique_ptr<ProbeReadback>>
    create(ResourceCreateContext& context, ResourceInstanceId) noexcept
    {
        auto storage = std::make_unique<ProbeReadback>(context);
        const auto& session = storage->device;
        if (context.frame_count != readback_frames) return cxx::unexpected(RenderError{kStorageContract});
        for (auto*& buffer : storage->buffers)
        {
            const auto result = session.nriCore().CreatePlacedBuffer(
                session.device(), NriHostReadbackHeap, {transfer_bytes, 0, nri::BufferUsageBits::NONE}, buffer
            );
            if (result == nri::Result::SUCCESS) continue;
            for (auto* allocated : storage->buffers)
                if (allocated) session.nriCore().DestroyBuffer(allocated);
            storage->buffers = {};
            return cxx::unexpected(RenderError{kDeviceFailure});
        }
        return storage;
    }

    static ResourceRegistration factory() noexcept
    {
        const detail::StorageBindingOps binding{
            .describeGraphImport = [](const void*, std::uint32_t projection) noexcept
                -> RenderResult<ResourceProjection>
            {
                if (projection != 0) return cxx::unexpected(RenderError{kStorageContract});
                return ResourceProjection::bufferOutput(transfer_bytes, EGraphUse::TRANSFER_DESTINATION);
            },
            .resolveGraphImport = [](
                void* object, std::uint32_t, const VGraphResourceDesc&, std::span<detail::ResourceInput> outputs
            ) noexcept -> RenderResult<void>
            {
                auto& storage = *static_cast<ProbeReadback*>(object);
                if (outputs.size() != storage.buffers.size()) return cxx::unexpected(RenderError{kStorageContract});
                for (std::size_t slot = 0; slot < outputs.size(); ++slot)
                    outputs[slot] = {storage.buffers[slot], 0, transfer_bytes};
                return {};
            }
        };
        return makeResourceRegistration<Descriptor, ProbeReadback, &create>(&binding);
    }
};

inline constexpr EffectPortDefinition kTransferCapabilityPorts[]{
            {output_color, "color", {color_type, 1}, graph::EPinDirection::OUTPUT},
            {readback_value, "value", {"example.graph.readback", 1}, graph::EPinDirection::OUTPUT}
        };

inline constexpr EffectCapability kTransferCapability{
    .id = graph::nodeTypeId(transfer_capability), .canonical_name = transfer_capability, .ports = kTransferCapabilityPorts
};

static RenderResult<void> buildTransfer(EffectCompileContext& context, std::string_view) noexcept
{
    if (context.outputDemanded(output_color))
    {
        const auto color = clearColor(context);
        if (!color) return cxx::unexpected(color.error());
        if (auto published = context.publish(output_color, {{output_color, *color}}); !published) return published;
    }
    if (!context.outputDemanded(readback_value)) return {};

    const auto readback = context.importFinalBuffer(
        readback_binding, ProbeReadback::Descriptor::descriptor.type,
        {"", GraphBufferDesc{transfer_bytes}, EGraphOrigin::IMPORTED, {}, EGraphUse::TRANSFER_DESTINATION}
    );
    if (!readback) return cxx::unexpected(readback.error());
    const GraphBufferDesc description{transfer_bytes};
    const auto history = take(
        context.declareHistory("samples", {description, {}, EGraphUse::TRANSFER_SOURCE, history_distance})
    );
    const auto fallback = take(context.declareResource({"history fallback", description}));
    // Reject after the first arm has changed both graph resources and node-side declarations.
    const auto rejected = context.choose(
        {EConditionSource::HISTORY_VALID, history.previous.index},
        [&](EffectCompileContext& arm) noexcept -> RenderResult<ResourceVersion>
        {
            const auto unused = arm.declarePreparedBuffer("discarded", 4, 8);
            if (!unused) return cxx::unexpected(unused.error());
            const auto published = arm.publish(readback_value, {{readback_value, *unused}});
            if (!published) return cxx::unexpected(published.error());
            return *unused;
        },
        [](EffectCompileContext&) noexcept -> RenderResult<ResourceVersion>
        {
            return cxx::unexpected(RenderError{kGraphInvalidRange});
        }
    );
    CHECK(!rejected && rejected.error().type == kGraphInvalidRange);
    const auto previous = take(context.choose(
        {EConditionSource::HISTORY_VALID, history.previous.index},
        [&](EffectCompileContext&) noexcept -> RenderResult<ResourceVersion> { return ResourceVersion{history.previous, 0}; },
        [&](EffectCompileContext& arm) noexcept -> RenderResult<ResourceVersion>
        {
            auto seed = arm.addTransferPass("initialize history fallback", TransferWork{{
                FillBuffer{{{fallback, 0}}, fallback_value}
            }});
            if (!seed) return lux::cxx::unexpected(seed.error());
            return seed->written.front();
        }
    ));
    const auto written = take(context.addTransferPass("write current history", TransferWork{{
        FillBuffer{{{history.current, 0}}, expected_value}
    }}));
    CHECK(context.publishHistory(written.written.front()));
    const auto copied = take(context.addTransferPass("read earlier history", TransferWork{{
        CopyBuffer{{previous}, {*readback}}
    }}));
    return context.publish(readback_value, {{readback_value, copied.written.front()}});
}

static EffectImplementation transferImplementation()
{
    return {
        .capability = graph::nodeTypeId(transfer_capability),
        .identity = {std::string{transfer_capability}, 1},
        .supported_outputs = {output_color, readback_value},
        .initialize = [](EffectInitializationContext&, std::string_view arguments) noexcept -> RenderResult<void>
        {
            if (!arguments.empty()) return cxx::unexpected(RenderError{kInvalidConfig});
            return {};
        },
        .build = buildTransfer
    };
}

struct RejectedStorage final
{
    struct Descriptor final
    {
        using Config = void;

        static inline const ResourceDefinition
            descriptor{ResourceNameView{"example.storage.reject"}, 0, 0, {}, "A recoverable factory failure."};
    };

    static RenderResult<std::unique_ptr<RejectedStorage>> create(ResourceCreateContext&, ResourceInstanceId) noexcept
    {
        return cxx::unexpected(RenderError{kDeviceCapacity});
    }

    static ResourceRegistration factory() noexcept
    {
        return makeResourceRegistration<Descriptor, RejectedStorage, &create>();
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

static void checkResourceDomains(ipc::IpcRequester& client, DeviceSessionId session, ResourceInstanceId global)
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
    const auto make_view = [&](SceneId scene) -> ViewScope
    {
        const auto reply = call<CreateView>(client, ids[1], {scene});
        CHECK(reply.error.type == 0);
        return {scene, reply.view};
    };
    const auto ensure = [&](const VResourceScope& scope, ResourceNameView name)
    {
        const auto reply = call<GetOrCreateResource>(client, ids[4], {session, scope, name});
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
    CHECK(call<QueryResource>(client, ids[5], {session, first, view_name}).error.type == kStorageMismatch);
    CHECK(reference(first_resource, global) == 0);
    CHECK(reference(first_resource, first_scene_resource) == 0);
    CHECK(reference(first_resource, second_scene_resource) == kStorageMismatch);
    CHECK(reference(first_resource, second_resource) == kStorageMismatch);
    CHECK(reference(first_scene_resource, first_resource) == kStorageMismatch);

    CHECK(call<RemoveScene>(client, ids[2], {first}).error.type == 0);
    CHECK((DomainStorage::destroyed == std::vector{first_resource, first_scene_resource}));
    CHECK(reference(first_resource, global) == kStorageIdentity);
    CHECK(call<QueryResource>(client, ids[5], {session, first_view, global_name}).error.type == kRuntimeIdentity);
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
    config.frames_in_flight = readback_frames;
    auto server = take(RenderServer::create(config, *execution));
    {
        auto registration = ProbeStorageType<false>::factory();
        ResourceDefinition descriptor = *registration.descriptor;
        std::string resource_name{descriptor.type.name()};
        std::string description{descriptor.description};
        descriptor.type = ResourceNameView{resource_name};
        descriptor.description = description;

        const auto methods = ProbeStorage::commandBindings();
        std::vector<CommandBinding> bindings(methods.begin(), methods.end());
        CHECK(!bindings.empty());
        registration.descriptor = &descriptor;
        take(server->registerResource(std::move(registration), {}, bindings));
    }
    take(server->registerResource<ProbeStorageType<true>>(ProbeStorage::commandBindings()));
    take(server->registerResource(DomainStorage::factory<EResourceDomain::SCENE>(), {}, DomainStorage::commandBindings()));
    take(server->registerResource(DomainStorage::factory<EResourceDomain::VIEW>(), {}, DomainStorage::commandBindings()));
    take(server->registerResource(ProbeReadback::factory()));
    take(server->registerResource<RejectedStorage>());
    const auto duplicate_resource = server->registerResource<ProbeStorageType<false>>(ProbeStorage::commandBindings());
    CHECK(!duplicate_resource && duplicate_resource.error().type == kStorageMismatch);

    const auto setter_a = take(server->registerNodeEffectCommand(
        CommandBinding::declareForEffect<ProbeEffectCommand, ProbeEffectState, &updateRouted<17>>()
    ));
    const auto setter_b = take(server->registerNodeEffectCommand(
        CommandBinding::declareForEffect<ProbeEffectCommand, ProbeEffectState, &updateRouted<29>>()
    ));
    CHECK(setter_a.command == setter_b.command && setter_a.binding != setter_b.binding);
    const auto routed = kRoutedCapability;
    CHECK(server->registerEffectCapability(routed, {setter_a.command}));
    CHECK(server->registerEffectImplementation(routedImplementation(route_b, initializeRouted<29>), {setter_b}));
    CHECK(server->registerEffectImplementation(routedImplementation(route_a, initializeRouted<17>), {setter_a}));
    CHECK(server->registerEffectImplementation(routedImplementation(
        route_failure,
        [](EffectInitializationContext&, std::string_view) noexcept -> RenderResult<void>
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
    ), {setter_a}));
    const auto transfer = kTransferCapability;
    CHECK(server->registerEffectCapability(transfer));
    CHECK(server->registerEffectImplementation(transferImplementation()));

    const auto duplicate_node = server->registerEffectCapability(routed, {setter_a.command});
    CHECK(!duplicate_node && duplicate_node.error().type == kCapabilityConflict);
    auto changed = routedImplementation(route_a, initializeRouted<17>);
    changed.display_name = "Conflicting implementation metadata";
    const auto duplicate_implementation = server->registerEffectImplementation(std::move(changed), {setter_a});
    CHECK(!duplicate_implementation && duplicate_implementation.error().type == kCapabilityConflict);
    auto unknown_output = transferImplementation();
    unknown_output.identity.canonical_name = "example.graph.unknown_output";
    unknown_output.supported_outputs.push_back(effectPortSemantic("missing"));
    const auto unsupported_port = server->registerEffectImplementation(std::move(unknown_output));
    CHECK(!unsupported_port && unsupported_port.error().type == kCapabilityDeclaration);
    auto missing_command = routedImplementation("example.effect.missing_command", initializeRouted<17>);
    const auto incomplete = server->registerEffectImplementation(std::move(missing_command));
    CHECK(!incomplete && incomplete.error().type == kCapabilityDeclaration);

    const auto handler = take(server->registerHandler<ProbeHandler, ProbeHandler::Bindings>());
    const auto duplicate_handler = server->registerHandler<ProbeHandler, ProbeHandler::Bindings>();
    CHECK(!duplicate_handler && duplicate_handler.error().type == kCommandContract);
    CHECK(take(server->handlerId(cxx::make_basic_type_info<ProbeHandler>())) == handler);
    const auto other_handler = server->registerHandler<ProbeHandler, ProbeHandler::Bindings>("other");
    CHECK(!other_handler);

    auto client = take(ipc::IpcCore::instance().createRequester(config.channel_name));
    RenderResult<void> ran;
    std::thread thread([&]() noexcept { ran = server->run(); });
    const auto command_id = [&](std::string_view command)
    {
        const auto found = call<QueryCommandIds>(client, QueryCommandIds::id, std::array{CommandName{command}});
        CHECK(!found.error.type && found.ids.size() == 1);
        return found.ids.front();
    };
    const auto identity = call<ProbeIdentity>(client, command_id(ProbeIdentity::name), 42);
    CHECK(!identity.error.type && identity.value == 42);
    const auto create_effect = command_id(CreateEffect::name);
    const auto remove_effect = command_id(RemoveEffect::name);
    const auto route_id = command_id(ProbeEffectCommand::name);
    CHECK(route_id == setter_a.command);

    struct RoutedEffect final
    {
        EffectCreated reply;
        graph::NodeId first;
        graph::NodeId last;
    };
    const auto make_routed = [&](std::string_view primary, std::string_view secondary = {}, std::string args = {})
    {
        CreateEffectRequest request;
        request.graph = std::make_unique<EffectGraph>();
        EffectGraphEdit edit{*request.graph};
        const auto first = take(edit.addNode(routed, {std::string{primary}, 1}, std::move(args)));
        auto last = first;
        if (!secondary.empty())
        {
            last = take(edit.addNode(routed, {std::string{secondary}, 1}));
            CHECK(edit.connect(edit.pinId(first, output_color), edit.pinId(last, previous_color)));
        }
        CHECK(edit.setOutput("color", edit.pinId(last, output_color), true));
        CHECK(edit.commit());
        return RoutedEffect{call<CreateEffect>(client, create_effect, std::move(request)), first, last};
    };
    const auto pair = make_routed(route_a, route_b);
    const auto peer = make_routed(route_a);
    CHECK(!pair.reply.error.type && !peer.reply.error.type && pair.first != pair.last);
    const auto failed_state = make_routed(route_a, route_failure);
    CHECK(failed_state.reply.error.type == kInvalidConfig && failed_state.reply.effect.isNull());
    CHECK(make_routed(route_a, {}, "invalid").reply.error.type == kInvalidConfig);
    CHECK(make_routed(transfer_capability).reply.error.type == kCapabilityDeclaration);
    const auto read = [&](const RoutedEffect& effect, graph::NodeId node)
    {
        return call<ProbeEffectCommand>(client, route_id, {effect.reply.effect, node});
    };
    CHECK(read(pair, pair.first).value == 17 && read(pair, pair.last).value == 29);
    CHECK(read(pair, {}).error.type == kCommandContract);
    const auto rejected = call<ProbeEffectCommand>(client, route_id, {pair.reply.effect, pair.first, 101});
    CHECK(rejected.error.type == kInvalidConfig && read(pair, pair.first).revision == 1);
    const auto updated = call<ProbeEffectCommand>(client, route_id, {pair.reply.effect, pair.first, 42});
    CHECK(!updated.error.type && updated.value == 42 && updated.revision == 2);
    const auto repeated = call<ProbeEffectCommand>(client, route_id, {pair.reply.effect, pair.first, 42});
    CHECK(!repeated.error.type && repeated.revision == 3);
    CHECK(read(pair, pair.last).value == 29 && read(pair, pair.last).revision == 1);
    CHECK(read(peer, peer.first).value == 17 && read(peer, peer.first).revision == 1);
    CHECK(!call<RemoveEffect>(client, remove_effect, {pair.reply.effect}).error.type);
    const auto replacement = make_routed(route_b);
    CHECK(!replacement.reply.error.type && replacement.reply.effect != pair.reply.effect);
    CHECK(read(pair, pair.first).error.type == kRuntimeIdentity);
    CHECK(read(replacement, replacement.first).value == 29);
    CHECK(ProbeStorage::allocations == 0);
    std::puts("EFFECT ROUTING PASS: no-device node state, shared command identity, rollback, and stale EffectId");

    CreateEffectRequest request;
    request.graph = std::make_unique<EffectGraph>();
    EffectGraphEdit edit{*request.graph};
    const auto node = take(edit.addNode(transfer, {std::string{transfer_capability}, 1}));
    CHECK(edit.setOutput("color", edit.pinId(node, output_color), true));
    CHECK(edit.setOutput("value", edit.pinId(node, readback_value)));
    CHECK(edit.commit());
    const auto effect = call<CreateEffect>(client, create_effect, std::move(request));
    CHECK(!effect.error.type && ProbeStorage::allocations == 0);

    const auto opened = call<CreateDeviceSession>(client, command_id(CreateDeviceSession::name), device);
    CHECK(!opened.error.type && !opened.device.isNull());
    const auto session = opened.device;
    const auto get_resource = command_id(GetOrCreateResource::name);
    const auto upload = command_id(ProbeUpload::name);
    const auto size = command_id(ProbeSize::name);
    const auto refused = call<GetOrCreateResource>(client, get_resource, {
        session, DeviceScope{}, RejectedStorage::Descriptor::descriptor.type
    });
    CHECK(refused.error.type == kDeviceCapacity && refused.resource_id.isNull());
    std::array<std::byte, 1024> bytes{};
    std::vector<ResourceInstanceId> resources;
    for (auto name : {ProbeResource<false>::descriptor.type, ProbeResource<true>::descriptor.type})
    {
        const auto created = call<GetOrCreateResource>(client, get_resource, {session, DeviceScope{}, name});
        CHECK(!created.error.type);
        resources.push_back(created.resource_id);
        const auto written = call<ProbeUpload>(client, upload, {created.resource_id, bytes});
        CHECK(!written.error.type && written.bytes == bytes.size());
        const auto observed = call<ProbeSize>(client, size, {created.resource_id});
        CHECK(!observed.error.type && observed.bytes == bytes.size());
    }
    CHECK(resources[0] != resources[1] && ProbeStorage::allocations == 2);
    const auto scene = call<CreateScene>(client, command_id(CreateScene::name), {});
    const auto view = call<CreateView>(client, command_id(CreateView::name), {scene.scene});
    CHECK(!scene.error.type && !view.error.type);
    const auto readback = call<GetOrCreateResource>(client, get_resource, {
        session, ViewScope{scene.scene, view.view}, ProbeReadback::Descriptor::descriptor.type
    });
    CHECK(!readback.error.type);
    const auto create_target = command_id(CreateTarget::name);
    const auto target = call<CreateTarget>(
        client, create_target, {session, {16, 16, rdesc::ETextureFormat::RGBA8_UNORM}}
    );
    const auto alternate = call<CreateTarget>(
        client, create_target, {session, {16, 16, rdesc::ETextureFormat::RGBA16_SFLOAT}}
    );
    const auto job = call<CreateRenderJob>(
        client, command_id(CreateRenderJob::name), {scene.scene, view.view, effect.effect, EJobCadence::REQUESTED}
    );
    CHECK(!target.error.type && !alternate.error.type && !job.error.type);
    const auto start = command_id(StartRenderJob::name);
    CHECK(call<StartRenderJob>(client, start, {session, job.job, target.target}).error.type == kJobState);
    CHECK(!call<PrepareEffect>(client, command_id(PrepareEffect::name), {effect.effect, session}).error.type);
    CHECK(call<StartRenderJob>(client, start, {session, job.job, target.target}).error.type == kStorageIdentity);
    JobBinding binding;
    binding.resources = {{std::string{readback_binding}, resources.front()}};
    const auto wrong_owner = call<StartRenderJob>(client, start, {session, job.job, target.target, binding});
    CHECK(wrong_owner.error.type == kStorageMismatch);
    binding.resources.front().resource = readback.resource_id;
    CHECK(call<StartRenderJob>(client, start, {session, job.job, alternate.target, binding}).error.type ==
        kDeviceArgument);
    CHECK(!call<StartRenderJob>(client, start, {session, job.job, target.target, binding}).error.type);
    const auto render = command_id(RequestRenderJob::name);
    std::array<std::uint32_t, readback_frames> slots;
    for (std::size_t frame = 0; frame < slots.size(); ++frame)
    {
        const auto submitted = call<RequestRenderJob>(client, render, {job.job});
        CHECK(!submitted.error.type && submitted.job == job.job);
        CHECK(submitted.frame_index == frame && submitted.slot_index < slots.size());
        const auto selected = std::span{slots}.first(frame);
        CHECK(std::ranges::find(selected, submitted.slot_index) == selected.end());
        slots[frame] = submitted.slot_index;
    }

    const auto retained = call<ProbeSize>(client, size, {resources.front()});
    CHECK(!retained.error.type && retained.bytes == bytes.size());
    checkResourceDomains(client, session, resources.front());
    client.reset();
    server->stop();
    thread.join();
    CHECK(ran && server->stats().validation_errors == 0 && ProbeStorage::submitted_copies > 0);
    server.reset();
    for (std::size_t frame = 0; frame < slots.size(); ++frame)
    {
        const auto expected = frame < history_distance ? fallback_value : expected_value;
        CHECK(ProbeReadback::observed[slots[frame]] == expected);
    }
    CHECK(ProbeStorage::allocations == ProbeStorage::releases);
    CHECK(DomainStorage::destroyed.size() == 5);
    execution.reset();
    std::puts("SDK PASS: EffectGraph qualification, distance-two History, Resource upload, scopes and cleanup");
}
