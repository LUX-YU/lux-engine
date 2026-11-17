#include "SceneCapture.hpp"
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/runtime/RenderClient.hpp>
#include <lux/engine/render/commands/UiCommands.hpp>
#include <lux/engine/render/capabilities/UiScene.hpp>
#include <optional>

#undef CHECK
#define CHECK(...) ::foundation_test::require(static_cast<bool>((__VA_ARGS__)), __LINE__)

namespace
{
    struct Delta final { int value = 0; };
    struct Stamp final { int value = 0; };
    struct Read final
    {
        static constexpr std::string_view name = "test.frame.read";
        static constexpr bool requires_frame_boundary = false;
        using Data = int;
        struct Request final { ResourceInstanceId resource; };
        struct Response final { RenderError error{}; int sum = 0, stamp = 0, calls = 0; };
        static ResourceInstanceId resourceId(const Request& r) noexcept { return r.resource; }
    };
    template <int I> struct Inputs final
    {
        using Config = void;
        using Ops = void;
        using Commands = std::tuple<Read>;
        using PerFrameData = std::tuple<Delta, Stamp>;
        static inline const ResourceDefinition descriptor{
            ResourceNameView{I == 0 ? "test.frame.a" : "test.frame.b"}, 0, 0, {}, "column probe",
            EResourceDomain::SCENE, resourceCommandDescriptors(static_cast<Commands*>(nullptr)),
            resourceFrameTypes(static_cast<PerFrameData*>(nullptr))
        };
    };
    struct FrameInputCapability final
    {
        using Resources = std::tuple<Inputs<0>, Inputs<1>, InstanceResource, LightResource>;
        using Commands = std::tuple<>;
        using PrerequisiteCapabilities = std::tuple<>;
    };

    struct Counter final
    {
        int sum = 0, stamp = 0, calls = 0;
        static inline std::atomic<bool> gate = false, entered = false;
        static RenderResult<std::unique_ptr<Counter>> create(ResourceCreateContext&, ResourceInstanceId) noexcept
        { return std::make_unique<Counter>(); }
        static RenderResult<void>
        apply(Counter& value, std::span<const Delta> deltas, std::span<const Stamp> stamps) noexcept
        {
            if (std::ranges::any_of(deltas, [](auto d) { return d.value < 0; }))
                return lux::cxx::unexpected(RenderError{kStorageContract});
            if (gate.load()) { entered.store(true); gate.wait(true); }
            ++value.calls;
            for (auto d : deltas) value.sum += d.value;
            for (auto s : stamps) value.stamp = s.value;
            return {};
        }
        static void read(Counter& value, const Read::Request&, Read::Response& response) noexcept
        { response = {{}, value.sum, value.stamp, value.calls}; }
        template <int I> static ResourceRegistration factory() noexcept
        {
            static const std::array commands{CommandBinding::declare<Read, &read>()};
            return makeResourceRegistration<Inputs<I>, Counter, &create>(nullptr, nullptr, commands,
                FrameDataCall::declare<Inputs<I>, Counter, &apply>());
        }
    };
    struct Configuration final { int unused = 0; bool operator==(const Configuration&) const noexcept = default; };
    struct ClearFeature final
    {
        using Config = Configuration;
        static constexpr FeatureName name{"test.frame.clear"};
        using Ports = ClearFeature;

        static RenderResult<void> describe(const void*, std::string_view, FeaturePorts& ports) noexcept
        {
            GraphImageDesc image{32, 32, 1, 1, rdesc::ETextureFormat::RGBA8_UNORM};
            image.extent.source = EImageExtentSource::TARGET;
            ports.resources.push_back({"color", EFeaturePort::OUTPUT, image});
            return {};
        }

        static RenderResult<void> build(const void*, std::string_view, FeatureBuildContext& context) noexcept
        {
            auto pass = context.graph.addTransferPass("clear", TransferWork{
                {ClearImage{{context.resource("color")}, {1, 0, 0, 1}}}
            });
            if (!pass) return lux::cxx::unexpected(pass.error());
            return context.publish("color", pass->written.front());
        }
    };
    template <class Response> struct Reply final
    {
        using receiver_concept = stdexec::receiver_t;
        std::optional<Response>* result;
        stdexec::inplace_stop_token stop;
        auto get_env() const noexcept { return stdexec::env{stdexec::prop{stdexec::get_stop_token, stop}}; }
        void set_value(Response value) && noexcept { result->emplace(std::move(value)); }
    };
    template <class Predicate> void until(Predicate ready)
    {
        const auto deadline = Clock::now() + 20s;
        while (!ready()) { CHECK(Clock::now() < deadline); std::this_thread::yield(); }
    }
    template <class C> typename C::Response request(
        RenderClient& client, CommandId id, typename C::Request input, bool success = true
    )
    {
        std::optional<typename C::Response> response;
        stdexec::inplace_stop_source stop;
        auto op = stdexec::connect(
            client.request<C>(id, input), Reply<typename C::Response>{&response, stop.get_token()}
        );
        stdexec::start(op);
        until([&] { take(client.dispatchResponses(64)); return response.has_value(); });
        if (success) CHECK(response->error.type == 0);
        return std::move(*response);
    }
}

int main(int argc, char** argv)
{
    auto execution_result = lux::execution::ExecutionRuntime::create({2, 128, {64}});
    CHECK(execution_result);
    auto execution = std::move(*execution_result);
    RenderServerConfig config; config.channel_name = "f2.frame.packet"; config.uploads.emplace();
    DeviceOptions options; options.texture_descriptors = true; options.multiple_queues = true;
    if (argc > 1 && std::string_view(argv[1]) == "d3d12") options.backend = EGraphicsBackend::D3D12;
    auto server = take(RenderServer::create(config, *execution, options));
    take(server->registerResource(Counter::factory<0>()));
    take(server->registerResource(Counter::factory<1>()));
    const auto feature = take(server->registerFeature<ClearFeature>());
    const auto implementation = registerRenderProbe<Configuration, FrameInputCapability>(*server, "test.frame.columns", {feature},
        probePorts({"color"}, {}),
        [](const CapabilityBuildInput& input) noexcept -> RenderResult<CapabilityGraphContribution>
        {
            CapabilityGraphContribution graph;
            graph.definition.features.push_back({
                input.features.front(), "clear", take(ConfigValue::make(Configuration{}))
            });
            graph.definition.outputs.push_back({{0, "color"}, GraphOutputRequirements{input.output.format, 1}});
            publishProbeOutputs(graph, input);
            return graph;
        });
    auto wire = ipc::IpcCore::instance().createRequester(config.channel_name); CHECK(wire);
    auto frame_wire = ipc::IpcCore::instance().createRequester(config.channel_name); CHECK(frame_wire);
    CHECK(server->setFrameSource(frame_wire->id()));
    std::thread render([&] { CHECK(server->run()); });
    auto client = take(RenderClient::create(std::move(*frame_wire)));
    const auto commands = queryCommands(*wire);
    const auto command = [&](std::string_view name)
    {
        return std::ranges::find(commands.commands, name, &CommandInfo::name)->id;
    };
    const auto submit = command(SubmitFrame::name);
    const auto scene = call<CreateScene>(*wire, commands, {}).scene;
    const auto view = call<CreateView>(*wire, commands, {scene}).view;
    const auto a = call<GetOrCreateResource>(*wire, commands, {scene, Inputs<0>::descriptor.type}).resource_id;
    const auto b = call<GetOrCreateResource>(*wire, commands, {scene, Inputs<1>::descriptor.type}).resource_id;
    const TargetDesc target{32, 32, rdesc::ETextureFormat::RGBA8_UNORM, 1, ETargetKind::OFFSCREEN, true};
    const auto effect = call<CreateEffect>(
        *wire, commands, probeRequest<Configuration, FrameInputCapability>(target, implementation, Configuration{}, {"color"})
    ).effect;
    const auto output = call<CreateTarget>(*wire, commands, {target}).target;
    auto attachment = call<Attach>(*wire, commands, {scene, view, output, effect}).attachment;
    const auto schema = call<QueryFrameInputLayout>(*wire, commands, attachment).layout;
    using Resources = std::tuple<Inputs<0>, Inputs<1>, InstanceResource, LightResource>;
    std::array<FrameData, 2> packets{
        take(FrameData::create<Resources>(schema)), take(FrameData::create<Resources>(schema))
    };
    const auto light_resource = call<GetOrCreateResource>(*wire, commands, {scene, kLightResourceName}).resource_id;
    const std::array<VDirectLight, 1> initial_light{DirectionalLightGpu{}};
    const auto light = call<LightingCommands::CreateLights>(*wire, commands,
        {light_resource, {0, initial_light, {}}}).lights.front();
    auto& packet = packets[0];
    DirectionalLightGpu brighter; brighter.radiance = {4, 3, 2, 0};
    CHECK(packet.addData<LightResource>(DirectLightUpdate{light, brighter, {}}));
    auto deltas = take(packet.appendData<Inputs<0>, Delta>(3));
    for (auto& d : deltas) d.value = 2;
    CHECK(packet.addData<Inputs<0>>(Stamp{9}));
    CHECK(packet.addData<Inputs<1>>(Delta{50}));
    CHECK(packet.seal());
    CHECK(request<SubmitFrame>(*client, submit, {attachment, &packet}).outcome == EFrameOutcome::SUBMITTED);
    CHECK(packet.writable());
    auto first = call<Read>(*wire, commands, {a}), second = call<Read>(*wire, commands, {b});
    CHECK(first.sum == 6 && first.stamp == 9 && first.calls == 1);
    CHECK(second.sum == 50 && second.calls == 1); // Shared Storage and element types remain distinct declarations.
    packet.reset(); CHECK(packet.seal());
    request<SubmitFrame>(*client, submit, {attachment, &packet});
    CHECK(call<Read>(*wire, commands, {a}).calls == 1); // Empty columns are no update.
    packet.reset(); CHECK(packet.addData<Inputs<0>>(Delta{1})); CHECK(packet.seal());
    Counter::gate = true; Counter::entered = false;
    std::optional<FrameCompletion> terminal;
    stdexec::inplace_stop_source stop;
    RenderFrame input{attachment, &packet};
    auto operation = stdexec::connect(
        client->request<SubmitFrame>(input), Reply<FrameCompletion>{&terminal, stop.get_token()}
    );
    stdexec::start(operation);
    until([] { return Counter::entered.load(); });
    CHECK(!packet.appendData<Inputs<0>, Delta>(1));
    CHECK(packets[1].appendData<Inputs<0>, Delta>(4097));
    CHECK(request<SubmitFrame>(*client, submit, input, false).error.type == kFrameContract);
    CHECK(!packet.writable() && !terminal);
    stop.request_stop();
    CHECK(!packet.reserve<Inputs<0>, Delta>(8192));
    Counter::gate = false; Counter::gate.notify_all();
    until([&] { take(client->dispatchResponses(64)); return terminal.has_value(); });
    CHECK(packet.writable() && terminal->error.type == 0);
    auto wrong_layout = take(FrameLayout::create(schema.channels(), schema.revision()));
    CHECK(wrong_layout == schema && !wrong_layout.hasSameIdentity(schema));
    auto wrong_packet = take(FrameData::create<Resources>(wrong_layout)); CHECK(wrong_packet.seal());
    CHECK(request<SubmitFrame>(*client, submit, {attachment, &wrong_packet}, false).error.type == kFrameContract);
    CHECK(wrong_packet.writable() && call<Read>(*wire, commands, {a}).sum == 7);
    packet.reset(); CHECK(packet.addData<InstanceResource>(TransformUpdate{})); CHECK(packet.seal());
    CHECK(request<SubmitFrame>(*client, submit, {attachment, &packet}, false).error.type == kStorageIdentity);
    CHECK(packet.writable());
    packet.reset(); CHECK(packet.seal());
    request<SubmitFrame>(*client, submit, {attachment, &packet, true});
    CameraInput camera; camera.exposure = 2;
    call<UpdateViewCamera>(*wire, commands, {{scene, view}, camera});
    camera.near_distance = -1;
    CHECK(call<UpdateViewCamera>(*wire, commands, {{scene, view}, camera}, false).error.type == kFrameContract);

    const auto choices = call<ListCapabilities>(*wire, commands, {});
    const auto ui = std::ranges::find_if(choices.implementations, [](const auto& value)
        { return value.capability.type == lux::cxx::make_basic_type_info<ImguiCapability>(); });
    CHECK(ui != choices.implementations.end());
    CreateEffectRequest ui_request{{target.format}};
    ui_request.composition.contributions.push_back({"ui", ui->capability.type, ui->implementation_id,
        take(ConfigValue::make(UiConfiguration{}))});
    ui_request.composition.outputs.push_back({"ui", "color"});
    const auto ui_effect = call<CreateEffect>(*wire, commands, std::move(ui_request)).effect;
    const auto ui_target = call<CreateTarget>(*wire, commands, {{32, 32, target.format}}).target;
    const auto ui_attachment = call<Attach>(*wire, commands, {scene, view, ui_target, ui_effect}).attachment;
    std::array<ImDrawVert, 3> vertices{};
    vertices[0].pos = {0, 0}; vertices[1].pos = {32, 0}; vertices[2].pos = {0, 32};
    for (auto& vertex : vertices) { vertex.uv = {.5F, .5F}; vertex.col = 0xffffffff; }
    const std::array<ImDrawIdx, 3> indices{0, 1, 2};
    ImDrawCmd draw{}; draw.ElemCount = 3; draw.TextureId = 9; draw.ClipRect = {0, 0, 32, 32};
    const ImguiDrawList list{vertices, indices, {&draw, 1}};
    const ImguiImage image{9, {}, output};
    const UiDrawInput ui_input{ui_attachment, 0, {{&list, 1}, {0, 0}, {32, 32}, {1, 1}, {&image, 1}}};
    packet.reset(); CHECK(packet.seal());
    CHECK(request<SubmitUiFrame>(*client, command(SubmitUiFrame::name), {{attachment, &packet}, {&ui_input, 1}}).outcome
        == EFrameOutcome::SUBMITTED);
    CHECK(packet.writable());
    detach(*wire, commands, ui_attachment);
    call<RemoveTarget>(*wire, commands, ui_target);
    call<RemoveEffect>(*wire, commands, {ui_effect});
    const auto second_output = call<CreateTarget>(*wire, commands, {target}).target;
    const auto second_attachment = call<Attach>(*wire, commands, {scene, view, second_output, effect}).attachment;
    const auto second_schema = call<QueryFrameInputLayout>(*wire, commands, second_attachment).layout;
    auto second_packet = take(FrameData::create<Resources>(second_schema));
    const auto old_attachment = attachment;
    detach(*wire, commands, attachment);
    attachment = call<Attach>(*wire, commands, {scene, view, output, effect}).attachment;
    CHECK(attachment != old_attachment);
    CHECK(second_packet.seal());
    request<SubmitFrame>(*client, submit, {second_attachment, &second_packet});
    CHECK(second_packet.writable()); // Dense Attachment relocation retained layout and cold bindings.
    detach(*wire, commands, second_attachment);
    call<RemoveTarget>(*wire, commands, second_output);
    packet.reset(); CHECK(packet.seal());
    CHECK(request<SubmitFrame>(*client, submit, {attachment, &packet}, false).error.type == kFrameContract);
    CHECK(packet.writable());
    detach(*wire, commands, attachment);
    call<RemoveTarget>(*wire, commands, output);
    call<RemoveEffect>(*wire, commands, {effect});
    call<RemoveView>(*wire, commands, {scene, view});
    call<RemoveScene>(*wire, commands, {scene});
    server->stop(); render.join(); client->serverStopped({kOutcomeUnknown});
    CHECK(server->stats().validation_errors == 0);
    std::puts("F2: real column submission, batch apply, declaration isolation, empty update, grow while borrowed,");
    std::puts("duplicate/cancel terminal ownership, stale layout, Camera command and explicit UI upload passed.");
}
